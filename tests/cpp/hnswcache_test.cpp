#include "../../hnswlib/hnswcache.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

// Helper function to create a test file with known content
static std::string create_test_file(size_t num_pages, size_t page_size) {
    std::string filename = "/tmp/hnswcache_test_" + std::to_string(std::rand()) + ".bin";
    std::ofstream ofs(filename, std::ios::binary);
    if (!ofs) {
        throw std::runtime_error("Failed to create test file");
    }

    // Each page contains: page_id repeated as uint32_t values
    for (size_t page_id = 0; page_id < num_pages; ++page_id) {
        std::vector<char> page_data(page_size);
        // Fill page with pattern: first 4 bytes = page_id, rest = page_id repeated
        for (size_t i = 0; i + sizeof(uint32_t) <= page_size; i += sizeof(uint32_t)) {
            uint32_t value = static_cast<uint32_t>(page_id);
            std::memcpy(page_data.data() + i, &value, sizeof(uint32_t));
        }
        ofs.write(page_data.data(), page_size);
    }
    ofs.close();
    return filename;
}

static void remove_test_file(const std::string& filename) {
    std::remove(filename.c_str());
}

// Verify that page content is correct for the given page_id
static bool verify_page_content(const char* data, size_t page_size, hnswlib::page_id_t expected_page_id) {
    for (size_t i = 0; i + sizeof(uint32_t) <= page_size; i += sizeof(uint32_t)) {
        uint32_t value;
        std::memcpy(&value, data + i, sizeof(uint32_t));
        if (value != expected_page_id) {
            return false;
        }
    }
    return true;
}

// Test 1: Basic single-threaded get_page functionality
static void test_basic_get_page() {
    std::cout << "Test: basic_get_page... ";

    const size_t page_size = 64;
    const size_t num_pages = 10;
    const size_t cache_size = page_size * 5;

    std::string filename = create_test_file(num_pages, page_size);

    try {
        hnswlib::HnswPageCache cache(filename, page_size, cache_size);

        // Get page 0 and verify content
        {
            auto handler = cache.get_page(0);
            assert(verify_page_content(handler.get_ptr(), page_size, 0));
        }

        // Get page 3 and verify content
        {
            auto handler = cache.get_page(3);
            assert(verify_page_content(handler.get_ptr(), page_size, 3));
        }

        // Get page 7 and verify content
        {
            auto handler = cache.get_page(7);
            assert(verify_page_content(handler.get_ptr(), page_size, 7));
        }

        std::cout << "PASSED\n";
    } catch (const std::exception& e) {
        std::cout << "FAILED: " << e.what() << "\n";
        remove_test_file(filename);
        exit(1);
    }

    remove_test_file(filename);
}

// Test 2: Page caching - accessing same page multiple times
static void test_page_caching() {
    std::cout << "Test: page_caching... ";

    const size_t page_size = 64;
    const size_t num_pages = 10;
    const size_t cache_size = page_size * 5;

    std::string filename = create_test_file(num_pages, page_size);

    try {
        hnswlib::HnswPageCache cache(filename, page_size, cache_size);

        // Get page 2 multiple times
        char* ptr1;
        {
            auto handler1 = cache.get_page(2);
            ptr1 = handler1.get_ptr();
            assert(verify_page_content(ptr1, page_size, 2));
        }

        // Get same page again - should return from cache (same pointer)
        {
            auto handler2 = cache.get_page(2);
            char* ptr2 = handler2.get_ptr();
            assert(ptr1 == ptr2); // Should be same cached page
            assert(verify_page_content(ptr2, page_size, 2));
        }

        std::cout << "PASSED\n";
    } catch (const std::exception& e) {
        std::cout << "FAILED: " << e.what() << "\n";
        remove_test_file(filename);
        exit(1);
    }

    remove_test_file(filename);
}

// Test 3: LRU eviction when cache is full
static void test_lru_eviction() {
    std::cout << "Test: lru_eviction... ";

    const size_t page_size = 64;
    const size_t num_pages = 20;
    const size_t cache_size = page_size * 3; // Cache can hold only 3 pages

    std::string filename = create_test_file(num_pages, page_size);

    try {
        hnswlib::HnswPageCache cache(filename, page_size, cache_size);

        // Access pages 0, 1, 2 - fills the cache
        {
            auto h0 = cache.get_page(0);
            assert(verify_page_content(h0.get_ptr(), page_size, 0));
        }
        {
            auto h1 = cache.get_page(1);
            assert(verify_page_content(h1.get_ptr(), page_size, 1));
        }
        {
            auto h2 = cache.get_page(2);
            assert(verify_page_content(h2.get_ptr(), page_size, 2));
        }

        // Access page 3 - should evict page 0 (LRU)
        {
            auto h3 = cache.get_page(3);
            assert(verify_page_content(h3.get_ptr(), page_size, 3));
        }

        // Access page 4 - should evict page 1
        {
            auto h4 = cache.get_page(4);
            assert(verify_page_content(h4.get_ptr(), page_size, 4));
        }

        // Access page 0 again - should be reloaded from disk
        {
            auto h0_again = cache.get_page(0);
            assert(verify_page_content(h0_again.get_ptr(), page_size, 0));
        }

        std::cout << "PASSED\n";
    } catch (const std::exception& e) {
        std::cout << "FAILED: " << e.what() << "\n";
        remove_test_file(filename);
        exit(1);
    }

    remove_test_file(filename);
}

