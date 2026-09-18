#ifndef CQ_TEST_TYPES_HPP
#define CQ_TEST_TYPES_HPP

#include "cq/crc32.hpp"

#include <cstdint>

struct Data
{
    std::uint32_t id{0U};
    std::uint16_t value{0U};
    std::uint8_t status{0U};
};

template <>
struct cq::CrcTraits<Data>
{
    static constexpr bool is_defined = true;

    static void accumulate(const Data& d, cq::Crc32& crc) noexcept
    {
        crc.update(d.id);
        crc.update(d.value);
        crc.update(d.status);
    }
};

#endif
