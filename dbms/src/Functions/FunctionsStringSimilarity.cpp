#include <Functions/FunctionsStringSimilarity.h>

#include <Functions/FunctionFactory.h>
#include <Functions/FunctionsHashing.h>
#include <Common/HashTable/ClearableHashMap.h>
#include <Common/HashTable/Hash.h>
#include <Common/UTF8Helpers.h>

#include <Core/Defines.h>

#include <common/unaligned.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

#ifdef __SSE4_2__
#    include <nmmintrin.h>
#endif

namespace DB
{
/** Distance function implementation.
  * We calculate all the n-grams from left string and count by the index of
  * 16 bits hash of them in the map.
  * Then calculate all the n-grams from the right string and calculate
  * the n-gram distance on the flight by adding and subtracting from the hashmap.
  * Then return the map into the condition of which it was after the left string
  * calculation. If the right string size is big (more than 2**15 bytes),
  * the strings are not similar at all and we return 1.
  */
template <size_t N, class CodePoint, bool UTF8, bool CaseInsensitive>
struct NgramDistanceImpl
{
    using ResultType = Float32;

    /// map_size for ngram difference.
    static constexpr size_t map_size = 1u << 16;

    /// If the haystack size is bigger than this, behaviour is unspecified for this function.
    static constexpr size_t max_string_size = 1u << 15;

    /// Default padding to read safely.
    static constexpr size_t default_padding = 16;

    /// Max codepoints to store at once. 16 is for batching usage and PODArray has this padding.
    static constexpr size_t simultaneously_codepoints_num = default_padding + N - 1;

    /** This fits mostly in L2 cache all the time.
      * Actually use UInt16 as addings and subtractions do not UB overflow. But think of it as a signed
      * integer array.
      */
    using NgramStats = UInt16[map_size];

    static ALWAYS_INLINE UInt16 ASCIIHash(const CodePoint * code_points)
    {
        return intHashCRC32(unalignedLoad<UInt32>(code_points)) & 0xFFFFu;
    }

    static ALWAYS_INLINE UInt16 UTF8Hash(const CodePoint * code_points)
    {
        UInt64 combined = (static_cast<UInt64>(code_points[0]) << 32) | code_points[1];
#ifdef __SSE4_2__
        return _mm_crc32_u64(code_points[2], combined) & 0xFFFFu;
#else
        return (intHashCRC32(combined) ^ intHashCRC32(code_points[2])) & 0xFFFFu;
#endif
    }

    template <size_t Offset, class Container, size_t... I>
    static ALWAYS_INLINE inline void unrollLowering(Container & cont, const std::index_sequence<I...> &)
    {
        ((cont[Offset + I] = std::tolower(cont[Offset + I])), ...);
    }

    static ALWAYS_INLINE size_t readASCIICodePoints(CodePoint * code_points, const char *& pos, const char * end)
    {
        /// Offset before which we copy some data.
        constexpr size_t padding_offset = default_padding - N + 1;
        /// We have an array like this for ASCII (N == 4, other cases are similar)
        /// |a0|a1|a2|a3|a4|a5|a6|a7|a8|a9|a10|a11|a12|a13|a14|a15|a16|a17|a18|
        /// And we copy                                ^^^^^^^^^^^^^^^ these bytes to the start
        /// Actually it is enough to copy 3 bytes, but memcpy for 4 bytes translates into 1 instruction
        memcpy(code_points, code_points + padding_offset, roundUpToPowerOfTwoOrZero(N - 1) * sizeof(CodePoint));
        /// Now we have an array
        /// |a13|a14|a15|a16|a4|a5|a6|a7|a8|a9|a10|a11|a12|a13|a14|a15|a16|a17|a18|
        ///              ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
        /// Doing unaligned read of 16 bytes and copy them like above
        /// 16 is also chosen to do two `movups`.
        /// Such copying allow us to have 3 codepoints from the previous read to produce the 4-grams with them.
        memcpy(code_points + (N - 1), pos, default_padding * sizeof(CodePoint));

        if constexpr (CaseInsensitive)
        {
            /// We really need template lambdas with C++20 to do it inline
            unrollLowering<N - 1>(code_points, std::make_index_sequence<padding_offset>());
        }
        pos += padding_offset;
        if (pos > end)
            return default_padding - (pos - end);
        return default_padding;
    }

