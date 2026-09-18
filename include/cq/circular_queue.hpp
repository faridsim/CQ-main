#ifndef CQ_CIRCULAR_QUEUE_HPP
#define CQ_CIRCULAR_QUEUE_HPP

#include "cq/crc32.hpp"
#include "cq/read_result.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <type_traits>

namespace cq
{

/**
 * @brief Fixed-capacity multi-writer / multi-reader circular queue.
 *
 * Purpose: Concurrently buffer typed items for embedded-style use with
 * independent readers, overwrite semantics, CRC integrity, and expiration.
 * Behavior: All storage is embedded in std::array members (no dynamic memory,
 * no pointers to items). Writers overwrite the oldest slot when full. Each
 * reader keeps its own nextSequence cursor. Slot index = writeSequence % Capacity.
 *
 * @tparam T          Item type: nothrow default-constructible and copy-assignable;
 *                    CrcTraits<T> must be defined (integral/enum have a default).
 * @tparam Capacity   Number of ring slots; must be greater than zero.
 * @tparam MaxReaders Maximum concurrently registered readers; must be greater than zero.
 * @tparam Clock      Time-source policy used for timestamps and expiration;
 *                    defaults to std::chrono::steady_clock.
 */
template <typename T,
          std::size_t Capacity,
          std::size_t MaxReaders,
          typename Clock = std::chrono::steady_clock>
class CircularQueue
{
public:
//invarint of the class

    static_assert(Capacity > 0U, "Capacity must be > 0");
    static_assert(MaxReaders > 0U, "MaxReaders must be > 0");
    static_assert(std::is_nothrow_default_constructible_v<T>,
                  "T must be nothrow default-constructible");
    static_assert(std::is_nothrow_copy_assignable_v<T>,
                  "T must be nothrow copy-assignable");
    static_assert(CrcTraits<T>::is_defined,
                  "Specialize cq::CrcTraits<T> for this item type");

    /** Soft upper bound on sizeof(T) * Capacity for embedded footprint checks. */
    static constexpr std::size_t kMaxQueueBytes = 256U * 1024U;
    static_assert(sizeof(T) * Capacity <= kMaxQueueBytes,
                  "static storage exceeds kMaxQueueBytes");

    /** Convenience alias for the read result type of this queue instantiation. */
    using Result = ReadResult<T, Clock>;

    /** Clock::time_point used for item timestamps. */
    using TimePoint = typename Clock::time_point;

    /**
     * @brief Construct an empty queue with a fixed item lifetime.
     *
     * Purpose: Bind the expiration interval used by every subsequent read.
     * Behavior: No items are present; writeSequence starts at zero; no readers
     * are registered. Copy and move are deleted.
     * @param expiration Maximum age an item may have when read; older items
     *                   are reported as ReadStatus::Expired. Measured with Clock::now().
     */
    explicit CircularQueue
    (
        std::chrono::milliseconds expiration
    );

    /** @brief Copy construction is disabled — the queue owns unique sync state. */
    CircularQueue(const CircularQueue&) = delete;

    /** @brief Copy assignment is disabled. */
    CircularQueue& operator=(const CircularQueue&) = delete;

    /** @brief Move construction is disabled. */
    CircularQueue(CircularQueue&&) = delete;

    /** @brief Move assignment is disabled. */
    CircularQueue& operator=(CircularQueue&&) = delete;

    /**
     * @brief Destroy the queue.
     *
     * Purpose: Release mutex / per-reader condition_variable resources.
     * Behavior: Undefined to destroy while other threads still call members.
     */
    ~CircularQueue() = default;

    /**
     * @brief Append an item to the ring, overwriting the oldest slot when full.
     *
     * Purpose: Publish one item to all registered readers.
     * Behavior: Under the mutex, stamps Clock::now(), computes CRC via
     * CrcTraits, stores at writeSequence % Capacity, increments writeSequence,
     * then notify_one on each active reader whose cursor is behind. Always
     * succeeds in O(1); never blocks on a full queue.
     * @param item Value to store (copied into the slot).
     * @return void
     */
    void write(const T& item) noexcept;

