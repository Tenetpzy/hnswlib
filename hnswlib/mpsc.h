#pragma once

#include <atomic>
#include <utility>

#include <atomic>
#include <utility>
#include <optional>

template <typename T>
class MpscBlockingQueue {
private:
    struct Node {
        T data;
        std::atomic<Node*> next{nullptr};

        template<typename... Args>
        Node(Args&&... args) : data(std::forward<Args>(args)...) {}

        Node() {}
    };

    std::atomic<Node*> head;
    Node* tail;
    std::atomic<uint64_t> wake_epoch{0};
    std::atomic<bool> stop_token{false};

public:
    MpscBlockingQueue() {
        Node* dummy = new Node();
        head.store(dummy);
        tail = dummy;
    }

    ~MpscBlockingQueue() {
        while (tail) {
            Node* next = tail->next.load();
            delete tail;
            tail = next;
        }
    }

    void stop() {
        stop_token.store(true, std::memory_order_release);
        wake_epoch.fetch_add(1, std::memory_order_release);
        wake_epoch.notify_all();
    }

    template <typename... Args>
    void enqueue(Args&&... args) {
        Node* new_node = new Node(std::forward<Args>(args)...);
        Node* prev_head = head.exchange(new_node, std::memory_order_acq_rel);
        prev_head->next.store(new_node, std::memory_order_release);

        wake_epoch.fetch_add(1, std::memory_order_release);
        wake_epoch.notify_one();
    }

    std::optional<T> dequeue_blocking() {
        while (true) {
            Node* next = tail->next.load(std::memory_order_acquire);

            if (next) {
                T value = std::move(next->data);
                delete tail;
                tail = next;
                return value;
            }

            if (stop_token.load(std::memory_order_acquire)) {
                return std::nullopt;
            }

            uint64_t snapshot_epoch = wake_epoch.load(std::memory_order_acquire);

            if (tail->next.load(std::memory_order_acquire) != nullptr) {
                continue;
            }

            if (stop_token.load(std::memory_order_acquire)) {
                return std::nullopt;
            }

            wake_epoch.wait(snapshot_epoch);
        }
    }
};