#pragma once

#include <cstdint>
#include <functional>
#include <liburing.h>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sched.h>
#include <coroutine>
#include <thread>
#include <atomic>
#include <x86intrin.h>
#include <cassert>
#include <cstring>
#include "async_simple/Executor.h"
#include "async_simple/coro/Lazy.h"
#include "mpsc.h"

namespace HnswExecutor {

class TscClock {
public:
    static double get_ticks_per_ns() {
        static const double ticks_per_ns = []() {
            // 预热：先做一次短 sleep，让 CPU 脱离深度睡眠状态，恢复全速频率
            // 防止从 C-State 唤醒过程影响校准
            using namespace std::chrono;
            std::this_thread::sleep_for(milliseconds(10));
            auto start_time = steady_clock::now();
            uint64_t start_tsc = __rdtsc();
            std::this_thread::sleep_for(milliseconds(100));
            uint64_t end_tsc = __rdtsc();
            auto end_time = steady_clock::now();
            auto duration_ns = duration_cast<nanoseconds>(end_time - start_time).count();
            return static_cast<double>(end_tsc - start_tsc) / duration_ns;
        }();
        return ticks_per_ns;
    }

    static uint64_t ms_to_ticks(uint64_t ms) {
        return static_cast<uint64_t>(ms * 1000000.0 * get_ticks_per_ns());
    }
    
    static uint64_t us_to_ticks(uint64_t us) {
        return static_cast<uint64_t>(us * 1000.0 * get_ticks_per_ns());
    }

    static inline uint64_t now() __attribute__((always_inline)) {
        return __rdtsc();
    }
    
    static inline void relax() __attribute__((always_inline)) {
        _mm_pause(); 
    }
};

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

class UringContext {
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

struct CqeHandler {
    virtual void complete(int32_t res) = 0;
    virtual ~CqeHandler() = default;
};

class UringExecutor: public Executor {
public:
    UringExecutor(int id) : cpu_id(id) {}

    ~UringExecutor() {
        if (worker_thread.joinable()) {
            running = false;
            wakeup();
            worker_thread.join();
        }
        io_uring_queue_exit(&ring);
        close(ev_fd);
    }

    int id() const {
        return cpu_id;
    }

