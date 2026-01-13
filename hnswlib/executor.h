#pragma once

#include <cstdint>
#include <functional>
#include <liburing.h>
#include <fcntl.h>
#include <optional>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sched.h>
#include <coroutine>
#include <thread>
#include <atomic>
#include <cassert>
#include <cstring>
#include "async_simple/Executor.h"
#include "async_simple/coro/Lazy.h"
#include "mpsc.h"

namespace HnswExecutor {

// 用于 O_DIRECT 的对齐内存分配器
struct AlignedBuffer {
    void* ptr = nullptr;
    size_t size = 0;

    AlignedBuffer(size_t s) : size(s) {
        if (posix_memalign(&ptr, 4096, size)) throw std::runtime_error("OOM");
    }
    ~AlignedBuffer() { free(ptr); }
    AlignedBuffer(const AlignedBuffer&) = delete;
};

using Executor = async_simple::Executor;
using Func = async_simple::Executor::Func;
using UringCallback = std::function<void(int)>;

class UringExecutor;

class Context {
public:
    static UringExecutor& current_executor() {
        return *current_executor_ptr;
    }

    static void set_current_executor(UringExecutor &executor) {
        current_executor_ptr = &executor;
    }

private:
    static inline thread_local UringExecutor* current_executor_ptr = nullptr;
};

class UringExecutor: public Executor {
public:
    UringExecutor(int id) : cpu_id(id) {
        setup_ring();
        setup_eventfd();
    }

    ~UringExecutor() {
        if (worker_thread.joinable()) {
            running = false;
            wakeup();
            worker_thread.join();
        }
        io_uring_queue_exit(&ring);
        close(ev_fd);
    }

    // 启动调度器线程
    void start() {
        running = true;
        worker_thread = std::thread([this]() {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(this->cpu_id, &cpuset);
            pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
            HnswExecutor::Context::set_current_executor(*this);
            this->run_loop();
        });
    }

    virtual bool schedule(Func func) override {
        task_queue.enqueue(std::move(func));
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (is_sleeping.load(std::memory_order_relaxed)) {
            if (is_sleeping.exchange(false, std::memory_order_acquire)) {
                wakeup();
            }
        }
        return true;
    }

    virtual bool currentThreadInExecutor() const override {
        return &HnswExecutor::Context::current_executor() == this;
    }

    // 提交一个IO操作，返回值是res
    // 协程通过co_await this->async_read(fd, buf, len, offset)来等待
    auto async_read(int fd, void* buf, unsigned len, off_t offset) {
        struct Awaiter {
            UringExecutor* sched;
            int fd; void* buf; unsigned len; off_t off;
            UringCallback callback;
            int res = 0;

            bool await_ready() { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                callback = [h, this](int io_res) {
                    res = io_res;
                    sched->schedule([h]() mutable {
                        h.resume();
                    });
                };

                struct io_uring_sqe* sqe = io_uring_get_sqe(&sched->ring);
                io_uring_prep_read(sqe, fd, buf, len, off);
                io_uring_sqe_set_data(sqe, callback.target<UringCallback>());

                io_uring_submit(&sched->ring);
            }
            int await_resume() { return res; }
        };
        return Awaiter{this, fd, buf, len, offset, {}, 0};
    }

    
private:

    void setup_ring() {
        struct io_uring_params params;
        memset(&params, 0, sizeof(params));
        if (io_uring_queue_init_params(IOURING_ENTRIES, &ring, &params) < 0)
            throw std::runtime_error("Init ring failed");
    }

    void setup_eventfd() {
        ev_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (ev_fd < 0) throw std::runtime_error("Init eventfd failed");
        arm_eventfd();
    }

    void arm_eventfd() {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);

        ev_iov.iov_base = &ev_buf;
        ev_iov.iov_len = sizeof(ev_buf);

        // 构建eventfd回调
        ev_callback = [this](int) {
            // 读取eventfd的值以清零
            uint64_t val;
            read(ev_fd, &val, sizeof(val));
        };

        io_uring_prep_readv(sqe, ev_fd, &ev_iov, 1, 0);
        io_uring_sqe_set_data(sqe, &ev_callback);
        io_uring_submit(&ring);
    }

    void wakeup() {
        uint64_t u = 1;
        write(ev_fd, &u, sizeof(u));
    }

    void run_loop() {
        while (true) {
            bool did_work = false;

            // 1. 处理新任务
            std::optional<Func> task;
            while ((task = task_queue.try_dequeue()).has_value()) {
                task.value()();
                did_work = true;
            }

            // 2. 处理IO完成事件
            io_uring_cqe* cqe;
            unsigned head;
            unsigned count = 0;

            io_uring_for_each_cqe(&ring, head, cqe) {
                void* data = io_uring_cqe_get_data(cqe);
                auto* cb = reinterpret_cast<UringCallback*>(data);
                if (cb) {
                    cb->operator()(cqe->res);
                }
                count++;
            }
            if (count > 0) {
                io_uring_cq_advance(&ring, count);
                did_work = true;
                incompleted_io_count -= count;
            }

            // 3. 休眠决策
            if (!did_work) {
                is_sleeping.store(true, std::memory_order_seq_cst);

                if (!task_queue.empty()) {
                    is_sleeping.store(false, std::memory_order_relaxed);
                    continue;
                }

                io_uring_enter(ring.ring_fd, 0, 1, IORING_ENTER_GETEVENTS, NULL);
            }
        }
    }

private:
    static constexpr int IOURING_ENTRIES = 4096;

    int cpu_id;
    struct io_uring ring;

    int ev_fd;
    uint64_t ev_buf = 0;
    struct iovec ev_iov;
    UringCallback ev_callback;

    uint32_t incompleted_io_count = 0;

    std::thread worker_thread;
    MpscQueue<Func> task_queue;

    alignas(64)
    std::atomic<bool> is_sleeping {false};
    std::atomic<bool> running {false};
};

}  // namespace Executor
