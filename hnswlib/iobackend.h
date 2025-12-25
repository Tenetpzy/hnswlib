#pragma once

#include <functional>
#include <liburing.h>
#include <memory>
#include <sys/types.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace hnswlib {

using IoCbFuncType = std::function<void()>;

struct HnswIOTask {
    int fd;
    void *buffer;
    off_t offset;
    size_t size;
    IoCbFuncType callback;

    HnswIOTask(int fd, void *buffer, off_t offset, size_t size, IoCbFuncType &&callback)
        : fd(fd), buffer(buffer), offset(offset), size(size), callback(std::move(callback)) {}
};

class IOBackend {
public:
    // initialize io_uring, start single working thread
    IOBackend();

    // RAII destructor
    ~IOBackend();

    // disable copy
    IOBackend(const IOBackend&) = delete;
    IOBackend& operator=(const IOBackend&) = delete;

    // submit to working thread (thread-safe, multiple callers)
    void submit_io_task(std::unique_ptr<HnswIOTask> task);

    // working thread main loop
    void run();

    // signal stop and join worker thread
    void stop();

private:
    // MPSC channel node
    struct TaskNode {
        std::unique_ptr<HnswIOTask> task;
        std::atomic<TaskNode*> next;
        TaskNode(std::unique_ptr<HnswIOTask> t) : task(std::move(t)), next(nullptr) {}
    };

    // lock-free fetch from mpsc channel, returns nullptr if empty
    TaskNode* mpsc_try_pop();

    // submit one task to io_uring
    void submit_one_task(HnswIOTask *task);

    // process completed io operations
    void process_completions(size_t batch_count);

private:
    // io_uring resources (RAII managed)
    io_uring ring_;

    // MPSC channel (lock-free queue)
    TaskNode* mpsc_head_;  // consumer visible
    std::atomic<TaskNode*> mpsc_tail_;               // producer visible (needs CAS)

    // synchronization
    std::atomic<bool> stop_flag_;
    std::condition_variable cv_;
    std::mutex cv_mutex_;

    // task tracking
    std::atomic<size_t> pending_tasks_;

    // worker thread
    std::thread worker_thread_;
    std::atomic<bool> running_;
};

} // namespace hnswlib
