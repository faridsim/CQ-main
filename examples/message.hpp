#ifndef CQ_MESSAGE_HPP
#define CQ_MESSAGE_HPP

#include "cq/crc32.hpp"

#include <cstdint>

struct Message
{
    std::uint32_t id;
    char          data[8];
    std::uint8_t  channel;
};

namespace cq
{
template <>
struct CrcTraits<Message>
{
    static void accumulate(const Message& msg, Crc32& crc) noexcept
    {
        crc.update(msg.id);

        for (char c : msg.data)
        {
            crc.update(static_cast<std::uint8_t>(c));
        }

        crc.update(msg.channel);
    }
};
} // namespace cq

#endif // CQ_MESSAGE_HPP