# Circular Queue — Design Note

## Architecture

Fixed ring of `Capacity` slots in `std::array`. A monotonic `writeSequence_`
counts every write; slot index is `writeSequence_ % Capacity`. Readers hold
an independent `nextSequence` cursor and their own `condition_variable` in a
fixed `MaxReaders` table.

## Multi-writer / multi-reader

One `std::mutex` serializes all access. Writers never block on full — they
overwrite. Readers never remove items; reading only advances that reader's
cursor. Wakeups are per-reader CV (`notify_one` by id), not a shared
`notify_all`. Chosen over lock-free: bounded critical section, simple
reasoning, no ABA, MISRA-friendlier than atomics+fences.

## Overwrite / lost items

`oldestAvailable = writeSequence_ > Capacity ? writeSequence_ - Capacity : 0`.
If `nextSequence < oldestAvailable`, `lostCount` is the gap, the cursor snaps
forward, and that call still delivers the oldest surviving item. Status is
`Overwritten` when the delivered slot is otherwise valid; `CrcError` /
`Expired` take precedence if the delivered slot is bad.

### Cap=3 walkthrough (all 7 writes + reads)

`writeSequence_` (`w`) is one counter on the queue — not a slot.
Each `slots_[i]` holds four fields: `value`, `timestamp`, `crc`, `sequence`
(`sequence` = copy of `w` at write time). Numbers below are illustrative
(`value` = 100+seq, `timestamp` ms, `crc` example hex).

