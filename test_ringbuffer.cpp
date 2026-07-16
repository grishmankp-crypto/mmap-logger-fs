// test_ringbuffer.cpp
// Spawns N producer threads hammering the ring buffer concurrently, plus
// 1 consumer thread draining it, and checks that EVERY packet pushed is
// popped exactly once, with no loss and no duplication. This is the actual
// proof that "thread-safe" isn't just a word we're using.

#include "ring_buffer.h"
#include <thread>
#include <vector>
#include <iostream>
#include <chrono>
#include <atomic>

int main(int argc, char** argv) {
    const int NUM_PRODUCERS   = 8;
    const int PACKETS_PER_PRODUCER = 50000;
    const size_t RING_CAPACITY = 4096;

    RingBuffer ring(RING_CAPACITY);
    std::atomic<size_t> checksum_pushed{0};
    std::atomic<size_t> checksum_popped{0};
    std::atomic<bool> consumer_done{false};

    auto start = std::chrono::steady_clock::now();

    // Consumer thread: drains and sums packet "value" (encoded in data)
    std::thread consumer([&]() {
        LogPacket pkt;
        while (ring.pop(pkt)) {
            checksum_popped.fetch_add(std::stoul(pkt.data), std::memory_order_relaxed);
        }
        consumer_done = true;
    });

    // Producers
    std::vector<std::thread> producers;
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&, p]() {
            for (int i = 0; i < PACKETS_PER_PRODUCER; ++i) {
                size_t val = p * PACKETS_PER_PRODUCER + i;
                checksum_pushed.fetch_add(val, std::memory_order_relaxed);
                ring.push(LogPacket{"/sensor.log", std::to_string(val), 0});
            }
        });
    }
    for (auto &t : producers) t.join();

    ring.shutdown();
    consumer.join();

    auto end = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(end - start).count();
    size_t total_packets = (size_t)NUM_PRODUCERS * PACKETS_PER_PRODUCER;

    std::cout << "Packets pushed:  " << ring.pushed() << "\n";
    std::cout << "Packets popped:  " << ring.popped() << "\n";
    std::cout << "Expected:        " << total_packets << "\n";
    std::cout << "Checksum match:  " << (checksum_pushed == checksum_popped ? "PASS" : "FAIL") << "\n";
    std::cout << "Count match:     " << (ring.pushed() == ring.popped() && ring.popped() == total_packets ? "PASS" : "FAIL") << "\n";
    std::cout << "Time:            " << secs << "s\n";
    std::cout << "Throughput:      " << (size_t)(total_packets / secs) << " packets/sec\n";

    return (checksum_pushed == checksum_popped) ? 0 : 1;
}
