#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>

constexpr inline uint64_t io_latency_us = 400; // assume each io op takes 400us
constexpr inline uint64_t beam_width = 2;
constexpr inline uint64_t ssd_channel_num = 8;

struct DetailLatency {
    double cpu_ms;
    double io_ms;
};

class ReqMetrics {
public:
    ReqMetrics(): cpu_duration(0), io_nums(0) {}
    ~ReqMetrics() = default;

    void on_cpu() __attribute__((always_inline)) {
        start_time = std::chrono::steady_clock::now();
    }

    void off_cpu() __attribute__((always_inline)) {
        auto end_time = std::chrono::steady_clock::now();
        cpu_duration += end_time - start_time;
    }

    void add_io() __attribute__((always_inline)) {
        ++io_nums;
    }

    std::chrono::nanoseconds get_cpu_duration() const {
        return cpu_duration;
    }

    uint64_t get_io_nums() const {
        return io_nums;
    }

    double get_latency_ms() const {
        double cpu_ms = std::chrono::duration<double, std::milli>(cpu_duration).count();
        double io_ms = (double(io_nums) * io_latency_us / beam_width) / 1000.0;
        return cpu_ms + io_ms;
    }

    DetailLatency get_detailed_latency() const {
        double cpu_ms = std::chrono::duration<double, std::milli>(cpu_duration).count();
        double io_ms = (double(io_nums) * io_latency_us / beam_width) / 1000.0;
        return DetailLatency{cpu_ms, io_ms};
    }

private:
    std::chrono::steady_clock::time_point start_time;
    std::chrono::nanoseconds cpu_duration;
    uint64_t io_nums;
};

class SSDChannelMetrics {
public:
    SSDChannelMetrics(): depth(0), depth_sum(0), req_count(0) {}

    void add_req() {
        auto d = ++depth;
        depth_sum += d;
        ++req_count;
    }

    void sub_req() {
        assert(depth > 0);
        --depth;
    }

    double get_avg_depth() const {
        if (req_count == 0) return 0;
        return static_cast<double>(depth_sum) / req_count;
    }

    void reset() {
        depth = 0;
        depth_sum = 0;
        req_count = 0;
    }

private:
    alignas(64)
    std::atomic_uint64_t depth;
    std::atomic_uint64_t depth_sum;
    std::atomic_uint64_t req_count;
};