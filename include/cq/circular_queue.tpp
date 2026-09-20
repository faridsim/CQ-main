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


//one write wake several eligible readers
//no alloactions and no throwing ,so we make fucntion noexpect


template <typename T, std::size_t Capacity, std::size_t MaxReaders>
void CircularQueue<T, Capacity, MaxReaders>::write(const T& item) noexcept
{
    //ids of readers that should be notified 
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

        
        ////decision :it notifies when there is no lock after loop is finsihed so takes more time to get notifeid 
        //ideal timing ,only the last item in the loop
        for (std::size_t i = 0U; i < MaxReaders; ++i)
        {
            if (readers_[i].active &&
                readers_[i].nextSequence < writeSequence_)
            {   //notify count[0] becomes 2 
                //notify count[1] becomes 8
                notifyIds[notifyCount] = i;
                ++notifyCount;
            }
        }
    }

    //decision :it notifies when there is no lock after loop is finsihed so takes more time to get notifeid 
    //loops though all notify ids and call their respective readers
    for (std::size_t i = 0U; i < notifyCount; ++i)
    {
        readers_[notifyIds[i]].itemAdded.notify_one();
    }
}


//ReaderId ties a call to a registered slot; unregistered readers can't wait or be notified
//read(ReaderId id, ...) takes the struct by value 
//sentinel: ReaderId::kInvalid (== SIZE_MAX) means no free slot; no optional, no bool

template <typename T, std::size_t Capacity, std::size_t MaxReaders>
ReaderId
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

    //cant be max readers
    return ReaderId{ReaderId::kInvalid};  // invalid
}


// The notification only means "new data exists." The reader reads the
    // slot at its own cursor, which may be an older item it hasn't consumed
    // yet. So a wake-up from a fresh write can lead to reading a different,
    // older slot. That's correct — the writer doesn't dictate what each
    // reader reads; it just signals availability
    

template <typename T, std::size_t Capacity, std::size_t MaxReaders>
typename CircularQueue<T, Capacity, MaxReaders>::Result
CircularQueue<T, Capacity, MaxReaders>::readLocked(ReaderId id) noexcept
{
    Result result{};

    //Necessary — readLocked is also called by tryRead, which does no prior check.
    //active+next sequence<write seqeucene +not time out al by read fucntion but we still need it  
    if (id.value >= MaxReaders || !readers_[id.value].active)
    {
        result.status = ReadStatus::InvalidReader;
        //?????value field
        return result;
    }

   //slot with 4 capcity and nextseq of 7
   //next seq is 7 


   //when write seq was 7 means 6 was wriiten and ++,so 7 is still empty
   //still have this gaurd beacuse try-read can get it 
   //caught up readers gte deafult constructed t
    ReaderState& reader = readers_[id.value];

    if (reader.nextSequence == writeSequence_)
    {
        result.status = ReadStatus::Empty;
        return result;
    }


    // Computes the oldest available for all readers(0 for non slow readers and number for slow readers)
    //oldest=7-4=3 
    const std::uint64_t oldestAvailable =
        (writeSequence_ > Capacity) ? (writeSequence_ - Capacity) : 0U;


    std::uint64_t lostCount = 0U;
    //n<w and active ??????

    
    //reader next is 2 ,next-seq becomes 3 and lose one to read
    //readre next is 6,doest enter 
    
    if (reader.nextSequence < oldestAvailable)
    {
        lostCount = oldestAvailable - reader.nextSequence;

        reader.nextSequence = oldestAvailable;
    }

    //index is depedn on next -seq ,nextsewu is
    // Calculating the index it should read
    //for slow path becomes 4%3=1
    //for normal path becomes 
    //reader with seq-3 whcih was pervilsuty 2 ,measn 3%3=0
    //readeer with seq-6 =6%2 becomes slot 2 
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

//takes struct and then reads the "value" field 
//Yes. The caller holds a valid id; write uses that same id to notify only that reader's CV.
template <typename T, std::size_t Capacity, std::size_t MaxReaders>
typename CircularQueue<T, Capacity, MaxReaders>::Result
CircularQueue<T, Capacity, MaxReaders>::read
(
    ReaderId id,
    std::chrono::milliseconds timeout
) noexcept
{
    std::unique_lock<std::mutex> lock(mutex_);

    
    //caller can call with wrong id 
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
    //doesnt pass the id to readlcoked 
    //Defensive against future design,unregsiter
    //Filters the inactive
    if (!readers_[id.value].active)
    {
        Result result{};

        result.status = ReadStatus::InvalidReader;

        return result;
    }


    // Filters the time out
    //empty not expired
    //waiting to get notified  longer than it should 
    if (!ready)
    {
        Result result{};

        result.status = ReadStatus::Empty;

        return result;
    }


    return readLocked(id);
}

//we can have some helper fucntions
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