```text
Capacity = 3
write # = sequence stored in slot.sequence
w after = writeSequence_ after that write
alive   = sequences still in the ring

Each box =
  value | timestamp | crc | sequence

═══════════════════════════════════════════════════════════════
START  w=0
                   ┌─────────────────┬─────────────────┬─────────────────┐
        slots_     │        0        │        1        │        2        │
                   │ empty           │ empty           │ empty           │
                   └─────────────────┴─────────────────┴─────────────────┘
  alive = [)   reader next=0 → Empty

═══════════════════════════════════════════════════════════════
write(seq0) → slot 0%3=0     w=1
                   ┌─────────────────┬─────────────────┬─────────────────┐
                   │ v=100           │ empty           │ empty           │
                   │ ts=0ms          │                 │                 │
                   │ crc=0xA0        │                 │                 │
                   │ seq=0           │                 │                 │
                   └─────────────────┴─────────────────┴─────────────────┘
  alive = [0]

write(seq1) → slot 1%3=1     w=2
                   ┌─────────────────┬─────────────────┬─────────────────┐
                   │ v=100 ts=0ms    │ v=101 ts=10ms   │ empty           │
                   │ crc=0xA0 seq=0  │ crc=0xA1 seq=1  │                 │
                   └─────────────────┴─────────────────┴─────────────────┘
  alive = [0,1]

write(seq2) → slot 2%3=2     w=3   FULL
                   ┌─────────────────┬─────────────────┬─────────────────┐
                   │ v=100 ts=0ms    │ v=101 ts=10ms   │ v=102 ts=20ms   │
                   │ crc=0xA0 seq=0  │ crc=0xA1 seq=1  │ crc=0xA2 seq=2  │
                   └─────────────────┴─────────────────┴─────────────────┘
  alive = [0,1,2]

═══════════════════════════════════════════════════════════════
OVERWRITE (oldest dies each time)

write(seq3) → slot 3%3=0     w=4   replaces seq0
                   ┌─────────────────┬─────────────────┬─────────────────┐
                   │ v=103 ts=30ms   │ v=101 ts=10ms   │ v=102 ts=20ms   │
                   │ crc=0xA3 seq=3  │ crc=0xA1 seq=1  │ crc=0xA2 seq=2  │
                   └─────────────────┴─────────────────┴─────────────────┘
  alive = [1,2,3]     gone: 0

write(seq4) → slot 4%3=1     w=5   replaces seq1
                   ┌─────────────────┬─────────────────┬─────────────────┐
                   │ v=103 ts=30ms   │ v=104 ts=40ms   │ v=102 ts=20ms   │
                   │ crc=0xA3 seq=3  │ crc=0xA4 seq=4  │ crc=0xA2 seq=2  │
                   └─────────────────┴─────────────────┴─────────────────┘
  alive = [2,3,4]     gone: 0,1

write(seq5) → slot 5%3=2     w=6   replaces seq2
                   ┌─────────────────┬─────────────────┬─────────────────┐
                   │ v=103 ts=30ms   │ v=104 ts=40ms   │ v=105 ts=50ms   │
                   │ crc=0xA3 seq=3  │ crc=0xA4 seq=4  │ crc=0xA5 seq=5  │
                   └─────────────────┴─────────────────┴─────────────────┘
  alive = [3,4,5]     gone: 0,1,2

write(seq6) → slot 6%3=0     w=7   replaces seq3
                   ┌─────────────────┬─────────────────┬─────────────────┐
                   │ v=106 ts=60ms   │ v=104 ts=40ms   │ v=105 ts=50ms   │
                   │ crc=0xA6 seq=6  │ crc=0xA4 seq=4  │ crc=0xA5 seq=5  │
                   └─────────────────┴─────────────────┴─────────────────┘
  alive = [4,5,6]     gone: 0,1,2,3
  oldestAvailable = 7-3 = 4
  next write → slot 7%3=1, replaces seq4

═══════════════════════════════════════════════════════════════
READ after 7 writes (w=7)

                   writeSequence_ = 7   (queue member, not in a slot)
                          |
                          v
                   ┌─────────────────┬─────────────────┬─────────────────┐
        slots_     │        0        │        1        │        2        │
                   │ v=106 ts=60ms   │ v=104 ts=40ms   │ v=105 ts=50ms   │
                   │ crc=0xA6 seq=6  │ crc=0xA4 seq=4  │ crc=0xA5 seq=5  │
                   └─────────────────┴─────────────────┴─────────────────┘
                       ^                 ^                 ^
                    next=6→seq6       next=4→seq4       next=5→seq5

  Fast reader  next=4: deliver slot1 (v=104,seq=4), next→5, lost=0, Valid
               next=5: deliver slot2 (v=105,seq=5), next→6
               next=6: deliver slot0 (v=106,seq=6), next→7
               next=7: Empty  (caught up with w)

  Slow reader  next=0: 0 < oldest(4)
               snap next→4, lostCount=4  (missed seq0..3)
               deliver slot1 (v=104,seq=4), next→5
               status = Overwritten
```

## Notification

Each `ReaderState` owns a `condition_variable`. After `write`, only active
readers with `nextSequence < writeSequence_` get `notify_one` (by reader id /
slot index). `read(timeout)` waits on **that** reader's CV under the shared
mutex — no lost wakeups, no cross-reader wake. `tryRead` never waits.
`unregisterReader` notifies that reader's CV so a blocked `read` can exit.

## CRC

Trait-based `CrcTraits<T>` feeds defined members into CRC-32/IEEE. No
`reinterpret_cast`, no padding bytes. Integral/enum types get a default;
structs specialize once.

## Timestamp / expiration

`Clock` is a template policy (default `steady_clock`). Write stamps
`Clock::now()`. Read reports `Expired` when `now - timestamp > expiration`.
Tests inject `TestClock`.

## New-reader policy

`registerReader` starts at the current `writeSequence_` — only future items.

## Limitations

- Slow readers lose data by design (overwrite).
- Shared mutex still serializes all access; only wakeups are per-reader.
- `uint64_t` sequence assumed not to wrap in product lifetime.

## MISRA C++:2023 deviations

| Area | Why |
|------|-----|
| `<mutex>`, `<condition_variable>`, `<chrono>`, `<optional>` | Required for the chosen portable sync/time model |
| Class templates / `.tpp` | Capacity and type must be compile-time |
| `CQ_ENABLE_TEST_HOOKS` CRC corruptor | Test-only; not in production builds |