// Test 4: Reference counting prevents eviction
static void test_refcount_prevents_eviction() {
    std::cout << "Test: refcount_prevents_eviction... ";

    const size_t page_size = 64;
    const size_t num_pages = 20;
    const size_t cache_size = page_size * 3;

    std::string filename = create_test_file(num_pages, page_size);

    try {
        hnswlib::HnswPageCache cache(filename, page_size, cache_size);

        // Hold references to pages 0, 1, 2
        auto h0 = cache.get_page(0);
        auto h1 = cache.get_page(1);
        auto h2 = cache.get_page(2);

        char* ptr0 = h0.get_ptr();
        char* ptr1 = h1.get_ptr();
        char* ptr2 = h2.get_ptr();

        assert(verify_page_content(ptr0, page_size, 0));
        assert(verify_page_content(ptr1, page_size, 1));
        assert(verify_page_content(ptr2, page_size, 2));

        // Release h0, now page 0 can be evicted
        h0 = std::move(h1); // This releases h0 and moves h1

        // Access page 3 - should be able to evict page 0 now
        auto h3 = cache.get_page(3);
        assert(verify_page_content(h3.get_ptr(), page_size, 3));

        // Page 2 should still be valid (still held)
        assert(verify_page_content(ptr2, page_size, 2));

        std::cout << "PASSED\n";
    } catch (const std::exception& e) {
        std::cout << "FAILED: " << e.what() << "\n";
        remove_test_file(filename);
        exit(1);
    }

    remove_test_file(filename);
}

// Test 5: PageHandler move semantics
static void test_page_handler_move() {
    std::cout << "Test: page_handler_move... ";

    const size_t page_size = 64;
    const size_t num_pages = 10;
    const size_t cache_size = page_size * 5;

    std::string filename = create_test_file(num_pages, page_size);

    try {
        hnswlib::HnswPageCache cache(filename, page_size, cache_size);

        // Test move constructor
        auto h1 = cache.get_page(0);
        char* original_ptr = h1.get_ptr();

        auto h2 = std::move(h1);
        assert(h2.get_ptr() == original_ptr);

        // Test move assignment
        auto h3 = cache.get_page(1);
        h3 = std::move(h2);
        assert(h3.get_ptr() == original_ptr);

        // Verify content is still valid
        assert(verify_page_content(h3.get_ptr(), page_size, 0));

        std::cout << "PASSED\n";
    } catch (const std::exception& e) {
        std::cout << "FAILED: " << e.what() << "\n";
        remove_test_file(filename);
        exit(1);
    }

    remove_test_file(filename);
}

