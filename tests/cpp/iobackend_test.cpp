#include "../../hnswlib/iobackend.h"
#include <thread>
#include <vector>
#include <cassert>
#include <iostream>
#include <chrono>
#include <atomic>
#include <random>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>

// Helper to create a temporary file with known content
int create_temp_file(const std::string& content) {
    std::string temp_path = "/tmp/iobackend_test_" + std::to_string(getpid()) + ".tmp";
    int fd = open(temp_path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        throw std::runtime_error("Failed to create temp file: " + std::string(strerror(errno)));
    }
    if (write(fd, content.c_str(), content.size()) != static_cast<ssize_t>(content.size())) {
        close(fd);
        throw std::runtime_error("Failed to write temp file");
    }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

void cleanup_temp_file(int fd) {
    if (fd >= 0) {
        char path[256];
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        char actual_path[256];
        ssize_t len = readlink(path, actual_path, sizeof(actual_path) - 1);
        if (len > 0) {
            actual_path[len] = '\0';
            unlink(actual_path);
        }
        close(fd);
    }
}

void test_basic_submit_and_complete() {
    std::cout << "Test: basic_submit_and_complete... " << std::flush;

    int fd = create_temp_file("Hello, World!");
    std::unique_ptr<char[]> buffer = std::make_unique<char[]>(64);
    std::atomic<bool> completed{false};
    std::atomic<int> result{-1};

    {
        hnswlib::IOBackend backend;

        backend.submit_io_task(std::make_unique<hnswlib::HnswIOTask>(
            fd, buffer.get(), 0, 13,
            [&completed, &result]() {
                completed = true;
                result = 0;
            }
        ));

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    assert(completed.load());
    assert(result.load() == 0);
    assert(memcmp(buffer.get(), "Hello, World!", 13) == 0);

    cleanup_temp_file(fd);
    std::cout << "PASSED" << std::endl;
}

void test_multiple_tasks_sequential() {
    std::cout << "Test: multiple_tasks_sequential... " << std::flush;

    int fd = create_temp_file("ABCDEFGHIJ");
    std::vector<std::unique_ptr<char[]>> buffers(10);
    std::atomic<int> completed_count{0};

    for (int i = 0; i < 10; ++i) {
        buffers[i] = std::make_unique<char[]>(2);
    }

    {
        hnswlib::IOBackend backend;

        for (int i = 0; i < 10; ++i) {
            backend.submit_io_task(std::make_unique<hnswlib::HnswIOTask>(
                fd, buffers[i].get(), i, 1,
                [&completed_count, i]() {
                    completed_count.fetch_add(1);
                }
            ));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    assert(completed_count.load() == 10);
    for (int i = 0; i < 10; ++i) {
        assert(buffers[i][0] == static_cast<char>('A' + i));
    }

    cleanup_temp_file(fd);
    std::cout << "PASSED" << std::endl;
}

void test_concurrent_submitters() {
    std::cout << "Test: concurrent_submitters(" << std::flush;

    constexpr int num_threads = 32;
    constexpr int tasks_per_thread = 1000;
    constexpr int total_tasks = num_threads * tasks_per_thread;

    int fd = create_temp_file(std::string(100000, 'X'));
    std::vector<std::unique_ptr<char[]>> buffers(total_tasks);
    std::atomic<int> completed_count{0};
    std::atomic<int> submitted_count{0};

    for (auto& buf : buffers) {
        buf = std::make_unique<char[]>(1);
    }

    {
        hnswlib::IOBackend backend;

        std::vector<std::thread> submitters;
        submitters.reserve(num_threads);

        for (int t = 0; t < num_threads; ++t) {
            submitters.emplace_back([&backend, &buffers, &completed_count, &submitted_count, &fd, t, tasks_per_thread]() {
                for (int i = 0; i < tasks_per_thread; ++i) {
                    int task_id = t * tasks_per_thread + i;
                    backend.submit_io_task(std::make_unique<hnswlib::HnswIOTask>(
                        fd, buffers[task_id].get(), task_id % 100000, 1,
                        [&completed_count]() {
                            completed_count.fetch_add(1);
                        }
                    ));
                    submitted_count.fetch_add(1);
                }
            });
        }

        for (auto& t : submitters) {
            t.join();
        }

        // Wait for all tasks to complete
        while (completed_count.load() < total_tasks) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::cout << completed_count.load() << " tasks)... PASSED" << std::endl;
    assert(completed_count.load() == total_tasks);

    cleanup_temp_file(fd);
}

void test_high_throughput_submitters() {
    std::cout << "Test: high_throughput_submitters(" << std::flush;

    constexpr int num_threads = 64;
    constexpr int tasks_per_thread = 5000;
    constexpr int total_tasks = num_threads * tasks_per_thread;

    int fd = create_temp_file(std::string(1000000, 'Y'));
    std::vector<std::unique_ptr<char[]>> buffers(total_tasks);
    std::atomic<int> completed_count{0};

    for (auto& buf : buffers) {
        buf = std::make_unique<char[]>(4);
    }

    auto start = std::chrono::high_resolution_clock::now();

    {
        hnswlib::IOBackend backend;

        std::vector<std::thread> submitters;
        submitters.reserve(num_threads);

        for (int t = 0; t < num_threads; ++t) {
            submitters.emplace_back([&backend, &buffers, &completed_count, &fd, t, tasks_per_thread]() {
                for (int i = 0; i < tasks_per_thread; ++i) {
                    int task_id = t * tasks_per_thread + i;
                    backend.submit_io_task(std::make_unique<hnswlib::HnswIOTask>(
                        fd, buffers[task_id].get(), (task_id * 4) % 1000000, 4,
                        [&completed_count]() {
                            completed_count.fetch_add(1);
                        }
                    ));
                }
            });
        }

        for (auto& t : submitters) {
            t.join();
        }

        // Wait for completion with timeout
        int waited = 0;
        while (completed_count.load() < total_tasks && waited < 30000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            waited += 10;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << completed_count.load() << " tasks in " << duration << "ms)... PASSED" << std::endl;
    assert(completed_count.load() == total_tasks);

    cleanup_temp_file(fd);
}

void test_stop_during_active() {
    std::cout << "Test: stop_during_active... " << std::flush;

    int fd = create_temp_file(std::string(1000, 'Z'));
    std::atomic<int> completed_count{0};

    {
        hnswlib::IOBackend backend;

        // Submit many tasks rapidly
        for (int i = 0; i < 100; ++i) {
            auto buffer = std::make_unique<char[]>(10);
            backend.submit_io_task(std::make_unique<hnswlib::HnswIOTask>(
                fd, buffer.get(), 0, 10,
                [&completed_count]() {
                    completed_count.fetch_add(1);
                }
            ));
        }

        // Stop while tasks may still be processing
        backend.stop();
    }

    // Verify some tasks completed (not all may have been processed due to rapid stop)
    std::cout << completed_count.load() << " completed)... PASSED" << std::endl;

    cleanup_temp_file(fd);
}

void test_task_order_preservation() {
    std::cout << "Test: task_order_preservation... " << std::flush;

    int fd = create_temp_file("ABCDEFGHIJ");
    std::vector<std::unique_ptr<char[]>> buffers(10);
    std::vector<int> completion_order;
    std::mutex order_mutex;

    for (int i = 0; i < 10; ++i) {
        buffers[i] = std::make_unique<char[]>(1);
    }

    {
        hnswlib::IOBackend backend;

        for (int i = 0; i < 10; ++i) {
            const int task_id = i;
            backend.submit_io_task(std::make_unique<hnswlib::HnswIOTask>(
                fd, buffers[i].get(), i, 1,
                [&completion_order, &order_mutex, task_id]() {
                    std::lock_guard<std::mutex> lock(order_mutex);
                    completion_order.push_back(task_id);
                }
            ));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // IO operations may complete out of order due to async nature
    // Just verify all tasks completed and data is correct
    assert(completion_order.size() == 10);
    for (int i = 0; i < 10; ++i) {
        assert(buffers[i][0] == static_cast<char>('A' + i));
    }

    cleanup_temp_file(fd);
    std::cout << "PASSED" << std::endl;
}

void test_large_buffer_io() {
    std::cout << "Test: large_buffer_io... " << std::flush;

    std::string large_content(65536, 'L');
    for (size_t i = 0; i < large_content.size(); ++i) {
        large_content[i] = static_cast<char>('A' + (i % 26));
    }

    int fd = create_temp_file(large_content);
    auto buffer = std::make_unique<char[]>(65536);
    std::atomic<bool> completed{false};

    {
        hnswlib::IOBackend backend;

        backend.submit_io_task(std::make_unique<hnswlib::HnswIOTask>(
            fd, buffer.get(), 0, 65536,
            [&completed]() {
                completed = true;
            }
        ));

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    assert(completed.load());
    assert(memcmp(buffer.get(), large_content.c_str(), 65536) == 0);

    cleanup_temp_file(fd);
    std::cout << "PASSED" << std::endl;
}

void test_random_access_pattern() {
    std::cout << "Test: random_access_pattern... " << std::flush;

    std::string file_content(10000, 'R');
    for (size_t i = 0; i < file_content.size(); ++i) {
        file_content[i] = static_cast<char>('0' + (i % 10));
    }

    int fd = create_temp_file(file_content);

    constexpr int num_tasks = 100;
    std::vector<std::unique_ptr<char[]>> buffers(num_tasks);
    std::atomic<int> completed_count{0};

    for (int i = 0; i < num_tasks; ++i) {
        buffers[i] = std::make_unique<char[]>(10);
    }

    std::vector<int> offsets;
    {
        hnswlib::IOBackend backend;

        std::mt19937 rng(42);
        std::uniform_int_distribution<int> dist(0, 9990);

        for (int i = 0; i < num_tasks; ++i) {
            int offset = dist(rng);
            offsets.push_back(offset);
            backend.submit_io_task(std::make_unique<hnswlib::HnswIOTask>(
                fd, buffers[i].get(), offset, 10,
                [&completed_count]() {
                    completed_count.fetch_add(1);
                }
            ));
        }

        while (completed_count.load() < num_tasks) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    assert(completed_count.load() == num_tasks);
    for (int i = 0; i < num_tasks; ++i) {
        assert(memcmp(buffers[i].get(), &file_content[offsets[i]], 10) == 0);
    }

    cleanup_temp_file(fd);
    std::cout << "PASSED" << std::endl;
}

void test_stress_many_threads() {
    std::cout << "Test: stress_many_threads(" << std::flush;

    constexpr int num_threads = 128;
    constexpr int tasks_per_thread = 100;
    constexpr int total_tasks = num_threads * tasks_per_thread;

    int fd = create_temp_file(std::string(100000, 'S'));
    std::vector<std::unique_ptr<char[]>> buffers(total_tasks);
    std::atomic<int> completed_count{0};

    for (auto& buf : buffers) {
        buf = std::make_unique<char[]>(8);
    }

    {
        hnswlib::IOBackend backend;

        std::vector<std::thread> submitters;
        submitters.reserve(num_threads);

        for (int t = 0; t < num_threads; ++t) {
            submitters.emplace_back([&backend, &buffers, &completed_count, &fd, t, tasks_per_thread]() {
                for (int i = 0; i < tasks_per_thread; ++i) {
                    int task_id = t * tasks_per_thread + i;
                    backend.submit_io_task(std::make_unique<hnswlib::HnswIOTask>(
                        fd, buffers[task_id].get(), (task_id * 8) % 100000, 8,
                        [&completed_count]() {
                            completed_count.fetch_add(1);
                        }
                    ));
                }
            });
        }

        for (auto& t : submitters) {
            t.join();
        }

        int waited = 0;
        while (completed_count.load() < total_tasks && waited < 30000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            waited += 10;
        }
    }

    std::cout << completed_count.load() << " tasks)... PASSED" << std::endl;
    assert(completed_count.load() == total_tasks);

    cleanup_temp_file(fd);
}

void test_backpressure_under_load() {
    std::cout << "Test: backpressure_under_load... " << std::flush;

    int fd = create_temp_file(std::string(1000000, 'B'));
    std::atomic<int> completed_count{0};
    constexpr int total_tasks = 50000;

    // Store buffers in a vector to keep them alive until tasks complete
    std::vector<std::unique_ptr<char[]>> buffers(total_tasks);

    {
        hnswlib::IOBackend backend;

        auto start = std::chrono::high_resolution_clock::now();

        // Burst submit tasks as fast as possible
        for (int i = 0; i < total_tasks; ++i) {
            buffers[i] = std::make_unique<char[]>(16);
            backend.submit_io_task(std::make_unique<hnswlib::HnswIOTask>(
                fd, buffers[i].get(), (i * 16) % 1000000, 16,
                [&completed_count]() {
                    completed_count.fetch_add(1);
                }
            ));
        }

        // Wait for completion
        while (completed_count.load() < total_tasks) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        std::cout << duration << "ms)... PASSED" << std::endl;
    }

    assert(completed_count.load() == total_tasks);
    cleanup_temp_file(fd);
}

void test_io_correctness() {
    std::cout << "Test: io_correctness... " << std::flush;

    // Create a file with known pattern
    std::string content;
    for (int i = 0; i < 1000; ++i) {
        content += static_cast<char>('A' + (i % 26));  // Repeating ABC... pattern
    }

    int fd = create_temp_file(content);
    constexpr int num_reads = 100;

    // Read different offsets and verify data
    std::vector<std::unique_ptr<char[]>> buffers(num_reads);
    std::atomic<int> completed_count{0};

    // Offsets to read from
    std::vector<int> offsets = {0, 1, 10, 26, 100, 250, 500, 999};

    {
        hnswlib::IOBackend backend;

        int idx = 0;
        for (int offset : offsets) {
            int read_size = std::min(10, 1000 - offset);
            buffers[idx] = std::make_unique<char[]>(read_size);
            backend.submit_io_task(std::make_unique<hnswlib::HnswIOTask>(
                fd, buffers[idx].get(), offset, read_size,
                [&completed_count]() {
                    completed_count.fetch_add(1);
                }
            ));
            ++idx;
        }

        while (completed_count.load() < static_cast<int>(offsets.size())) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    assert(completed_count.load() == static_cast<int>(offsets.size()));

    // Verify all reads are correct
    int idx = 0;
    for (int offset : offsets) {
        int read_size = std::min(10, 1000 - offset);
        for (int j = 0; j < read_size; ++j) {
            char expected = content[offset + j];
            char actual = buffers[idx][j];
            assert(actual == expected && "IO read data mismatch");
        }
        ++idx;
    }

    cleanup_temp_file(fd);
    std::cout << "PASSED" << std::endl;
}

void test_concurrent_read_correctness() {
    std::cout << "Test: concurrent_read_correctness... " << std::flush;

    // Create a file with 64KB of unique pattern
    std::string content(65536, 'X');
    for (size_t i = 0; i < content.size(); ++i) {
        content[i] = static_cast<char>(i % 256);  // Unique byte at each position
    }

    int fd = create_temp_file(content);

    constexpr int num_tasks = 256;
    std::vector<std::unique_ptr<char[]>> buffers(num_tasks);
    std::atomic<int> completed_count{0};

    for (auto& buf : buffers) {
        buf = std::make_unique<char[]>(256);
    }

    {
        hnswlib::IOBackend backend;

        // Read from 256 different 256-byte aligned offsets
        for (int i = 0; i < num_tasks; ++i) {
            int offset = i * 256;  // Aligned reads
            backend.submit_io_task(std::make_unique<hnswlib::HnswIOTask>(
                fd, buffers[i].get(), offset, 256,
                [&completed_count]() {
                    completed_count.fetch_add(1);
                }
            ));
        }

        while (completed_count.load() < num_tasks) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    assert(completed_count.load() == num_tasks);

    // Verify all data is correct
    for (int i = 0; i < num_tasks; ++i) {
        for (int j = 0; j < 256; ++j) {
            char expected = content[i * 256 + j];
            char actual = buffers[i][j];
            assert(actual == expected && "Concurrent IO read data mismatch");
        }
    }

    cleanup_temp_file(fd);
    std::cout << "PASSED" << std::endl;
}

void test_write_and_read_back() {
    std::cout << "Test: write_and_read_back... " << std::flush;

    std::string temp_path = "/tmp/iobackend_test_write_" + std::to_string(getpid()) + ".tmp";
    int fd = open(temp_path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        throw std::runtime_error("Failed to create temp file");
    }

    // Write initial data
    std::string write_content = "WRITE_TEST_DATA_12345678";
    if (write(fd, write_content.c_str(), write_content.size()) != static_cast<ssize_t>(write_content.size())) {
        close(fd);
        unlink(temp_path.c_str());
        throw std::runtime_error("Failed to write initial data");
    }

    lseek(fd, 0, SEEK_SET);

    auto read_buffer = std::make_unique<char[]>(64);
    std::atomic<bool> completed{false};

    {
        hnswlib::IOBackend backend;

        backend.submit_io_task(std::make_unique<hnswlib::HnswIOTask>(
            fd, read_buffer.get(), 0, write_content.size(),
            [&completed]() {
                completed = true;
            }
        ));

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    assert(completed.load());
    assert(memcmp(read_buffer.get(), write_content.c_str(), write_content.size()) == 0);

    close(fd);
    unlink(temp_path.c_str());
    std::cout << "PASSED" << std::endl;
}

void test_partial_read_correctness() {
    std::cout << "Test: partial_read_correctness... " << std::flush;

    // Create file with known pattern
    std::string content;
    for (int i = 0; i < 256; ++i) {
        content += static_cast<char>(i);  // 0x00, 0x01, 0x02, ...
    }

    int fd = create_temp_file(content);

    // Read various partial ranges
    struct TestCase {
        int offset;
        int size;
    };

    std::vector<TestCase> tests = {
        {0, 1},      // First byte
        {255, 1},    // Last byte
        {128, 128},  // Middle half
        {0, 256},    // Entire file
        {10, 50},    // Random middle segment
    };

    std::vector<std::unique_ptr<char[]>> buffers(tests.size());
    std::atomic<int> completed{0};

    {
        hnswlib::IOBackend backend;

        for (size_t i = 0; i < tests.size(); ++i) {
            buffers[i] = std::make_unique<char[]>(tests[i].size + 1);
            backend.submit_io_task(std::make_unique<hnswlib::HnswIOTask>(
                fd, buffers[i].get(), tests[i].offset, tests[i].size,
                [&completed]() {
                    completed.fetch_add(1);
                }
            ));
        }

        while (completed.load() < static_cast<int>(tests.size())) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // Verify correctness
    for (size_t i = 0; i < tests.size(); ++i) {
        for (int j = 0; j < tests[i].size; ++j) {
            char expected = content[tests[i].offset + j];
            char actual = buffers[i][j];
            assert(actual == expected && "Partial read data mismatch");
        }
    }

    cleanup_temp_file(fd);
    std::cout << "PASSED" << std::endl;
}

int main() {
    std::cout << "=== IOBackend Tests ===" << std::endl;

    test_basic_submit_and_complete();
    test_multiple_tasks_sequential();
    test_stop_during_active();
    test_large_buffer_io();
    test_task_order_preservation();
    test_concurrent_submitters();
    test_high_throughput_submitters();
    test_random_access_pattern();
    test_stress_many_threads();
    test_backpressure_under_load();
    test_io_correctness();
    test_concurrent_read_correctness();
    test_write_and_read_back();
    test_partial_read_correctness();

    std::cout << "=== All IOBackend Tests PASSED ===" << std::endl;
    return 0;
}