    static ALWAYS_INLINE size_t readUTF8CodePoints(CodePoint * code_points, const char *& pos, const char * end)
    {
        /// The same copying as described in the function above.
        memcpy(code_points, code_points + default_padding - N + 1, roundUpToPowerOfTwoOrZero(N - 1) * sizeof(CodePoint));

        size_t num = N - 1;
        while (num < default_padding && pos < end)
        {
            size_t length = UTF8::seqLength(*pos);

            if (pos + length > end)
                length = end - pos;

            CodePoint res;
            /// This is faster than just memcpy because of compiler optimizations with moving bytes.
            switch (length)
            {
                case 1:
                    res = 0;
                    memcpy(&res, pos, 1);
                    break;
                case 2:
                    res = 0;
                    memcpy(&res, pos, 2);
                    break;
                case 3:
                    res = 0;
                    memcpy(&res, pos, 3);
                    break;
                default:
                    memcpy(&res, pos, 4);
            }

            /// This is not a really true case insensitive utf8. We zero the 5-th bit of every byte.
            /// For ASCII it works https://catonmat.net/ascii-case-conversion-trick. For most cyrrilic letters also does.
            /// For others, we don't care now. Lowering UTF is not a cheap operation.
            if constexpr (CaseInsensitive)
            {
                switch (length)
                {
                    case 4:
                        res &= ~(1u << (5 + 3 * CHAR_BIT));
                        [[fallthrough]];
                    case 3:
                        res &= ~(1u << (5 + 2 * CHAR_BIT));
                        [[fallthrough]];
                    case 2:
                        res &= ~(1u << (5 + CHAR_BIT));
                        [[fallthrough]];
                    default:
                        res &= ~(1u << 5);
                }
            }

            pos += length;
            code_points[num++] = res;
        }
        return num;
    }

    static ALWAYS_INLINE inline size_t calculateNeedleStats(
        const char * data,
        const size_t size,
        NgramStats & ngram_stats,
        size_t (*read_code_points)(CodePoint *, const char *&, const char *),
        UInt16 (*hash_functor)(const CodePoint *))
    {
        // To prevent size_t overflow below.
        if (size < N)
            return 0;

        const char * start = data;
        const char * end = data + size;
        CodePoint cp[simultaneously_codepoints_num] = {};

        /// read_code_points returns the position of cp where it stopped reading codepoints.
        size_t found = read_code_points(cp, start, end);
        /// We need to start for the first time here, because first N - 1 codepoints mean nothing.
        size_t i = N - 1;
        /// Initialize with this value because for the first time `found` does not initialize first N - 1 codepoints.
        size_t len = -N + 1;
        do
        {
            len += found - N + 1;
            for (; i + N <= found; ++i)
                ++ngram_stats[hash_functor(cp + i)];
            i = 0;
        } while (start < end && (found = read_code_points(cp, start, end)));

        return len;
    }

    static ALWAYS_INLINE inline UInt64 calculateHaystackStatsAndMetric(
        const char * data,
        const size_t size,
        NgramStats & ngram_stats,
        size_t & distance,
        size_t (*read_code_points)(CodePoint *, const char *&, const char *),
        UInt16 (*hash_functor)(const CodePoint *))
    {
        size_t ngram_cnt = 0;
        const char * start = data;
        const char * end = data + size;
        CodePoint cp[simultaneously_codepoints_num] = {};

        /// allocation tricks, most strings are relatively small
        static constexpr size_t small_buffer_size = 256;
        std::unique_ptr<UInt16[]> big_buffer;
        UInt16 small_buffer[small_buffer_size];
        UInt16 * ngram_storage = small_buffer;

        if (size > small_buffer_size)
        {
            ngram_storage = new UInt16[size];
            big_buffer.reset(ngram_storage);
        }

        /// read_code_points returns the position of cp where it stopped reading codepoints.
        size_t found = read_code_points(cp, start, end);
        /// We need to start for the first time here, because first N - 1 codepoints mean nothing.
        size_t iter = N - 1;

        do
        {
            for (; iter + N <= found; ++iter)
            {
                UInt16 hash = hash_functor(cp + iter);
                if (static_cast<Int16>(ngram_stats[hash]) > 0)
                    --distance;
                else
                    ++distance;

                ngram_storage[ngram_cnt++] = hash;
                --ngram_stats[hash];
            }
            iter = 0;
        } while (start < end && (found = read_code_points(cp, start, end)));

        /// Return the state of hash map to its initial.
        for (size_t i = 0; i < ngram_cnt; ++i)
            ++ngram_stats[ngram_storage[i]];
        return ngram_cnt;
    }

