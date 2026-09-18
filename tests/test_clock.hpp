#ifndef CQ_TEST_CLOCK_HPP
#define CQ_TEST_CLOCK_HPP

#include <chrono>

// Controllable clock so expiration tests need no real sleeps.
struct TestClock
{
    using duration = std::chrono::milliseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<TestClock, duration>;

    static time_point now() noexcept
    {
        return time_point{duration{currentMs_}};
    }

    static void reset() noexcept
    {
        currentMs_ = 0;
    }

    static void advance(std::chrono::milliseconds delta) noexcept
    {
        currentMs_ += delta.count();
    }

private:
    static inline rep currentMs_{0};
};

#endif
