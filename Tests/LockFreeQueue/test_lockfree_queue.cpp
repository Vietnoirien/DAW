// SPDX-License-Identifier: AGPL-3.0-or-later
// LiBeDAW unit tests — Suite A: LockFreeQueue
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <thread>
#include <atomic>

#include "LockFreeQueue.h"

// ─── Named constants ─────────────────────────────────────────────────────────
constexpr int kQueueCapacity = 128;
constexpr int kProducerItems = 10000;

// ─── Trivial-copy sentinel ────────────────────────────────────────────────────
// TrackCommand is defined in MainComponent; use an equivalent trivially
// copyable POD for queue correctness tests.
struct TestItem
{
    int   id   = 0;
    float data = 0.0f;
};
static_assert(std::is_trivially_copyable_v<TestItem>,
              "TestItem must be trivially copyable to be safe in LockFreeQueue");

// ─────────────────────────────────────────────────────────────────────────────
// TEST: empty queue returns std::nullopt on pop()
// ─────────────────────────────────────────────────────────────────────────────
TEST(LockFreeQueue, EmptyQueueReturnsNullopt)
{
    LockFreeQueue<TestItem, kQueueCapacity> q;
    auto result = q.pop();
    EXPECT_FALSE(result.has_value())
        << "pop() on an empty queue must return std::nullopt";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: single push/pop round-trip preserves data
// ─────────────────────────────────────────────────────────────────────────────
TEST(LockFreeQueue, SinglePushPopPreservesData)
{
    LockFreeQueue<TestItem, kQueueCapacity> q;
    const TestItem original{42, 3.14f};
    ASSERT_TRUE(q.push(original)) << "push() on empty queue must return true";

    auto result = q.pop();
    ASSERT_TRUE(result.has_value()) << "pop() after single push must return a value";
    EXPECT_EQ(result->id,   original.id)   << "id field must survive the round-trip";
    EXPECT_NEAR(result->data, original.data, 1e-6f)
        << "data field must survive the round-trip (float exact copy)";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: N pushes followed by N pops preserves FIFO order
// ─────────────────────────────────────────────────────────────────────────────
TEST(LockFreeQueue, FifoOrderPreserved)
{
    constexpr int kN = 10;
    LockFreeQueue<TestItem, kQueueCapacity> q;

    for (int i = 0; i < kN; ++i)
        ASSERT_TRUE(q.push({i, static_cast<float>(i)}))
            << "push() must succeed while capacity is not exceeded (item " << i << ")";

    for (int i = 0; i < kN; ++i)
    {
        auto result = q.pop();
        ASSERT_TRUE(result.has_value()) << "pop() must return a value for item " << i;
        EXPECT_EQ(result->id, i) << "FIFO order violated at position " << i;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: push() returns false when queue is at capacity (no overflow)
// ─────────────────────────────────────────────────────────────────────────────
TEST(LockFreeQueue, FullQueueDropsItem)
{
    // Use a tiny capacity to make saturation easy
    LockFreeQueue<TestItem, 4> q;

    // AbstractFifo capacity is one less than the declared size,
    // so capacity 4 actually holds 3 items.
    int accepted = 0;
    for (int i = 0; i < 8; ++i)
        if (q.push({i, 0.0f})) ++accepted;

    // At least one push must have been rejected (the queue overflowed)
    EXPECT_LT(accepted, 8)
        << "At least one push() must fail once the queue is full";
    // All accepted items should be recoverable
    int popped = 0;
    while (q.pop().has_value()) ++popped;
    EXPECT_EQ(popped, accepted)
        << "Number of poppable items must equal the number of accepted pushes";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: producer/consumer two-thread correctness
// Run with -fsanitize=thread for TSAN coverage.
// ─────────────────────────────────────────────────────────────────────────────
TEST(LockFreeQueue, ProducerConsumerTwoThread)
{
    // Large capacity so the producer is unlikely to block in this test
    LockFreeQueue<TestItem, kProducerItems> q;
    std::atomic<int> consumedCount{0};

    std::thread producer([&]()
    {
        for (int i = 0; i < kProducerItems; ++i)
        {
            TestItem item{i, static_cast<float>(i)};
            // Spin until space is available (lock-free busy-wait, test only)
            while (!q.push(item)) {}
        }
    });

    std::thread consumer([&]()
    {
        int expected = 0;
        while (expected < kProducerItems)
        {
            auto item = q.pop();
            if (item.has_value())
            {
                // Verify order is preserved across threads
                EXPECT_EQ(item->id, expected)
                    << "Cross-thread FIFO order violated at item " << expected;
                ++expected;
                consumedCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(consumedCount.load(), kProducerItems)
        << "All produced items must be consumed exactly once";
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST: trivially copyable constraint check (compile-time)
// ─────────────────────────────────────────────────────────────────────────────
TEST(LockFreeQueue, TrivialCopyableConstraint)
{
    // This is a compile-time property — if it compiles, the static_assert passed.
    // We add a runtime reflection check as documentation.
    EXPECT_TRUE(std::is_trivially_copyable_v<TestItem>)
        << "Items placed in LockFreeQueue must be trivially copyable for ABI safety";
}