    template <class Callback, class... Args>
    static inline size_t dispatchSearcher(Callback callback, Args &&... args)
    {
        if constexpr (!UTF8)
            return callback(std::forward<Args>(args)..., readASCIICodePoints, ASCIIHash);
        else
            return callback(std::forward<Args>(args)..., readUTF8CodePoints, UTF8Hash);
    }

    static void constant_constant(std::string data, std::string needle, Float32 & res)
    {
        NgramStats common_stats;
        memset(common_stats, 0, sizeof(common_stats));

        /// We use unsafe versions of getting ngrams, so I decided to use padded strings.
        const size_t needle_size = needle.size();
        const size_t data_size = data.size();
        needle.resize(needle_size + default_padding);
        data.resize(data_size + default_padding);

        size_t second_size = dispatchSearcher(calculateNeedleStats, needle.data(), needle_size, common_stats);
        size_t distance = second_size;
        if (data_size <= max_string_size)
        {
            size_t first_size = dispatchSearcher(calculateHaystackStatsAndMetric, data.data(), data_size, common_stats, distance);
            res = distance * 1.f / std::max(first_size + second_size, size_t(1));
        }
        else
        {
            res = 1.f;
        }
    }

    static void vector_constant(
        const ColumnString::Chars & data, const ColumnString::Offsets & offsets, std::string needle, PaddedPODArray<Float32> & res)
    {
        /// zeroing our map
        NgramStats common_stats;
        memset(common_stats, 0, sizeof(common_stats));

        /// We use unsafe versions of getting ngrams, so I decided to use padded_data even in needle case.
        const size_t needle_size = needle.size();
        needle.resize(needle_size + default_padding);

        const size_t needle_stats_size = dispatchSearcher(calculateNeedleStats, needle.data(), needle_size, common_stats);

        size_t distance = needle_stats_size;
        size_t prev_offset = 0;
        for (size_t i = 0; i < offsets.size(); ++i)
        {
            const UInt8 * haystack = &data[prev_offset];
            const size_t haystack_size = offsets[i] - prev_offset - 1;
            if (haystack_size <= max_string_size)
            {
                size_t haystack_stats_size = dispatchSearcher(
                    calculateHaystackStatsAndMetric, reinterpret_cast<const char *>(haystack), haystack_size, common_stats, distance);
                res[i] = distance * 1.f / std::max(haystack_stats_size + needle_stats_size, size_t(1));
            }
            else
            {
                /// if the strings are too big, we say they are completely not the same
                res[i] = 1.f;
            }
            distance = needle_stats_size;
            prev_offset = offsets[i];
        }
    }
};


struct NameNgramDistance
{
    static constexpr auto name = "ngramDistance";
};

struct NameNgramDistanceCaseInsensitive
{
    static constexpr auto name = "ngramDistanceCaseInsensitive";
};

struct NameNgramDistanceUTF8
{
    static constexpr auto name = "ngramDistanceUTF8";
};

struct NameNgramDistanceUTF8CaseInsensitive
{
    static constexpr auto name = "ngramDistanceCaseInsensitiveUTF8";
};

using FunctionNgramDistance = FunctionsStringSimilarity<NgramDistanceImpl<4, UInt8, false, false>, NameNgramDistance>;
using FunctionNgramDistanceCaseInsensitive = FunctionsStringSimilarity<NgramDistanceImpl<4, UInt8, false, true>, NameNgramDistanceCaseInsensitive>;
using FunctionNgramDistanceUTF8 = FunctionsStringSimilarity<NgramDistanceImpl<3, UInt32, true, false>, NameNgramDistanceUTF8>;
using FunctionNgramDistanceCaseInsensitiveUTF8 = FunctionsStringSimilarity<NgramDistanceImpl<3, UInt32, true, true>, NameNgramDistanceUTF8CaseInsensitive>;

void registerFunctionsStringSimilarity(FunctionFactory & factory)
{
    factory.registerFunction<FunctionNgramDistance>();
    factory.registerFunction<FunctionNgramDistanceCaseInsensitive>();
    factory.registerFunction<FunctionNgramDistanceUTF8>();
    factory.registerFunction<FunctionNgramDistanceCaseInsensitiveUTF8>();
}

}
