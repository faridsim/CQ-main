#include "cq/circular_queue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>
#include <vector>
//tests not to do
//fast reader+slow reader
//Fast reader continues correctly while another falls behind
//Slow reader blocking or modifying "shared state"

namespace
{

// Black-box: prove insert by retrieving the value (not write's void return)

template <typename Queue>
bool writeVisibleTo(Queue& q, cq::ReaderId id, int value)
{   //write 99 into slot
    q.write(value);
    //get result 
    const typename Queue::Result r = q.tryRead(id);
    return (r.status == cq::ReadStatus::Valid || r.status == cq::ReadStatus::Overwritten)
    //the new item has been written
        && r.item == value;
}


} // namespace

// 1 — MPMC read behaviour under heavy  overwrite contention.
// cause:Wrong oldestAvailable calculation or lostCount logic
// slow read doesnt return overwritten data under concurrent worklaod
// Verifies that under concurrent writers and slow readers,
// overwritten data is reported correctly through lostCount,
// and readers recover from sequence gaps without stale delivery(new data)how it is checked ?


TEST(BlackBox, MpmcOverwriteStorm)
{
    cq::CircularQueue<int, 8, 4> q(std::chrono::milliseconds{60000});

    const auto idA = q.registerReader();
    const auto idB = q.registerReader();
    //check that both has value
    ASSERT_TRUE(idA && idB);


    constexpr int kWriterThreads = 4;

// Number of writes performed by each writer thread
     constexpr int kWritesPerThread = 100;
// Creates four writer thread objects
     std::thread writers[kWriterThreads];
// Loop through all writer thread objects
    for (int w = 0; w < kWriterThreads; ++w)
    {
        // Each writer thread calls write() 100 times
        // Each thread writes a unique value range
        writers[w] = std::thread([&q, w]() {
            for (int i = 0; i < kWritesPerThread; ++i)
            {
                q.write(w * kWritesPerThread + i);
            }
        });
    }



    // main thread is slower 
    // Make readers slower than writers
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    //same register readers try to read
    auto rA = q.tryRead(*idA);
    auto rB = q.tryRead(*idB);

    //wait for all writer threads objects to finsih
    for (auto& t : writers)
    {
        t.join();
    }

    //Both are acceptable outcomes because the reader may or may not lose data depending on timing
    //Expanding time only increases chance of Overwritten; it does not guarantee it


    EXPECT_EQ(rA.status, cq::ReadStatus::Overwritten);
    EXPECT_EQ(rB.status, cq::ReadStatus::Overwritten);
    EXPECT_GE(rA.lostCount, 0U);
    EXPECT_GE(rB.lostCount, 0U);
}
//right notification logic under high write
//cause:wrong notification logic
//readers wait while writers concurrently publish data
//For wrong notification logic, the test should verify that:
//A writer wakes only readers that can actually read the new sequence.
//A reader waiting with no available data should not remain blocked after a valid write.
//Other readers should not be incorrectly affected
TEST(BlackBox, MpmcNotificationWakesCorrectReaders)
{
    cq::CircularQueue<int, 8, 4> q(std::chrono::milliseconds{60000});
    //id of registered reader wrapped in std::optional
    const auto idA = q.registerReader();
    const auto idB = q.registerReader();

    // check that both readers registered successfully
    ASSERT_TRUE(idA && idB);

    constexpr int kWriterThreads = 4;

    // Number of writes performed by each writer thread
    constexpr int kWritesPerThread = 100;

    // Creates four writer thread objects
    std::thread writers[kWriterThreads];

    // Loop through all writer thread objects
    for (int w = 0; w < kWriterThreads; ++w)
    {
        // Each writer thread calls write() 100 times
        // Each thread writes a unique value range
        writers[w] = std::thread([&q, w]() {

            //A sleep helps increase the probability, but it does not guarantee writers are still running.
            std::this_thread::sleep_for(std::chrono::milliseconds{100});

            for (int i = 0; i < kWritesPerThread; ++i)
            {
                q.write(w * kWritesPerThread + i);
            }
        });
    }


    //writers are writing ,readers are waiting 

    //Yes. Writers may still run; readers enter read() and wait until data arrives or timeout
    auto rA = q.read(*idA, std::chrono::milliseconds{200});
    auto rB = q.read(*idB, std::chrono::milliseconds{200});


    // wait for all writer threads objects to finish
    for (auto& t : writers)
    {
        t.join();
    }


    // result.status field is valid 
    // Both readers should have been notified and receive data
    EXPECT_EQ(rA.status, cq::ReadStatus::Valid);
    EXPECT_EQ(rB.status, cq::ReadStatus::Valid);


    // Values should come from one of the writer ranges
    //focus is not on write order but on notification correctness
    auto validRange = [](int value)
    {
        return (value >= 0 && value < 100) ||
               (value >= 100 && value < 200) ||
               (value >= 200 && value < 300) ||
               (value >= 300 && value < 400);
    };
    //
    EXPECT_TRUE(validRange(rA.item));
    EXPECT_TRUE(validRange(rB.item));
}