// Test 6: Multi-threaded concurrent access to different pages
static void test_multithread_different_pages() {
    std::cout << "Test: multithread_different_pages... ";

    const size_t page_size = 64;
    const size_t num_pages = 100;
    const size_t cache_size = page_size * 50;
    const int num_threads = 8;
    const int iterations = 100;

    std::string filename = create_test_file(num_pages, page_size);

    try {
        hnswlib::HnswPageCache cache(filename, page_size, cache_size);
        std::atomic<bool> success{true};
        std::vector<std::thread> threads;

        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&, t]() {
                std::mt19937 rng(t * 12345);
                for (int i = 0; i < iterations; ++i) {
                    hnswlib::page_id_t page_id = rng() % num_pages;
                    try {
                        auto handler = cache.get_page(page_id);
                        if (!verify_page_content(handler.get_ptr(), page_size, page_id)) {
                            success = false;
                        }
                    } catch (...) {
                        success = false;
                    }
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        assert(success.load());
        std::cout << "PASSED\n";
    } catch (const std::exception& e) {
        std::cout << "FAILED: " << e.what() << "\n";
        remove_test_file(filename);
        exit(1);
    }

    remove_test_file(filename);
}

// Test 7: Multi-threaded concurrent access to same page
static void test_multithread_same_page() {
    std::cout << "Test: multithread_same_page... ";

    const size_t page_size = 64;
    const size_t num_pages = 10;
    const size_t cache_size = page_size * 5;
    const int num_threads = 16;
    const int iterations = 200;

    std::string filename = create_test_file(num_pages, page_size);

    try {
        hnswlib::HnswPageCache cache(filename, page_size, cache_size);
        std::atomic<bool> success{true};
        std::vector<std::thread> threads;
        const hnswlib::page_id_t target_page = 5;

        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&]() {
                for (int i = 0; i < iterations; ++i) {
                    try {
                        auto handler = cache.get_page(target_page);
                        if (!verify_page_content(handler.get_ptr(), page_size, target_page)) {
                            success = false;
                        }
                    } catch (...) {
                        success = false;
                    }
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        assert(success.load());
        std::cout << "PASSED\n";
    } catch (const std::exception& e) {
        std::cout << "FAILED: " << e.what() << "\n";
        remove_test_file(filename);
        exit(1);
    }

    remove_test_file(filename);
}

// Test 8: Multi-threaded stress test with eviction pressure
static void test_multithread_eviction_stress() {
    std::cout << "Test: multithread_eviction_stress... ";

    const size_t page_size = 64;
    const size_t num_pages = 100;
    const size_t cache_size = page_size * 10; // Small cache to force eviction
    const int num_threads = 8;
    const int iterations = 500;

    std::string filename = create_test_file(num_pages, page_size);

    try {
        hnswlib::HnswPageCache cache(filename, page_size, cache_size);
        std::atomic<bool> success{true};
        std::atomic<int> completed_ops{0};
        std::vector<std::thread> threads;

        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&, t]() {
                std::mt19937 rng(t * 67890);
                for (int i = 0; i < iterations; ++i) {
                    hnswlib::page_id_t page_id = rng() % num_pages;
                    try {
                        auto handler = cache.get_page(page_id);
                        if (!verify_page_content(handler.get_ptr(), page_size, page_id)) {
                            success = false;
                        }
                        completed_ops.fetch_add(1);
                    } catch (...) {
                        success = false;
                    }
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        assert(success.load());
        assert(completed_ops.load() == num_threads * iterations);
        std::cout << "PASSED (" << completed_ops.load() << " operations)\n";
    } catch (const std::exception& e) {
        std::cout << "FAILED: " << e.what() << "\n";
        remove_test_file(filename);
        exit(1);
    }

    remove_test_file(filename);
}

// Test 9: Multi-threaded test with held references blocking eviction
static void test_multithread_held_references() {
    std::cout << "Test: multithread_held_references... ";

    const size_t page_size = 64;
    const size_t num_pages = 50;
    const size_t cache_size = page_size * 20;
    const int num_threads = 4;
    const int iterations = 100;

    std::string filename = create_test_file(num_pages, page_size);

    try {
        hnswlib::HnswPageCache cache(filename, page_size, cache_size);
        std::atomic<bool> success{true};
        std::vector<std::thread> threads;

        // Threads that hold references for varying durations
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&, t]() {
                std::mt19937 rng(t * 11111);
                for (int i = 0; i < iterations; ++i) {
                    hnswlib::page_id_t page_id = rng() % num_pages;
                    try {
                        auto handler = cache.get_page(page_id);
                        if (!verify_page_content(handler.get_ptr(), page_size, page_id)) {
                            success = false;
                        }
                        // Hold reference for random duration
                        std::this_thread::sleep_for(std::chrono::microseconds(rng() % 50));
                    } catch (...) {
                        success = false;
                    }
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        assert(success.load());
        std::cout << "PASSED\n";
    } catch (const std::exception& e) {
        std::cout << "FAILED: " << e.what() << "\n";
        remove_test_file(filename);
        exit(1);
    }

    remove_test_file(filename);
}

// Test 10: Race condition test - simultaneous first access to same page
static void test_race_first_access() {
    std::cout << "Test: race_first_access... ";

    const size_t page_size = 64;
    const size_t num_pages = 100;
    const size_t cache_size = page_size * 50;
    const int num_threads = 16;

    std::string filename = create_test_file(num_pages, page_size);

    try {
        // Run multiple rounds to increase chance of catching race conditions
        for (int round = 0; round < 10; ++round) {
            hnswlib::HnswPageCache cache(filename, page_size, cache_size);
            std::atomic<bool> success{true};
            std::vector<std::thread> threads;
            std::atomic<int> ready{0};
            std::atomic<bool> go{false};

            hnswlib::page_id_t target_page = round % num_pages;

            // All threads try to access the same uncached page simultaneously
            for (int t = 0; t < num_threads; ++t) {
                threads.emplace_back([&]() {
                    ready.fetch_add(1);
                    // Spin until all threads are ready
                    while (!go.load()) {
                        std::this_thread::yield();
                    }

                    try {
                        auto handler = cache.get_page(target_page);
                        if (!verify_page_content(handler.get_ptr(), page_size, target_page)) {
                            success = false;
                        }
                    } catch (...) {
                        success = false;
                    }
                });
            }

            // Wait for all threads to be ready
            while (ready.load() < num_threads) {
                std::this_thread::yield();
            }
            go = true;

            for (auto& t : threads) {
                t.join();
            }

            assert(success.load());
        }

        std::cout << "PASSED\n";
    } catch (const std::exception& e) {
        std::cout << "FAILED: " << e.what() << "\n";
        remove_test_file(filename);
        exit(1);
    }

    remove_test_file(filename);
}

int main() {
    std::cout << "=== HnswPageCache Unit Tests ===\n\n";

    std::cout << "--- Single-threaded Tests ---\n";
    test_basic_get_page();
    test_page_caching();
    test_lru_eviction();
    test_refcount_prevents_eviction();
    test_page_handler_move();

    std::cout << "\n--- Multi-threaded Tests ---\n";
    test_multithread_different_pages();
    test_multithread_same_page();
    test_multithread_eviction_stress();
    test_multithread_held_references();
    test_race_first_access();

    std::cout << "\n=== All Tests Passed ===\n";
    return 0;
}
