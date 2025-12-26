#pragma once

#include <atomic>
#include <utility>
#include <optional>
#include <new>

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

#ifdef __cpp_lib_hardware_interference_size
    static constexpr size_t cache_line_size = std::hardware_destructive_interference_size;
#else
    static constexpr size_t cache_line_size = 64;
#endif

    // 状态机简化：
    // ACTIVE (0): 消费者正在工作或队列有数据
    // WAITING (1): 消费者队列为空且准备睡眠
    enum State : uint32_t {
        ACTIVE = 0,
        WAITING = 1
    };

    alignas(cache_line_size) std::atomic<Node*> head;
    alignas(cache_line_size) Node* tail;
    
    // state 需要被生产者和消费者共同访问，单独占一行
    alignas(cache_line_size) std::atomic<uint32_t> state{ACTIVE};
    
    // stop_token 主要是消费者读，生产者写一次
    std::atomic<bool> stop_token{false};

    char padding[cache_line_size - sizeof(std::atomic<uint32_t>) - sizeof(std::atomic<bool>)];

public:
    MpscBlockingQueue() {
        Node* dummy = new Node();
        head.store(dummy, std::memory_order_relaxed);
        tail = dummy;
    }

    ~MpscBlockingQueue() {
        while (tail) {
            Node* next_node = tail->next.load(std::memory_order_relaxed);
            delete tail;
            tail = next_node;
        }
    }

    void stop() {
        stop_token.store(true, std::memory_order_release);
        // 强制唤醒：将状态改为 ACTIVE，确保 wait 能返回
        state.exchange(ACTIVE, std::memory_order_seq_cst);
        state.notify_all();
    }

    template <typename... Args>
    void enqueue(Args&&... args) {
        Node* new_node = new Node(std::forward<Args>(args)...);
        
        // 1. 生产者入队
        Node* prev_head = head.exchange(new_node, std::memory_order_acq_rel);
        prev_head->next.store(new_node, std::memory_order_release);

        // 2. 唤醒逻辑
        // 我们不只是检查 WAITING，而是尝试将其交换为 ACTIVE。
        // 如果原来的值是 WAITING，说明消费者正在准备睡或已经睡了，我们需要 notify。
        // 如果原来的值是 ACTIVE，说明消费者醒着，或者其他生产者已经叫醒它了，不需要 notify。
        // seq_cst: 不能使用acq_rel内存序，原因: 避免store-load重排
        // 此操作为read-modify-write操作，如果read为acquire，则上面的store可能被重排到read state之后
        // 导致消费者误认为没有数据而睡眠，错过唤醒
        if (state.exchange(ACTIVE, std::memory_order_seq_cst) == WAITING) {
            state.notify_one();
        }
    }

    std::optional<T> dequeue_blocking() {
        auto has_data = wait_for_data_or_stop();
        if (!has_data) {
            return std::nullopt;
        }
        Node* next_node = tail->next.load(std::memory_order_acquire);
        return consume_node(next_node);
    }

    std::optional<T> try_dequeue() {
        Node* next_node = tail->next.load(std::memory_order_acquire);
        if (next_node) {
            return consume_node(next_node);
        }
        return std::nullopt;
    }

    // returns true for new data, false for stop
    bool wait_for_data_or_stop() {
        while (true) {
            // 1. 尝试直接消费
            Node* next_node = tail->next.load(std::memory_order_acquire);
            if (next_node) {
                return true;
            } else if (stop_token.load(std::memory_order_acquire)) {
                return false;
            }

            // 2. 准备睡眠
            state.store(WAITING, std::memory_order_seq_cst);

            // 3. 双重检查 (Double Check)
            // 必须在设置 WAITING 之后再次检查，防止在设置过程中有数据进来
            next_node = tail->next.load(std::memory_order_acquire);
            if (next_node) {
                // 有数据了，撤销 WAITING 状态（改回 ACTIVE），去消费
                state.store(ACTIVE, std::memory_order_relaxed);
                return true;
            } else if (stop_token.load(std::memory_order_acquire)) {
                return false;
            }

            // 4. 阻塞等待
            // wait 的逻辑是：如果 state == WAITING，则挂起，基于futex实现原子性完成两步骤。
            // 
            // 安全性：
            // 情况 A：生产者在 "第3步双重检查" 之前入队。
            //    -> 第3步会看到数据，直接返回。
            //
            // 情况 B：生产者在 "第3步" 之后，"第4步 wait" 之前入队。
            //    -> 生产者执行 exchange(ACTIVE)，state 变为 ACTIVE。
            //    -> 消费者执行 wait(WAITING)。发现 state 实际上是 ACTIVE。
            //    -> wait 立即返回，不睡。
            //    -> 循环回到开头，消费数据。
            //
            // 情况 C：生产者在 "第4步 wait" 已经睡着后入队。
            //    -> 生产者执行 exchange(ACTIVE)，state 变为 ACTIVE。
            //    -> 系统唤醒消费者。
            state.wait(WAITING, std::memory_order_acquire);
        }
    }

private:
    std::optional<T> consume_node(Node* next_node) {
        T value = std::move(next_node->data);
        delete tail;
        tail = next_node;
        return value;
    }
};