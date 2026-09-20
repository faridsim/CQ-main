#ifndef CQ_CRC32_HPP
#define CQ_CRC32_HPP

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace cq
{

/**
 * @brief Running CRC-32/IEEE accumulator for per-item integrity checks.
 *
 * Purpose: Build a digest over the logical fields of a queue item.
 * Behavior: Call update for each field, then value() for the final digest.
 * Uses a nibble-table algorithm (no dynamic memory, no byte-pointer casts).
 */
class Crc32
{
public:
    /**
     * @brief Fold one integral or enum field into the running CRC.
     *
     * Purpose: Include a single scalar member in the digest.
     * Behavior: Interprets the value as unsigned bits and hashes it
     * little-endian, one byte at a time. Does not touch object padding.
     * @tparam T Integral or enum type (enforced by static_assert).
     * @param value Field value to hash.
     * @return void
     */
    template <typename T>
    void update(T value) noexcept
    {   
        //Two identical strings could produce different CRCs beacsue of padding and implementation
        //For int, uint32_t, enum → there is a clear numeric value.
        //For string, class, struct → there is not a single obvious numeric representation

        static_assert(std::is_integral_v<T> || std::is_enum_v<T>,
                      "Crc32::update supports integral and enum types only");

        //Is T an enum type?if int,ignore
        if constexpr (std::is_enum_v<T>)
        {   
           
            using Unsigned = std::make_unsigned_t<std::underlying_type_t<T>>;

 //Find the enum's real storage type here is unit-16 beacsue alotugh type is limited but machine type order can be different
            Unsigned bits = static_cast<Unsigned>(value);
 //two dienticla forloops beacsue Yes. Unsigned can become a different datatype depending on whether T is enum or integer.
            for (std::size_t i = 0U; i < sizeof(Unsigned); ++i)
            {
                updateByte(static_cast<std::uint8_t>((bits >> (i * 8U)) & 0xFFU));
            }
        }

        else
        {
            using Unsigned = std::make_unsigned_t<T>;
            Unsigned bits = static_cast<Unsigned>(value);
            for (std::size_t i = 0U; i < sizeof(Unsigned); ++i)
            {
                updateByte(static_cast<std::uint8_t>((bits >> (i * 8U)) & 0xFFU));
            }
        }
    }

    /**
     * @brief Finalize the accumulator and obtain the CRC-32 digest.
     *
     * Purpose: Produce the stored/compared integrity value for an item.
     * Behavior: Applies the IEEE final XOR to the internal state; does not
     * reset the accumulator (construct a new Crc32 to start over).
     * @return 32-bit CRC-32/IEEE digest.
     */
    std::uint32_t value() const noexcept
    {
        return crc_ ^ 0xFFFFFFFFU;
    }

private:
    // IEEE polynomial reflected form; nibble table trades a few cycles for
    // a 16-entry constexpr table instead of the classic 256-entry one.
    static constexpr std::uint32_t kPoly = 0xEDB88320U;

    static constexpr std::uint32_t nibbleTable(std::uint32_t nibble) noexcept
    {
        std::uint32_t c = nibble;
        for (std::uint32_t bit = 0U; bit < 4U; ++bit)
        {
            if ((c & 1U) != 0U)
            {
                c = kPoly ^ (c >> 1U);
            }
            else
            {
                c = c >> 1U;
            }
        }
        return c;
    }

    void updateByte(std::uint8_t byte) noexcept
    {
        // Process low nibble then high nibble of each input byte.
        std::uint32_t index = (crc_ ^ byte) & 0x0FU;
        crc_ = nibbleTable(index) ^ (crc_ >> 4U);
        index = (crc_ ^ (byte >> 4U)) & 0x0FU;
        crc_ = nibbleTable(index) ^ (crc_ >> 4U);
    }

    // CRC-32 init value is all-ones; final XOR is applied in value().
    std::uint32_t crc_{0xFFFFFFFFU};
};

/**
 * @brief Primary template: CRC coverage is undefined for unknown types.
 *
 * Purpose: Force a specialization (or the integral/enum default) before
 * CircularQueue can be instantiated with type T.
 * Behavior: Has no accumulate(), so has_crc_traits<T> reports false and
 * computeCrc / CircularQueue reject this T at compile time.
 *
 * For struct types the user writes a full specialization of this template
 * in namespace cq, next to their struct. That specialization supplies the
 * per-type CRC policy by defining a static accumulate() function. It is
 * intentionally non-generic: C++17 has no reflection, so the library cannot
 * know which members are data vs padding vs pointers.
 *
 * @tparam T Item type lacking a known CRC mapping.
 */
template <typename T, typename = void>
struct CrcTraits
{
    // NEW: no members. The presence or absence of accumulate() is what the
    // detector below keys on. This replaces the old is_defined boolean.
};

/**
 * @brief Default CRC coverage for integral and enum item types.
 *
 * Purpose: Allow CircularQueue of integral/enum T without a user specialization.
 * Behavior: accumulate forwards the whole value to Crc32::update.
 *
 * @tparam T Integral or enum item type.
 */
template <typename T>
struct CrcTraits<T, std::enable_if_t<std::is_integral_v<T> || std::is_enum_v<T>>>
{
    /**
     * @brief Feed the entire scalar item into @p crc.
     *
     * Purpose: Define which bits of T participate in the digest.
     * Behavior: Calls crc.update(value) once.
     * @param value Item to hash.
     * @param crc   Accumulator to update.
     * @return void
     */
    static void accumulate(const T& value, Crc32& crc) noexcept
    {
        crc.update(value);
    }
};

/**
 * @brief Detector: does CrcTraits<T> provide a usable accumulate()?
 *
 * Purpose: Replace the old is_defined boolean with a structural check.
 * Behavior: true_type when CrcTraits<T>::accumulate(const T&, Crc32&) is
 * well-formed; false_type otherwise (primary template has no accumulate,
 * so unspecialized types report false).
 *
 * @tparam T Item type to probe.
 */
template <typename T, typename = void>
struct has_crc_traits : std::false_type {};

template <typename T>
struct has_crc_traits<T, std::void_t<decltype(
    CrcTraits<T>::accumulate(std::declval<const T&>(),
                             std::declval<Crc32&>()))>>
    : std::true_type {};

/**
 * @brief Compute the CRC-32 digest for an item via CrcTraits.
 *
 * Purpose: Single entry point used by the queue on write and on read verify.
 * Behavior: Instantiates a fresh Crc32, runs CrcTraits<T>::accumulate, returns
 * value(). Compile-fails unless has_crc_traits<T>::value is true.
 * @tparam T Item type with defined CrcTraits.
 * @param value Item whose digest is requested.
 * @return CRC-32/IEEE digest covering the traits-defined fields of @p value.
 */
template <typename T>
std::uint32_t computeCrc(const T& value) noexcept
{
    static_assert(has_crc_traits<T>::value,
                  "Specialize cq::CrcTraits<T> to define CRC coverage for this item type");
        
    Crc32 crc;
    CrcTraits<T>::accumulate(value, crc);
    return crc.value();
}

} // namespace cq

#endif // CQ_CRC32_HPP