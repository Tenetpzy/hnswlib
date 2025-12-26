#include "../../hnswlib/mpsc.h"
#include <thread>
#include <vector>
#include <cassert>
#include <iostream>
#include <chrono>

void test_try_dequeue_empty() {
    std::cout << "Test: try_dequeue_empty... " << std::flush;

    MpscBlockingQueue<int> queue;

    auto opt = queue.try_dequeue();
    assert(!opt.has_value());

    std::cout << "PASSED" << std::endl;
}

void test_try_dequeue_with_data() {
    std::cout << "Test: try_dequeue_with_data... " << std::flush;

    MpscBlockingQueue<int> queue;

    queue.enqueue(42);
    queue.enqueue(100);

    auto opt1 = queue.try_dequeue();
    auto opt2 = queue.try_dequeue();

    assert(opt1.has_value() && opt1.value() == 42);
    assert(opt2.has_value() && opt2.value() == 100);

    std::cout << "PASSED" << std::endl;
}

void test_basic_enqueue_dequeue_blocking() {
    std::cout << "Test: basic_enqueue_dequeue_blocking... " << std::flush;

    MpscBlockingQueue<int> queue;

    queue.enqueue(42);
    queue.enqueue(100);
    queue.enqueue(999);

    auto opt1 = queue.dequeue_blocking();
    auto opt2 = queue.dequeue_blocking();
    auto opt3 = queue.dequeue_blocking();

    assert(opt1.has_value() && opt1.value() == 42);
    assert(opt2.has_value() && opt2.value() == 100);
    assert(opt3.has_value() && opt3.value() == 999);

    std::cout << "PASSED" << std::endl;
}

