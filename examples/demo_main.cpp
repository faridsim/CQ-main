#include "cq/circular_queue.hpp"
#include "message.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

int main()
{
    cq::CircularQueue<Message, 16, 4> queue(std::chrono::milliseconds{60000});

    const cq::ReaderId reader = queue.registerReader();
    if (reader.value == cq::ReaderId::kInvalid)
    {
        std::cerr << "registerReader failed\n";
        return 1;
    }

    // One writer thread: publish 10 messages, 100 ms apart.
    std::thread writer([&]() {
        for (std::uint32_t i = 0U; i < 10U; ++i)
        {
            Message m{};
            m.id      = i;
            m.data[0] = 'm';
            m.channel = static_cast<std::uint8_t>(i % 4U);

            queue.write(m);
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    });

    // Main thread: read 10 messages.
    for (int i = 0; i < 10; ++i)
    {
        const auto r = queue.read(reader, std::chrono::milliseconds{500});
        if (r.status == cq::ReadStatus::Valid)
        {
            std::cout << "read id=" << r.item.id
                      << " ch=" << static_cast<int>(r.item.channel)
                      << '\n';
        }
    }

    writer.join();

    std::cout << "done size=" << queue.size() << '\n';
    return 0;
}