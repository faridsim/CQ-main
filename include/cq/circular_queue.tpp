#ifndef CQ_CIRCULAR_QUEUE_TPP
#define CQ_CIRCULAR_QUEUE_TPP

namespace cq
{

template <typename T, std::size_t Capacity, std::size_t MaxReaders>
CircularQueue<T, Capacity, MaxReaders>::CircularQueue
(
    std::chrono::milliseconds expiration
)
    : expiration_(expiration)
{
}



template <typename T, std::size_t Capacity, std::size_t MaxReaders>
void CircularQueue<T, Capacity, MaxReaders>::write(const T& item) noexcept
{
    std::array<std::size_t, MaxReaders> notifyIds{};
    std::size_t notifyCount = 0U;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        const std::size_t index =
            static_cast<std::size_t>(writeSequence_ % Capacity);

        Slot& slot = slots_[index];

        slot.value = item;

        slot.timestamp = std::chrono::steady_clock::now();

        slot.crc = computeCrc(item);

        slot.sequence = writeSequence_;

        ++writeSequence_;


        // Wake only readers that can observe the new sequence.
        for (std::size_t i = 0U; i < MaxReaders; ++i)
        {
            if (readers_[i].active &&
                readers_[i].nextSequence < writeSequence_)
            {
                notifyIds[notifyCount] = i;
                ++notifyCount;
            }
        }
    }


    for (std::size_t i = 0U; i < notifyCount; ++i)
    {
        readers_[notifyIds[i]].itemAdded.notify_one();
    }
}



template <typename T, std::size_t Capacity, std::size_t MaxReaders>
std::optional<ReaderId>
CircularQueue<T, Capacity, MaxReaders>::registerReader() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);

    for (std::size_t i = 0U; i < MaxReaders; ++i)
    {
        if (!readers_[i].active)
        {
            readers_[i].active = true;

            // Catch-up from "now": prior items are intentionally invisible.
            readers_[i].nextSequence = writeSequence_;

            // Store the value of the id that has been registered.
            return ReaderId{i};
        }
    }

    return std::nullopt;
}



template <typename T, std::size_t Capacity, std::size_t MaxReaders>
typename CircularQueue<T, Capacity, MaxReaders>::Result
CircularQueue<T, Capacity, MaxReaders>::readLocked(ReaderId id) noexcept
{
    Result result{};


    // Out readers that are inactive or bad id.
    // Can come from non blocking reader.
    if (id.value >= MaxReaders || !readers_[id.value].active)
    {
        result.status = ReadStatus::InvalidReader;
        return result;
    }


    // nextSequence can come from non blocking read.
    // Fast reader has nothing to catch up.
    ReaderState& reader = readers_[id.value];

    if (reader.nextSequence == writeSequence_)
    {
        result.status = ReadStatus::Empty;
        return result;
    }


    // Computes the oldest available for usage in slow path.
    const std::uint64_t oldestAvailable =
        (writeSequence_ > Capacity) ? (writeSequence_ - Capacity) : 0U;


    std::uint64_t lostCount = 0U;


    // Slow read. Set reader sequence to oldest available.
    if (reader.nextSequence < oldestAvailable)
    {
        lostCount = oldestAvailable - reader.nextSequence;

        reader.nextSequence = oldestAvailable;
    }


    // Calculating the index it should read.
    const std::size_t index =
        static_cast<std::size_t>(reader.nextSequence % Capacity);


    // One specific slot.
    const Slot& slot = slots_[index];


    // Value field in slot.
    result.item = slot.value;

    result.timestamp = slot.timestamp;

    result.lostCount = lostCount;


    ++reader.nextSequence;


    // Classify after advancing so a bad slot cannot stall the reader.
    //
    // Value stored in memory could be corrupted so we use CRC.
    if (computeCrc(slot.value) != slot.crc)
    {
        result.status = ReadStatus::CrcError;
    }

    // Data can be valid by CRC check but still expired.
    // The data is still physically there and was not overwritten,
    // but it is too old to be useful anymore.
    else if ((std::chrono::steady_clock::now() - slot.timestamp)
             > expiration_)
    {
        result.status = ReadStatus::Expired;
    }

    else if (lostCount > 0U)
    {
        result.status = ReadStatus::Overwritten;
    }

    else
    {
        result.status = ReadStatus::Valid;
    }


    return result;
}
template <typename T, std::size_t Capacity, std::size_t MaxReaders>
typename CircularQueue<T, Capacity, MaxReaders>::Result
CircularQueue<T, Capacity, MaxReaders>::tryRead(ReaderId id) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);

    return readLocked(id);
}



template <typename T, std::size_t Capacity, std::size_t MaxReaders>
typename CircularQueue<T, Capacity, MaxReaders>::Result
CircularQueue<T, Capacity, MaxReaders>::read
(
    ReaderId id,
    std::chrono::milliseconds timeout
) noexcept
{
    std::unique_lock<std::mutex> lock(mutex_);


    if (id.value >= MaxReaders || !readers_[id.value].active)
    {
        Result result{};

        result.status = ReadStatus::InvalidReader;

        return result;
    }


    ReaderState& reader = readers_[id.value];


    const bool ready = reader.itemAdded.wait_for(
        lock,
        timeout,
        [this, id]()
        {
            return !readers_[id.value].active
                || readers_[id.value].nextSequence < writeSequence_;
        });


    // Filters the inactive.
    if (!readers_[id.value].active)
    {
        Result result{};

        result.status = ReadStatus::InvalidReader;

        return result;
    }


    // Filters the time out.
    //
    // Timeout returns Empty; reader remains active and registered.
    if (!ready)
    {
        Result result{};

        result.status = ReadStatus::Empty;

        return result;
    }


    return readLocked(id);
}



template <typename T, std::size_t Capacity, std::size_t MaxReaders>
std::size_t CircularQueue<T, Capacity, MaxReaders>::size() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);

    return (writeSequence_ < Capacity)
               ? static_cast<std::size_t>(writeSequence_)
               : Capacity;
}



template <typename T, std::size_t Capacity, std::size_t MaxReaders>
bool CircularQueue<T, Capacity, MaxReaders>::empty() const noexcept
{
    return size() == 0U;
}



#ifdef CQ_ENABLE_TEST_HOOKS

template <typename T, std::size_t Capacity, std::size_t MaxReaders>
void CircularQueue<T, Capacity, MaxReaders>::corruptCrcForTest
(
    std::uint64_t sequence
) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);

    const std::size_t index =
        static_cast<std::size_t>(sequence % Capacity);

    slots_[index].crc ^= 0xFFFFFFFFU;
}

#endif


} // namespace cq

#endif // CQ_CIRCULAR_QUEUE_TPP