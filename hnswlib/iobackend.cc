#include "iobackend.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace hnswlib {

IOBackend::IOBackend() {
    // Initialize io_uring with queue depth 256
    int ret = io_uring_queue_init(256, &ring, 0);
    if (ret < 0) {
        throw std::runtime_error("Failed to initialize io_uring: " + std::string(strerror(-ret)));
    }

    // Start worker thread
    worker_thread = std::thread([this]() { this->run(); });
}

IOBackend::~IOBackend() {
    stop();
    if (worker_thread.joinable()) {
        worker_thread.join();
    }
    io_uring_queue_exit(&ring);
}

void IOBackend::submit_io_task(std::unique_ptr<HnswIOTask> task) {
    task_queue.enqueue(std::move(task));
}

void IOBackend::run() {
    while (true) {
        // Outer loop: wait for data or stop signal
        auto task_opt = task_queue.wait_for_data_or_stop();
        if (!task_opt) {
            // Stop signal received
            return;
        }

        std::vector<std::unique_ptr<HnswIOTask>> batch_tasks;
        uint32_t remaining = 0;

        do {
            // Try to dequeue more tasks
            unsigned sqe_available = io_uring_sq_space_left(&ring);
            for (unsigned i = 0; i < sqe_available; ++i) {
                auto next_task = task_queue.try_dequeue();
                if (!next_task.has_value()) {
                    break;
                }
                batch_tasks.push_back(std::move(next_task.value()));
            }

            // Submit all tasks to io_uring
            for (auto& task : batch_tasks) {
                struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
                if (!sqe) {
                    throw std::runtime_error("Failed to get SQE, should not happen");
                }

                // Setup pread operation
                io_uring_prep_read(sqe, task->fd, task->buffer, task->size, task->offset);
                
                // Transfer ownership: convert unique_ptr to raw pointer and store in user_data
                io_uring_sqe_set_data(sqe, task.release());
            }
            
            // Submit all prepared operations
            int submitted = io_uring_submit(&ring);
            if (submitted < 0) {
                throw std::runtime_error("io_uring_submit failed: " + std::string(strerror(-submitted)));
            }

            remaining += submitted;
            batch_tasks.clear();

            // Poll for completion events
            while (true) {
                struct io_uring_cqe* cqe;
                int ret = io_uring_peek_cqe(&ring, &cqe);
                
                if (ret == -EAGAIN) {
                    break;
                }
                
                if (ret < 0) {
                    throw std::runtime_error("io_uring_peek_cqe failed: " + std::string(strerror(-ret)));
                }

                // Recover the task from user_data
                HnswIOTask* completed_task = static_cast<HnswIOTask*>(io_uring_cqe_get_data(cqe));
                std::unique_ptr<HnswIOTask> task_ptr(completed_task);

                if (cqe->res < 0) {
                    std::cerr << "IO operation failed: " << strerror(-cqe->res) << std::endl;
                } else if (static_cast<size_t>(cqe->res) != task_ptr->size) {
                    std::cerr << "Incomplete read: expected " << task_ptr->size 
                            << " bytes, got " << cqe->res << " bytes" << ", offset " << task_ptr->offset << std::endl;
                }

                task_ptr->callback();
                io_uring_cqe_seen(&ring, cqe);
                --remaining;
            }
        } while (remaining > 0);
    }
}

void IOBackend::stop() {
    task_queue.stop();
}

} // namespace hnswlib