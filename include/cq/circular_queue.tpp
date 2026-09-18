#ifndef CQ_CIRCULAR_QUEUE_TPP
#define CQ_CIRCULAR_QUEUE_TPP

namespace cq
{

template <typename T, std::size_t Capacity, std::size_t MaxReaders, typename Clock>
CircularQueue<T, Capacity, MaxReaders, Clock>::CircularQueue
(
    std::chrono::milliseconds expiration
)
    : expiration_(expiration)
{
}



template <typename T, std::size_t Capacity, std::size_t MaxReaders, typename Clock>
void CircularQueue<T, Capacity, MaxReaders, Clock>::write(const T& item) noexcept
{
    std::array<std::size_t, MaxReaders> notifyIds{};
    std::size_t notifyCount = 0U;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::size_t index = static_cast<std::size_t>(writeSequence_ % Capacity);
        Slot& slot = slots_[index];
        slot.value = item;
        slot.timestamp = Clock::now();
        slot.crc = computeCrc(item);
        slot.sequence = writeSequence_;
        ++writeSequence_;

        // Wake only readers that can observe the new sequence.
        for (std::size_t i = 0U; i < MaxReaders; ++i)
        {
            if (readers_[i].active && readers_[i].nextSequence < writeSequence_)
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

template <typename T, std::size_t Capacity, std::size_t MaxReaders, typename Clock>
std::optional<ReaderId> CircularQueue<T, Capacity, MaxReaders, Clock>::registerReader() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0U; i < MaxReaders; ++i)
    {
        if (!readers_[i].active)
        {
            readers_[i].active = true;
            // Catch-up from "now": prior items are intentionally invisible.
            readers_[i].nextSequence = writeSequence_;
            //stote the value of the id that has been registered and stores it in struct 
            return ReaderId{i};
        }
    }
    return std::nullopt;
}
//bring back the to the oldest seqeunce possible
template <typename T, std::size_t Capacity, std::size_t MaxReaders, typename Clock>
void CircularQueue<T, Capacity, MaxReaders, Clock>::unregisterReader(ReaderId id) noexcept
{
    if (id.value >= MaxReaders)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!readers_[id.value].active)
        {
            return;
        }
        readers_[id.value].active = false;
        readers_[id.value].nextSequence = 0U;
    }
    // Unblock a waiter on this id so it can observe InvalidReader.
    readers_[id.value].itemAdded.notify_one();
}

//only chnages the next seqeunce when it is slow read ....

template <typename T, std::size_t Capacity, std::size_t MaxReaders, typename Clock>
typename CircularQueue<T, Capacity, MaxReaders, Clock>::Result
CircularQueue<T, Capacity, MaxReaders, Clock>::readLocked(ReaderId id) noexcept
{
    Result result{};
    //outs the readers that are inactive or bad id 
    //can come from non blocking reader
    if (id.value >= MaxReaders || !readers_[id.value].active)
    {
        result.status = ReadStatus::InvalidReader;
        return result;
    }
    //next =w can come from non blocking read 
    //fast reader has nothing to catch up
    ReaderState& reader = readers_[id.value];
    if (reader.nextSequence == writeSequence_)
    {
        result.status = ReadStatus::Empty;
        return result;
    }
    //what remains active and next<w
    //computes the oldset avaible for usage in slowpath
    const std::uint64_t oldestAvailable =
        (writeSequence_ > Capacity) ? (writeSequence_ - Capacity) : 0U;

    std::uint64_t lostCount = 0U;

    //slow read.set reader.sequence to oldest available
    if (reader.nextSequence < oldestAvailable)
    {
        lostCount = oldestAvailable - reader.nextSequence;
        reader.nextSequence = oldestAvailable;
    }

    //calculating the index it should read
    const std::size_t index =
        static_cast<std::size_t>(reader.nextSequence % Capacity);

    //one specefic slot
    const Slot& slot = slots_[index];
    //value field in slot 
    result.item = slot.value;
    
    result.timestamp = slot.timestamp;
    result.lostCount = lostCount;
    ++reader.nextSequence;

    // Classify after advancing so a bad slot cannot stall the reader
    //vaue stored in memory could be corrupted so we use src
    if (computeCrc(slot.value) != slot.crc)
    {
        result.status = ReadStatus::CrcError;
    }
    //data can be valid by crc check but still expired (passed the timeout in read but not passed the clok in here)
    //The data is still physically there and was not overwritten, but it is too old to be useful anymore.
    else if ((Clock::now() - slot.timestamp) > expiration_)
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



template <typename T, std::size_t Capacity, std::size_t MaxReaders, typename Clock>
typename CircularQueue<T, Capacity, MaxReaders, Clock>::Result
CircularQueue<T, Capacity, MaxReaders, Clock>::tryRead(ReaderId id) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);

    return readLocked(id);
}

template <typename T, std::size_t Capacity, std::size_t MaxReaders, typename Clock>
typename CircularQueue<T, Capacity, MaxReaders, Clock>::Result
CircularQueue<T, Capacity, MaxReaders, Clock>::read
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
    const bool ready = reader.itemAdded.wait_for(lock, timeout, [this, id]() {
        return !readers_[id.value].active
            || readers_[id.value].nextSequence < writeSequence_;
    });
    //filters the inactive
    if (!readers_[id.value].active)
    {
        Result result{};
        result.status = ReadStatus::InvalidReader;
        return result;
    }
    //filters the time out 
    //Yes. Timeout returns Empty; reader remains active and registered
    //defined in 192
    //if not time out 
    if (!ready)
    {
        Result result{};
        result.status = ReadStatus::Empty;
        return result;
    }

    return readLocked(id);
}


template <typename T, std::size_t Capacity, std::size_t MaxReaders, typename Clock>
std::size_t CircularQueue<T, Capacity, MaxReaders, Clock>::size() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return (writeSequence_ < Capacity) ? static_cast<std::size_t>(writeSequence_)
                                       : Capacity;
}

template <typename T, std::size_t Capacity, std::size_t MaxReaders, typename Clock>
bool CircularQueue<T, Capacity, MaxReaders, Clock>::empty() const noexcept
{
    return size() == 0U;
}
//only for testing to see reader 
#ifdef CQ_ENABLE_TEST_HOOKS
template <typename T, std::size_t Capacity, std::size_t MaxReaders, typename Clock>
void CircularQueue<T, Capacity, MaxReaders, Clock>::corruptCrcForTest
(
    std::uint64_t sequence
) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t index = static_cast<std::size_t>(sequence % Capacity);
    slots_[index].crc ^= 0xFFFFFFFFU;
}
#endif

} // namespace cq

#endif // CQ_CIRCULAR_QUEUE_TPP

