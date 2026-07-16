// ring_buffer.h — Step 2: the producer-consumer log engine
//
// Multiple FUSE write() calls (producers, possibly on different kernel threads)
// push LogPackets in here. ONE background thread (consumer) drains it and
// applies the data to storage. This decouples "how fast can I accept a write"
// from "how fast can I persist it."
//
// This version uses a mutex + 2 condition variables (not lock-free).
// That's a deliberate choice: correctness first. We measure it, and if you
// want to attempt a lock-free SPSC version later, we compare the two with
// real numbers instead of assuming lock-free is automatically better.

#pragma once
#include <deque>
#include <mutex>
#include <condition_variable>
#include <string>
#include <atomic>

struct LogPacket {
    std::string path;
    std::string data;
    off_t       offset;
};

class RingBuffer {
public:
    explicit RingBuffer(size_t capacity) : capacity_(capacity) {}

    // Producer side. Blocks ONLY if the buffer is full (backpressure —
    // this is what stops a runaway producer from consuming unbounded RAM).
    void push(LogPacket pkt) {
        std::unique_lock<std::mutex> lock(mtx_);
        not_full_.wait(lock, [this] { return queue_.size() < capacity_ || stop_; });
        if (stop_) return;
        queue_.push_back(std::move(pkt));
        pushed_.fetch_add(1, std::memory_order_relaxed);
        lock.unlock();
        not_empty_.notify_one();
    }

    // Consumer side. Blocks until something is available or shutdown() is called
    // and the queue has fully drained.
    bool pop(LogPacket &out) {
        std::unique_lock<std::mutex> lock(mtx_);
        not_empty_.wait(lock, [this] { return !queue_.empty() || stop_; });
        if (queue_.empty()) return false; // stopped AND drained
        out = std::move(queue_.front());
        queue_.pop_front();
        popped_.fetch_add(1, std::memory_order_relaxed);
        lock.unlock();
        not_full_.notify_one();
        return true;
    }

    // Signals no more pushes are coming; wakes any blocked pop() once drained.
    void shutdown() {
        { std::lock_guard<std::mutex> lock(mtx_); stop_ = true; }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    size_t pushed() const { return pushed_.load(std::memory_order_relaxed); }
    size_t popped() const { return popped_.load(std::memory_order_relaxed); }

private:
    std::deque<LogPacket>  queue_;
    size_t                 capacity_;
    std::mutex              mtx_;
    std::condition_variable not_empty_, not_full_;
    bool                    stop_ = false;
    std::atomic<size_t>     pushed_{0}, popped_{0};
};