    /**
     * @brief Claim a free reader slot with an independent read cursor.
     *
     * Purpose: Allow a consumer to receive items without affecting other readers.
     * Behavior: Activates the first inactive slot. The new reader's nextSequence
     * is set to the current writeSequence, so only items written after this call
     * are visible. Does nothing to existing items or other readers.
     * @return ReaderId wrapped in std::optional on success; std::nullopt when
     *         MaxReaders slots are already active.
     */
    std::optional<ReaderId> registerReader() noexcept;

    /**
     * @brief Release a previously registered reader slot.
     *
     * Purpose: Free a MaxReaders table entry for later reuse.
     * Behavior: Marks the slot inactive, clears its cursor, and wakes that
     * reader's waiters (if any). Safe no-op when @p id is out of range or
     * already inactive.
     * @param id Handle previously returned by registerReader.
     * @return void
     */
    void unregisterReader(ReaderId id) noexcept;

    /**
     * @brief Non-blocking read for one registered reader.
     *
     * Purpose: Pull the next available item (or report why none / why bad).
     * Behavior: If work exists, advances the reader by exactly one logical slot
     * after optionally resynchronizing past overwritten sequences. Classifies
     * the delivered slot as Valid, Overwritten, CrcError, or Expired. Does not
     * wait when the reader is caught up.
     * @param id Reader handle from registerReader.
     * @return ReadResult with status Empty when caught up, InvalidReader when
     *         @p id is bad, otherwise a consumed slot and its classification.
     */
    Result tryRead(ReaderId id) noexcept;

    /**
     * @brief Blocking read that waits up to a timeout for a new item.
     *
     * Purpose: Avoid polling; sleep until this reader's CV is notified or the
     * timeout elapses.
     * Behavior: wait_for on that reader's condition_variable with predicate
     * nextSequence < writeSequence (same mutex as write; no lost wakeups). On
     * wakeup with work, behaves like tryRead. On timeout with no work, returns
     * Empty.
     * @param id      Reader handle from registerReader.
     * @param timeout Maximum time to wait for at least one new item.
     * @return ReadResult: InvalidReader if @p id is bad; Empty on timeout with
     *         nothing available; otherwise the same classification as tryRead.
     */
    Result read(ReaderId id, std::chrono::milliseconds timeout) noexcept;

    /**
     * @brief Report how many items currently occupy the ring.
     *
     * Purpose: Observe occupancy independent of any reader cursor.
     * Behavior: Returns min(writeSequence, Capacity). After Capacity writes,
     * remains Capacity because new writes overwrite rather than grow.
     * @return Occupied slot count in [0, Capacity].
     */
    std::size_t size() const noexcept;

    /**
     * @brief Test whether the ring currently holds no items.
     *
     * Purpose: Quick emptiness check for callers.
     * Behavior: Equivalent to size() == 0 (true only before the first write).
     * @return true if no items have been written yet; false otherwise.
     */
    bool empty() const noexcept;

    /**
     * @brief Compile-time capacity of this queue instantiation.
     *
     * Purpose: Expose Capacity without hard-coding it at call sites.
     * Behavior: Constant expression; no runtime work.
     * @return The Capacity template argument.
     */
    static constexpr std::size_t capacity() noexcept
    {
        return Capacity;
    }

#ifdef CQ_ENABLE_TEST_HOOKS
    /**
     * @brief Test-only: deliberately invalidate the stored CRC of one slot.
     *
     * Purpose: Force ReadStatus::CrcError in unit tests.
     * Behavior: XORs the stored CRC for sequence % Capacity. Not compiled
     * unless CQ_ENABLE_TEST_HOOKS is defined.
     * @param sequence Absolute write sequence of the slot to corrupt.
     * @return void
     */
    void corruptCrcForTest(std::uint64_t sequence) noexcept;
#endif

private:
    struct Slot
    {
        T value{};
        TimePoint timestamp{};
        std::uint32_t crc{0U};
        std::uint64_t sequence{0U};
    };

    struct ReaderState
    {
        bool active{false};
        std::uint64_t nextSequence{0U};
        std::condition_variable itemAdded{};
    };

    Result readLocked(ReaderId id) noexcept;

    std::array<Slot, Capacity> slots_{};
    std::array<ReaderState, MaxReaders> readers_{};
    std::uint64_t writeSequence_{0U};
    std::chrono::milliseconds expiration_;
    mutable std::mutex mutex_;
};

} // namespace cq

#include "cq/circular_queue.tpp"

#endif // CQ_CIRCULAR_QUEUE_HPP

