#pragma once

#include <functional>
#include <liburing.h>
#include <memory>
#include <sys/types.h>
#include <thread>
#include "mpsc.h"

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
    // initialize io_uring, queue depth 256, start single working thread
    IOBackend();

    // RAII destructor
    ~IOBackend();

    // disable copy
    IOBackend(const IOBackend&) = delete;
    IOBackend& operator=(const IOBackend&) = delete;

    // submit to task_queue
    void submit_io_task(std::unique_ptr<HnswIOTask> task);

    // working thread main loop
    // outer loop:
    // wait_for_data_or_stop on task_queue, if stop, return
    // inner loop:
    // continue try_dequeue from task_queue until result is empty or not enough sqes
    // construct pread io_uring_sqe for tasks and submit it to io_uring, note: transfer unique_ptr to raw pointer and store in user_data
    // poll for completion events(do not enter kernel for waiting, just poll), for each event, recover unique_ptr<HnswIOTask> from user_data, call its callback
    // if all submitted tasks are done, go back to outer loop, else continue inner loop
    void run();

    // signal stop and join worker thread
    void stop();

private:
    io_uring ring;
    MpscBlockingQueue<std::unique_ptr<HnswIOTask>> task_queue;
    std::thread worker_thread;
};

} // namespace hnswlib
