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
 //???to fit 6 values into 8 bits
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



 //std::optional is 8 bytes more than int 
struct ReaderId {
    /** Sentinel meaning "no reader slot". Never a valid registered index. */
    static constexpr std::size_t kInvalid = static_cast<std::size_t>(-1);

    std::size_t value{kInvalid};
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
    // Value-initializes result.item to T{}. No garbage. No undefined behavior,
    // even if the caller ignores it.
    //
    // When status == Empty or status == InvalidReader, the function returns early.
    // result.item is still just T{} — a safe, deterministic default. Caller should
    // check status and ignore item.
    //
    // When status == Valid, Overwritten, CrcError, or Expired, the function runs
    // result.item = slot.value; — the real payload is copied in.
    T item{};

    /**
     * Number of sequences skipped because they had already been overwritten.
     * Orthogonal to status: may be non-zero even when status is CrcError or Expired.
     */
    std::uint64_t lostCount{0U};

    std::chrono::steady_clock::time_point timestamp{};
};


} // namespace cq

#endif // CQ_READ_RESULT_HPP