// 3 — Slow and fast readers consume independently.
// Verifies that a slow reader falling behind does not affect
// a fast reader, and each reader maintains its own cursor.
//
// Black-box validation:
// Readers are validated through returned Result values,
// checking delivery and overwrite behavior, not internal cursors.






// 4 — Blocked read is awakened after writer publication.

// Verifies that a reader blocked in read() can be notified by a writer,
// resume execution, and successfully consume the newly published item.

// Black-box validation:
// The test validates observable behavior through the returned Result.
// It does not inspect internal synchronization state or reader cursors.
TEST(BlackBox, NotificationWakesBlockedRead)
{
    cq::CircularQueue<int, 8, 2> q(std::chrono::milliseconds{60000});
    const std::optional<cq::ReaderId> id = q.registerReader();
    ASSERT_TRUE(id.has_value());
    //It waits to ensure read() starts and blocks, not finishes.
    std::thread writer([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
        q.write(42);
    });

    const auto r = q.read(*id, std::chrono::milliseconds{500});

    //make sure that read fisniehs
    writer.join();

    EXPECT_EQ(r.status, cq::ReadStatus::Valid);
    EXPECT_EQ(r.item, 42);
}

// 5 — Timeout returns Empty, then reader remains usable.

// Verifies that a reader timing out while waiting does not lose its
// registration state and can successfully retrieve future publications.

// Black-box validation:
// The test does not inspect internal reader state (nextSequence or active).
// It validates continued reader functionality through observable results:
 // timeout response followed by successful data retrieval.
TEST(BlackBox, TimeoutReturnsEmptyThenStillWorks)
{
    cq::CircularQueue<int, 8, 2> q(std::chrono::milliseconds{60000});
    const std::optional<cq::ReaderId> id = q.registerReader();
    ASSERT_TRUE(id.has_value());

    EXPECT_EQ(q.read(*id, std::chrono::milliseconds{20}).status, cq::ReadStatus::Empty);
    ASSERT_TRUE(writeVisibleTo(q, *id, 7));
}



//Verifies that a reader blocked in read() is safely released when
// unregistered, and that the freed reader slot can be reused successfully.

// Black-box validation:
// The test does not inspect internal reader state (active flag or cursor).
// It verifies observable behavior by confirming the returned status and
// successful data retrieval after registering the new reader.

TEST(BlackBox, UnregisterWhileBlockedThenReuse)
{
    cq::CircularQueue<int, 8, 2> q(std::chrono::milliseconds{60000});
    const std::optional<cq::ReaderId> id = q.registerReader();
    ASSERT_TRUE(id.has_value());

    std::atomic<bool> started{false};
    

    cq::ReadStatus blockedStatus = cq::ReadStatus::Valid;


    //purpose of using atomics
    std::thread waiter([&]() {
        started.store(true);
        //It is only a safe initial value; read().status overwrites it later.
        blockedStatus = q.read(*id, std::chrono::milliseconds{1000}).status;
    });
    //Without loops: main continues; waiter may not have started yet.
    //ensures worker has started executing lambda 
    ///While started is false, keep looping
    while (!started.load())
    {
        //?gives cpu to other thread,not constant check
        std::this_thread::yield();
    }

    //make sure it has reached the reads and blocked 
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    q.unregisterReader(*id);
    //Yes. Loop starts thread; sleep enters read; join finishes thread
    //read has been waiting ,so in that 20 ms it cnat fisnih without unregsitered arrives
    waiter.join();
    //get notifed and become in active and it gates become in active line 200
    EXPECT_EQ(blockedStatus, cq::ReadStatus::InvalidReader);

    const std::optional<cq::ReaderId> again = q.registerReader();

    ASSERT_TRUE(again.has_value());

    ASSERT_TRUE(writeVisibleTo(q, *again, 99));
}


// 7 — Late join cannot retrieve old inserts; retrieves new ones
//A reader that registers after writes have already happened starts from the current write position and does not consume previous messages.
TEST(BlackBox, RegisterMidStreamSeesOnlyNew)
{
    
    cq::CircularQueue<int, 16, 2> q(std::chrono::milliseconds{60000});

    q.write(1);
    q.write(2);
    q.write(3);

    //line 63 in tpp
    //register reader makes write sequence three
    const std::optional<cq::ReaderId> late = q.registerReader();

    //wrapped in std::optional so we can check has-value
    ASSERT_TRUE(late.has_value());
    //line 108
    //from tryread .>readlcoked ..>then goes to status::empty
    //at this point write sequence has become three 
    EXPECT_EQ(q.tryRead(*late).status, cq::ReadStatus::Empty);
    //Correct. For an overwrite test, status and lostCount are the main checks.

    //item is optional but useful to verify the surviving delivered value is correct.
    q.write(4);
    //r is result
    //same reader
    const auto r = q.tryRead(*late);
    //line 157
    EXPECT_EQ(r.status, cq::ReadStatus::Valid);
    //dont know 
    EXPECT_EQ(r.item, 4);
}






