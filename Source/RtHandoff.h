#pragma once

#include <atomic>
#include <memory>

/**
    RtHandoff: passes objects from the message thread to the audio thread
    without the audio thread ever allocating, freeing or locking.

    The message thread send()s a new object; the audio thread acquire()s it at
    the start of a block, and hands the one it replaces back through a
    "retired" slot for the message thread to collect() (free). If that slot is
    still occupied the audio thread waits a block rather than lose anything.
    While the audio thread isn't running (prepare / release) adoptNow() swaps
    synchronously.

    Used for the riser sample and for hosted effect plug-ins.
*/
template <typename T>
class RtHandoff
{
public:
    RtHandoff() = default;

    ~RtHandoff()
    {
        // Nothing else is running by now, so both hand-over slots can be freed.
        std::unique_ptr<T> (incoming.exchange (nullptr));
        collect();
    }

    /** Message thread: queue `next` for the audio thread. Something sent
        earlier and not adopted yet is simply replaced. */
    void send (std::unique_ptr<T> next)
    {
        collect();
        std::unique_ptr<T> (incoming.exchange (next.release()));
    }

    /** Message thread: frees what the audio thread has swapped out. */
    void collect() { std::unique_ptr<T> (retired.exchange (nullptr)); }

    /** Audio thread: adopts a newly sent object (true in `adopted` if so) and
        returns the current one. */
    T* acquire (bool* adopted = nullptr) noexcept
    {
        if (retired.load() == nullptr)
            if (auto* next = incoming.exchange (nullptr))
            {
                retired.store (active.release());
                active.reset (next);
                if (adopted != nullptr)
                    *adopted = true;
            }
        return active.get();
    }

    /** Only while the audio thread isn't running: adopts a sent object now,
        freeing the replaced one here. */
    T* adoptNow()
    {
        if (auto* next = incoming.exchange (nullptr))
            active.reset (next);
        return active.get();
    }

    /** The object the audio thread is using (or would use). */
    T* current() const noexcept { return active.get(); }

    RtHandoff (const RtHandoff&) = delete;
    RtHandoff& operator= (const RtHandoff&) = delete;

private:
    std::unique_ptr<T> active;
    std::atomic<T*>    incoming { nullptr };
    std::atomic<T*>    retired  { nullptr };
};
