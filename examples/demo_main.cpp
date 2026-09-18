#include "cq/circular_queue.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

int main()
{
    cq::CircularQueue<int, 32, 4> queue(std::chrono::milliseconds{60000});
    std::atomic<bool> running{true};
    // shared cout lock; drop if log interleaving is fine
    std::mutex logMutex;

    std::optional<cq::ReaderId> r0 = queue.registerReader();
    std::optional<cq::ReaderId> r1 = queue.registerReader();
    if (!r0 || !r1)
    {
        std::cerr << "registerReader failed\n";
        return 1;
    }

    auto writer = [&](int id) {
        int n = 0;
        while (running.load())
        {
            int value = id * 1000 + n;
            queue.write(value);
            {
                std::lock_guard<std::mutex> lock(logMutex);
                std::cout << "W" << id << " write " << value << '\n';
            }
            ++n;
            std::this_thread::sleep_for(std::chrono::milliseconds{80 + id * 20});
        }
    };

    auto reader = [&](cq::ReaderId id, const char* name) {
        while (running.load())
        {
            cq::CircularQueue<int, 32, 4>::Result result =
                queue.read(id, std::chrono::milliseconds{200});
            if (result.status == cq::ReadStatus::Empty)
            {
                continue;
            }
            std::lock_guard<std::mutex> lock(logMutex);
            std::cout << name << " status=" << static_cast<int>(result.status)
                      << " item=" << result.item
                      << " lost=" << result.lostCount << '\n';
        }
        for (;;)
        {
            cq::CircularQueue<int, 32, 4>::Result result = queue.tryRead(id);
            if (result.status == cq::ReadStatus::Empty)
            {
                break;
            }
            std::lock_guard<std::mutex> lock(logMutex);
            std::cout << name << " drain item=" << result.item << '\n';
        }
    };

    std::vector<std::thread> threads;
    threads.emplace_back(writer, 0);
    threads.emplace_back(writer, 1);
    threads.emplace_back(writer, 2);
    threads.emplace_back(reader, *r0, "R0");
    threads.emplace_back(reader, *r1, "R1");

    std::this_thread::sleep_for(std::chrono::seconds{3});
    running.store(false);
    for (std::thread& t : threads)
    {
        t.join();
    }

    queue.unregisterReader(*r0);
    queue.unregisterReader(*r1);
    std::cout << "done size=" << queue.size() << '\n';
    return 0;
}