void test_empty_queue_blocks() {
    std::cout << "Test: empty_queue_blocks... " << std::flush;

    MpscBlockingQueue<int> queue;
    bool got_value = false;

    std::thread consumer([&]() {
        auto opt = queue.dequeue_blocking();
        if (opt.has_value() && opt.value() == 42) {
            got_value = true;
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    queue.enqueue(42);

    consumer.join();
    assert(got_value);

    std::cout << "PASSED" << std::endl;
}

void test_stop_while_blocked() {
    std::cout << "Test: stop_while_blocked... " << std::flush;

    MpscBlockingQueue<int> queue;
    bool got_nullopt = false;

    std::thread consumer([&]() {
        auto opt = queue.dequeue_blocking();
        if (!opt.has_value()) {
            got_nullopt = true;
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    queue.stop();

    consumer.join();
    assert(got_nullopt);

    std::cout << "PASSED" << std::endl;
}

void test_multiple_producers() {
    std::cout << "Test: multiple_producers... " << std::flush;

    MpscBlockingQueue<int> queue;
    constexpr int producers = 32;
    constexpr int items_per_producer = 10000;
    std::atomic<int> total_received{0};

    std::vector<std::thread> producer_threads;
    producer_threads.reserve(producers);
    for (int i = 0; i < producers; ++i) {
        producer_threads.emplace_back([&queue, i, items_per_producer]() {
            for (int j = 0; j < items_per_producer; ++j) {
                queue.enqueue(i * items_per_producer + j);
            }
        });
    }

    std::thread consumer([&]() {
        int received = 0;
        while (received < producers * items_per_producer) {
            auto opt = queue.dequeue_blocking();
            if (opt.has_value()) {
                ++received;
            }
        }
        total_received.store(received, std::memory_order_relaxed);
    });

    for (auto& t : producer_threads) {
        t.join();
    }

    queue.stop();
    consumer.join();

    assert(total_received.load() == producers * items_per_producer);
    std::cout << "PASSED" << std::endl;
}

void test_stop_after_enqueue() {
    std::cout << "Test: stop_after_enqueue... " << std::flush;

    MpscBlockingQueue<int> queue;

    queue.enqueue(42);
    queue.stop();

    auto opt = queue.dequeue_blocking();
    assert(opt.has_value() && opt.value() == 42);

    auto opt2 = queue.dequeue_blocking();
    assert(!opt2.has_value());

    std::cout << "PASSED" << std::endl;
}

void test_wakeup_from_stop() {
    std::cout << "Test: wakeup_from_stop... " << std::flush;

    MpscBlockingQueue<int> queue;
    bool got_value = false;

    std::thread consumer([&]() {
        auto opt = queue.dequeue_blocking();
        if (opt.has_value() && opt.value() == 42) {
            got_value = true;
        }
    });

    queue.stop();
    queue.enqueue(42);

    consumer.join();
    assert(got_value);

    std::cout << "PASSED" << std::endl;
}

void test_move_semantics() {
    std::cout << "Test: move_semantics... " << std::flush;

    MpscBlockingQueue<std::vector<int>> queue;

    queue.enqueue(std::vector<int>{1, 2, 3});
    queue.enqueue(std::vector<int>{4, 5, 6});

    auto opt1 = queue.dequeue_blocking();
    auto opt2 = queue.dequeue_blocking();

    assert(opt1.has_value());
    assert(opt2.has_value());
    assert(opt1.value() == (std::vector<int>{1, 2, 3}));
    assert(opt2.value() == (std::vector<int>{4, 5, 6}));

    std::cout << "PASSED" << std::endl;
}

void test_mixed_try_and_blocking() {
    std::cout << "Test: mixed_try_and_blocking... " << std::flush;

    MpscBlockingQueue<int> queue;

    queue.enqueue(1);
    queue.enqueue(2);

    auto opt1 = queue.try_dequeue();
    assert(opt1.has_value() && opt1.value() == 1);

    auto opt2 = queue.try_dequeue();
    assert(opt2.has_value() && opt2.value() == 2);

    auto opt3 = queue.try_dequeue();
    assert(!opt3.has_value());

    std::cout << "PASSED" << std::endl;
}

void test_stress_test() {
    std::cout << "Test: stress_test... " << std::flush;

    MpscBlockingQueue<int> queue;
    constexpr int total_items = 100000;

    std::atomic<bool> stop_flag{false};
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};

    std::thread producer([&]() {
        while (produced.load(std::memory_order_relaxed) < total_items) {
            queue.enqueue(produced.load(std::memory_order_relaxed));
            produced.fetch_add(1, std::memory_order_relaxed);
        }
        stop_flag.store(true, std::memory_order_relaxed);
    });

    std::thread consumer([&]() {
        while (true) {
            auto opt = queue.dequeue_blocking();
            if (opt.has_value()) {
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
            if (stop_flag.load(std::memory_order_relaxed) &&
                consumed.load(std::memory_order_relaxed) >= produced.load(std::memory_order_relaxed)) {
                break;
            }
        }
    });

    producer.join();
    queue.stop();
    consumer.join();

    assert(consumed.load() == total_items);
    std::cout << "PASSED" << std::endl;
}

void test_reorder_producer_consumer() {
    std::cout << "Test: reorder_producer_consumer... " << std::flush;

    MpscBlockingQueue<int> queue;
    constexpr int num_items = 1000;

    std::atomic<int> consumer_started{0};
    std::vector<int> received;

    std::thread consumer([&]() {
        consumer_started.store(1, std::memory_order_relaxed);
        for (int i = 0; i < num_items; ++i) {
            auto opt = queue.dequeue_blocking();
            if (opt.has_value()) {
                received.push_back(opt.value());
            }
        }
    });

    while (!consumer_started.load(std::memory_order_relaxed)) {
        std::this_thread::yield();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    for (int i = 0; i < num_items; ++i) {
        queue.enqueue(i);
    }

    queue.stop();
    consumer.join();

    assert(received.size() == static_cast<size_t>(num_items));
    for (int i = 0; i < num_items; ++i) {
        assert(received[i] == i);
    }

    std::cout << "PASSED" << std::endl;
}

int main() {
    std::cout << "=== MPSC Blocking Queue Tests ===" << std::endl;

    test_try_dequeue_empty();
    test_try_dequeue_with_data();
    test_basic_enqueue_dequeue_blocking();
    test_empty_queue_blocks();
    test_stop_while_blocked();
    test_multiple_producers();
    test_stop_after_enqueue();
    test_wakeup_from_stop();
    test_move_semantics();
    test_mixed_try_and_blocking();
    test_stress_test();
    test_reorder_producer_consumer();

    std::cout << "=== All Tests PASSED ===" << std::endl;
    return 0;
}