    void start() {
        running = true;
        worker_thread = std::thread([this]() {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(this->cpu_id, &cpuset);
            pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
            HnswExecutor::UringContext::set_current_executor(*this);
            setup_ring();
            setup_eventfd();
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
        return &HnswExecutor::UringContext::current_executor() == this;
    }

    // async_simple框架依赖checkin和checkout做协程在特定上下文上的调度
    // 通过ViaCoroutine，自定义的awaiter中，拿到协程句柄，恢复时直接调它的resume，ViaCoroutine帮助协程在原上下文恢复执行
    // 我认为这是不好的设计，不应该把coroutine_handle暴露给协程的使用者，应该类似Rust，由Executor提供一个Waker对象用于唤醒
    // 不想改框架了，只能实现checkin和checkout
    virtual bool checkin(Func func, Context ctx, [[maybe_unused]] async_simple::ScheduleOptions opts) override {
        return reinterpret_cast<UringExecutor*>(ctx)->schedule(std::move(func));
    }
    virtual Context checkout() override {
        return this;
    }

    // 提交一个IO操作，返回值是res
    // 协程通过co_await this->async_read(fd, buf, len, offset)来等待
    auto async_read(int fd, void* buf, unsigned len, off_t offset) {
        struct ReadAwaiter : public CqeHandler {
            UringExecutor* sched;
            int fd; 
            void* buf; 
            unsigned len; 
            off_t off;
            int32_t result = 0;
            std::coroutine_handle<> coro;

            ReadAwaiter(UringExecutor* s, int f, void* b, unsigned l, off_t o)
                : sched(s), fd(f), buf(b), len(l), off(o) {}

            bool await_ready() const { return false; }

            void await_suspend(std::coroutine_handle<> h) {
                coro = h;
                struct io_uring_sqe* sqe = sched->get_sqe_safe();
                io_uring_prep_read(sqe, fd, buf, len, off);
                io_uring_sqe_set_data(sqe, static_cast<CqeHandler*>(this));
            }

            int await_resume() { return result; }

            void complete(int32_t res) override {
                result = res;
                coro.resume(); 
            }
        };

        return ReadAwaiter{this, fd, buf, len, offset};
    }

    
private:

    struct io_uring_sqe* get_sqe_safe() {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        if (!sqe) {
            io_uring_submit(&ring);
            sqe = io_uring_get_sqe(&ring);
            if (!sqe) throw std::runtime_error("IoUring SQ Full"); 
        }
        return sqe;
    }

    void setup_ring() {
        struct io_uring_params params;
        memset(&params, 0, sizeof(params));
        
        // 承诺只有一个线程提交请求，内核免锁
        params.flags |= IORING_SETUP_SINGLE_ISSUER;
        
        // 推迟到iouring_enter内核再收割，避免中断后半部的work_queue影响用户态线程的cache
        params.flags |= IORING_SETUP_DEFER_TASKRUN;
        
        if (io_uring_queue_init_params(IOURING_ENTRIES, &ring, &params) < 0)
            throw std::runtime_error("Init ring failed");
    }

    struct EventFdHandler : public CqeHandler {
        UringExecutor* executor;
        int fd;
        uint64_t buf = 0;
        struct iovec iov;

        EventFdHandler(UringExecutor* ex, int f) : executor(ex), fd(f) {
            iov.iov_base = &buf;
            iov.iov_len = sizeof(buf);
        }

        void arm() {
            struct io_uring_sqe* sqe = executor->get_sqe_safe();
            io_uring_prep_readv(sqe, fd, &iov, 1, 0);
            io_uring_sqe_set_data(sqe, static_cast<CqeHandler*>(this));
        }

        void complete(int32_t res) override {
            (void)res;
            arm();
        }
    };

    void setup_eventfd() {
        ev_fd = eventfd(0, 0);
        if (ev_fd < 0) 
            throw std::runtime_error("Init eventfd failed");
        ev_handler = std::make_unique<EventFdHandler>(this, ev_fd);
        ev_handler->arm(); 
    }

    void wakeup() {
        uint64_t u = 1;
        write(ev_fd, &u, sizeof(u));
    }

    void run_loop() {
        const uint64_t IDLE_TIMEOUT_TICKS = TscClock::ms_to_ticks(10);
        uint64_t start_idle_tsc = 0;
        bool is_spinning = false;

        while (running) {
            bool did_work = false;

            // 处理新任务
            std::optional<Func> task;
            int task_limit = 64;
            while (task_limit-- > 0 && (task = task_queue.try_dequeue()).has_value()) {
                task.value()();
                did_work = true;
            }

            // 处理IO完成事件
            io_uring_cqe* cqe;
            unsigned head;
            unsigned count = 0;

            io_uring_for_each_cqe(&ring, head, cqe) {
                count++;
                auto* handler = reinterpret_cast<CqeHandler*>(io_uring_cqe_get_data(cqe));
                if (handler) {
                    handler->complete(cqe->res);
                }
            }
            if (count > 0) {
                io_uring_cq_advance(&ring, count);
                did_work = true;
            }

            if (did_work) {
                io_uring_submit(&ring);
                is_spinning = false;  // 如果干了活，重置自旋状态
                continue;
            }

            if (!is_spinning) {
                // 刚发现没事做，记录当前时间，开始计时
                start_idle_tsc = TscClock::now();
                is_spinning = true;
            } else {
                // 已经在自旋了，检查是否超时
                uint64_t current_tsc = TscClock::now();
                if (current_tsc - start_idle_tsc < IDLE_TIMEOUT_TICKS) {
                    // 未超时：CPU降频空转
                    TscClock::relax(); 
                    continue; 
                }

                // 休眠
                is_sleeping.store(true, std::memory_order_seq_cst);

                if (!task_queue.empty()) {
                    is_sleeping.store(false, std::memory_order_relaxed);
                    continue;
                }

                io_uring_submit_and_wait(&ring, 1);
            }
        }
    }

private:
    static constexpr int IOURING_ENTRIES = 4096;

    int cpu_id;
    struct io_uring ring;

    int ev_fd;
    std::unique_ptr<EventFdHandler> ev_handler;

    std::thread worker_thread;
    MpscQueue<Func> task_queue;

    alignas(64)
    std::atomic<bool> is_sleeping {false};
    std::atomic<bool> running {false};
};

}  // namespace Executor
