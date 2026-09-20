#include "cq/circular_queue.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>


using QueueInt = cq::CircularQueue<int, 4, 4>;


class QueueTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
    }
};


// 8 — MaxReaders: only MaxReaders can retrieve; extra register fails.
TEST_F(QueueTest, MaxReadersLimit)
{
    cq::CircularQueue<int, 4, 1> q(std::chrono::milliseconds{1000});

    const cq::ReaderId only = q.registerReader();

    ASSERT_NE(only.value, cq::ReaderId::kInvalid);

    EXPECT_EQ(q.registerReader().value, cq::ReaderId::kInvalid);

    q.write(11);

    EXPECT_EQ(q.tryRead(only).item, 11);
}


// 9 — Bad id cannot retrieve; good id can after insert.
TEST_F(QueueTest, InvalidReaderId)
{
    QueueInt q(std::chrono::milliseconds{1000});

    EXPECT_EQ(
        q.tryRead(cq::ReaderId{99}).status,
        cq::ReadStatus::InvalidReader
    );


    const cq::ReaderId id = q.registerReader();

    ASSERT_NE(id.value, cq::ReaderId::kInvalid);

    q.write(5);

    EXPECT_EQ(q.tryRead(id).item, 5);
}


// 10 — Corrupted integrity: retrieve reports CrcError but still advances (next Empty).
TEST_F(QueueTest, CrcCorruptionOnRetrieve)
{
    QueueInt q(std::chrono::milliseconds{1000});

    const cq::ReaderId id = q.registerReader();

    ASSERT_NE(id.value, cq::ReaderId::kInvalid);


    q.write(5);

    q.corruptCrcForTest(0U);


    const auto r = q.tryRead(id);


    EXPECT_EQ(r.status, cq::ReadStatus::CrcError);

    EXPECT_EQ(r.item, 5);

    EXPECT_EQ(q.tryRead(id).status, cq::ReadStatus::Empty);
}


// 11 — Aged item: retrieve reports Expired (data present but unusable).
TEST_F(QueueTest, ExpirationOnRetrieve)
{
    QueueInt q(std::chrono::milliseconds{100});

    const cq::ReaderId id = q.registerReader();

    ASSERT_NE(id.value, cq::ReaderId::kInvalid);


    q.write(2);


    // Use the real monotonic clock now.
    std::this_thread::sleep_for(std::chrono::milliseconds{101});


    const auto r = q.tryRead(id);


    EXPECT_EQ(r.status, cq::ReadStatus::Expired);

    EXPECT_EQ(r.item, 2);
}


// 12 — size/empty track inserts (bounded); retrieve drains logically per reader.
TEST_F(QueueTest, SizeEmptySemantics)
{
    QueueInt q(std::chrono::milliseconds{1000});


    EXPECT_TRUE(q.empty());

    EXPECT_EQ(q.size(), 0U);

    EXPECT_EQ(q.capacity(), 4U);


    const cq::ReaderId id = q.registerReader();

    ASSERT_NE(id.value, cq::ReaderId::kInvalid);


    q.write(1);


    EXPECT_FALSE(q.empty());

    EXPECT_EQ(q.size(), 1U);

    EXPECT_EQ(q.tryRead(id).item, 1);



    for (int i = 0; i < 4; ++i)
    {
        q.write(i);
    }


    EXPECT_EQ(q.size(), 4U);


    q.write(4);


    EXPECT_EQ(q.size(), 4U);
}


// Smoke: insert then retrieve (black-box push analogue).
TEST_F(QueueTest, InsertThenRetrieve)
{
    QueueInt q(std::chrono::milliseconds{1000});


    const cq::ReaderId id = q.registerReader();

    ASSERT_NE(id.value, cq::ReaderId::kInvalid);


    q.write(7);


    const auto r = q.tryRead(id);


    EXPECT_EQ(r.status, cq::ReadStatus::Valid);

    EXPECT_EQ(r.item, 7);

    EXPECT_EQ(r.lostCount, 0U);
}