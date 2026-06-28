#pragma once
#include <JuceHeader.h>

// Lock-free SPSC queue backed by juce::AbstractFifo.
// Safe to push from one thread and pop from another without locking.
// Capacity is fixed at construction — no reallocation.
template <typename T, int Capacity = 512>
class LockFreeQueue
{
public:
    LockFreeQueue() : fifo_(Capacity) {}

    // Returns true if item was pushed successfully
    bool push(const T& item)
    {
        const auto scope = fifo_.write(1);
        if (scope.blockSize1 == 0)
            return false;  // queue full
        buffer_[scope.startIndex1] = item;
        return true;
    }

    // Returns true and sets item if queue is non-empty
    bool pop(T& item)
    {
        const auto scope = fifo_.read(1);
        if (scope.blockSize1 == 0)
            return false;  // queue empty
        item = buffer_[scope.startIndex1];
        return true;
    }

    bool isEmpty() const { return fifo_.getNumReady() == 0; }
    int  size()    const { return fifo_.getNumReady(); }

private:
    juce::AbstractFifo fifo_;
    std::array<T, Capacity> buffer_;

    JUCE_DECLARE_NON_COPYABLE(LockFreeQueue)
};
