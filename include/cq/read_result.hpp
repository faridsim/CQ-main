#ifndef CQ_READ_RESULT_HPP
#define CQ_READ_RESULT_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace cq
{

/**
 * @brief Outcome of a single read attempt against the circular queue.
 *
 * Purpose: Classify what a reader received (or why nothing was delivered).
 * Behavior: Returned inside ReadResult by tryRead / read. Integrity and
 * lifetime faults (CrcError, Expired) outrank Overwritten when the delivered
 * slot is bad; lostCount still reports skipped overwritten sequences.
 */
enum class ReadStatus : std::uint8_t
{
    /** Item passed CRC and expiration checks; lostCount is zero. */
    Valid = 0,
    /** Stored CRC does not match a fresh computeCrc of the slot contents. */
    CrcError,
    /** Item age exceeds the queue expiration interval. */
    Expired,
    /** One or more older items were overwritten; delivered item is otherwise valid. */
    Overwritten,
    /** Reader is caught up; no item available (or blocking read timed out). */
    Empty,
    /** ReaderId is out of range or not currently registered. */
    InvalidReader
};

/**
 * @brief Opaque handle identifying one registered reader.
 *
 * Purpose: Address a reader slot without pointers or heap objects.
 * Behavior: Obtained from CircularQueue::registerReader; pass to read APIs.
 * The numeric value is an index into the queue's fixed reader table.
 */
struct ReaderId
{
    /** Zero-based index of the reader slot inside the queue. */
    std::size_t value{0U};
};

/**
 * @brief Complete outcome of one tryRead / read call.
 *
 * Purpose: Return status, payload, overwrite gap, and write-time stamp together.
 * Behavior: Always returned by value. Fields item, timestamp, and lostCount are
 * meaningful for Valid, CrcError, Expired, and Overwritten. For Empty and
 * InvalidReader only status is meaningful.
 *
 * @tparam T     Item type copied from the queue slot.
 * @tparam Clock Clock type whose time_point is stored in timestamp.
 */
template <typename T, typename Clock = std::chrono::steady_clock>
struct ReadResult
{
    /** Classification of this read attempt. */
    ReadStatus status{ReadStatus::Empty};

    /** Copy of the slot contents when a slot was consumed; otherwise default. */
    T item{};

    /**
     * Number of sequences skipped because they had already been overwritten.
     * Orthogonal to status: may be non-zero even when status is CrcError or Expired.
     */
    std::uint64_t lostCount{0U};

    /** Clock::now() value recorded when the item was written; default if none. */
    typename Clock::time_point timestamp{};
};

} // namespace cq

#endif // CQ_READ_RESULT_HPP
