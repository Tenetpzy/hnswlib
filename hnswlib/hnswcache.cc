#include "hnswcache.h"
#include "executor.h"
#include "metric.h"
#include <cassert>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace hnswlib {

class PageLoadingAwaiter {
public:
    PageLoadingAwaiter(PageEntry *entry): entry(entry) {}

    void await_suspend(std::coroutine_handle<> h) {
        entry->waiters.push_back(h);
    }

    void await_resume() {}

    bool await_ready() {
        return entry->state == PageEntry::State::InCache;
    }

private:
    PageEntry *entry;
};


PageHandler::~PageHandler() {
    if (cache && entry)
        cache->put_page(entry);
}

PageHandler& PageHandler::operator=(PageHandler&& other) noexcept {
    if (this != &other) {
        if (cache && entry) {
            cache->put_page(this->entry);
        }
        cache = other.cache;
        entry = other.entry;
        other.cache = nullptr;
        other.entry = nullptr;
    }
    return *this;
}

// ReadAheadPageHandler::~ReadAheadPageHandler() {
//     if (cache && entry)
//         cache->sub_page_ref(this->entry);
// }

// ReadAheadPageHandler& ReadAheadPageHandler::operator=(ReadAheadPageHandler&& other) noexcept {
//     if (this != &other) {
//         if (cache && entry) {
//             cache->sub_page_ref(this->entry);
//         }
//         cache = other.cache;
//         entry = other.entry;
//         other.cache = nullptr;
//         other.entry = nullptr;
//     }
//     return *this;
// }

HnswPageCache::HnswPageCache(const std::string &location, size_t page_size, size_t cache_size, HnswPageCacheDispatcher *dispatcher, std::vector<SSDChannelMetrics> *channel_metrics) {
    this->dispatcher = dispatcher;
    this->channel_metrics = channel_metrics;
    this->location = location;
    this->page_size = page_size;
    size_t max_page_count = cache_size / page_size;

    page_data_pool = std::make_unique<char[]>(max_page_count * page_size);
    page_entries = std::unique_ptr<PageEntry[]>(new PageEntry[max_page_count]);
    page_count = max_page_count;
    for (size_t i = 0; i < max_page_count; i++) {
        page_entries[i].data = page_data_pool.get() + i * page_size;
        avail_page_entries.push_back(&page_entries[i]);
    }

    TAILQ_INIT(&history_list);
    TAILQ_INIT(&buffer_list);

    // Open file once and keep it cached
    fd = ::open(location.c_str(), O_RDONLY);
    if (fd < 0) {
        throw std::runtime_error("failed to open index file");
    }
}

HnswPageCache::~HnswPageCache() {
    if (fd >= 0) {
        ::close(fd);
    }
}

Lazy<PageHandler> HnswPageCache::get_page(page_id_t page_id, ReqMetrics &req_metrics) {
    // Check if the page is already in cache
    auto it = id_to_page.find(page_id);
    if (it != id_to_page.end()) {
        // Page exists in cache
        ++cache_hits;
        PageEntry* entry = it->second;
        ++entry->access_count;
        add_page_ref(entry);
        req_metrics.off_cpu();
        co_await PageLoadingAwaiter(entry);
        req_metrics.on_cpu();
        co_return PageHandler(dispatcher, entry);
    }

    // Page not in cache, need to load it
    ++cache_miss;
    PageEntry* entry = nullptr;

    // Try to get an available page entry from the pool
    if (!avail_page_entries.empty()) {
        entry = avail_page_entries.back();
        avail_page_entries.pop_back();
    } else {
        entry = co_await evict_one_for_use(req_metrics);
    }

    if (entry) {
        // Initialize the new page entry
        entry->page_id = page_id;

        // don't need to lock entry->mtx here since no other thread can access this entry yet
        entry->state = PageEntry::State::Loading;
        id_to_page[page_id] = entry;
        ++entry->access_count;
        add_page_ref(entry);

        co_await load_from_disk(entry, req_metrics);
        co_return PageHandler(dispatcher, entry);
    }

    // Should not reach here
    throw std::runtime_error("Unexpected failure to get or load page");
}

// std::optional<ReadAheadPageHandler> HnswPageCache::readahead_page(page_id_t page_id) {
//     std::unique_lock<std::mutex> lock(lru_mutex);

//     // Check if the page is already in cache
//     auto it = id_to_page.find(page_id);
//     if (it != id_to_page.end()) {
//         PageEntry* entry = it->second;
//         ++entry->access_count;
//         add_page_ref(entry);
//         return ReadAheadPageHandler(this, entry);
//     }

//     // Page not in cache, need to load it
//     PageEntry* entry = nullptr;

//     // Try to get an available page entry from the pool
//     if (!avail_page_entries.empty()) {
//         entry = avail_page_entries.back();
//         avail_page_entries.pop_back();
//     } else {
//         // No available entries, try to evict one from LRU list
//         if ((entry = evict_one_for_use()) == nullptr) {
//             // No evictable entries, cannot readahead now
//             return std::nullopt;
//         }
//     }

//     if (entry) {
//         // Initialize the new page entry
//         entry->page_id = page_id;

//         // don't need to lock entry->mtx here since no other thread can access this entry yet
//         entry->state = PageEntry::State::Loading;
//         id_to_page[page_id] = entry;
//         ++entry->access_count;
//         add_page_ref(entry);  // for readahead handler
//         add_page_ref(entry);  // for readahead thread, when Loading, entry cannot be evicted

//         // Load page from disk without holding the lru lock
//         lock.unlock();
//         load_from_disk_async(entry);
        
//         return ReadAheadPageHandler(this, entry);
//     }

//     return std::nullopt;
// }

// void HnswPageCache::readahead_page_async(page_id_t page_id) {
//     std::unique_lock<std::mutex> lock(lru_mutex);

//     // Check if the page is already in cache
//     auto it = id_to_page.find(page_id);
//     if (it != id_to_page.end()) {
//         ++it->second->access_count;
//         return;
//     }

//     // Page not in cache, need to load it
//     PageEntry* entry = nullptr;

//     // Try to get an available page entry from the pool
//     if (!avail_page_entries.empty()) {
//         entry = avail_page_entries.back();
//         avail_page_entries.pop_back();
//     } else {
//         // No available entries, try to evict one from LRU list
//         if ((entry = evict_one_for_use()) == nullptr) {
//             // No evictable entries, cannot readahead now
//             return;
//         }
//     }

//     if (entry) {
//         // Initialize the new page entry
//         entry->page_id = page_id;

//         // don't need to lock entry->mtx here since no other thread can access this entry yet
//         entry->state = PageEntry::State::Loading;
//         id_to_page[page_id] = entry;
//         ++entry->access_count;
//         add_page_ref(entry);  // for readahead thread, when Loading, entry cannot be evicted

//         // Load page from disk without holding the lru lock
//         lock.unlock();
//         load_from_disk_async(entry);
//         return;
//     }

//     return;
// }

void HnswPageCache::add_page_ref(PageEntry *entry) {
    ++entry->ref_count;
    // If ref_count was 0, move from lru_list to using_entry
    if (entry->ref_count == 1) {
        pin(entry);
    }
}

void HnswPageCache::sub_page_ref(PageEntry *entry) {
    --entry->ref_count;
    
    // If ref_count becomes 0, move from using_entry to lru_list
    if (entry->ref_count == 0) {
        unpin(entry);
    }
}

void HnswPageCache::pin(PageEntry *entry) {
    assert(entry->ref_count > 0);
    // std::cout << "Pinning page " << entry->page_id << std::endl;
    if (entry->list_head) {
        // std::cout << "Removing Page " << entry->page_id << " from LRU list" << std::endl;
        TAILQ_REMOVE(entry->list_head, entry, entry);
        entry->list_head = nullptr;
    }
}

void HnswPageCache::unpin(PageEntry *entry) {
    // std::cout << "Unpinning page " << entry->page_id << std::endl;
    assert(entry->ref_count == 0);
    if (!entry->list_head) {
        if (entry->access_count < K) {
            TAILQ_INSERT_HEAD(&history_list, entry, entry);
            entry->list_head = &history_list;
        } else {
            TAILQ_INSERT_HEAD(&buffer_list, entry, entry);
            entry->list_head = &buffer_list;
        }

        for (auto h: evict_waiters) {
            h.resume();
        }
        evict_waiters.clear();
    }
}

class EvictAwaiter {
public:
    EvictAwaiter(HnswPageCache *cache): cache(cache) {}

    void await_suspend(std::coroutine_handle<> h) {
        cache->evict_waiters.push_back(h);
    }

    void await_resume() {}

    bool await_ready() {
        return false;
    }

private:
    HnswPageCache *cache;
};

Lazy<PageEntry*> HnswPageCache::evict_one_for_use(ReqMetrics &req_metrics) {
    while (TAILQ_EMPTY(&history_list) && TAILQ_EMPTY(&buffer_list)) {
        req_metrics.off_cpu();
        co_await EvictAwaiter(this);
        req_metrics.on_cpu();
    }
    
    assert(!TAILQ_EMPTY(&history_list) || !TAILQ_EMPTY(&buffer_list));

    if (!TAILQ_EMPTY(&history_list)) {
        PageEntry* entry = TAILQ_LAST((&history_list), ListHead);
        TAILQ_REMOVE(&history_list, entry, entry);
        id_to_page.erase(entry->page_id);
        entry->release();
        co_return entry;
    }

    if (!TAILQ_EMPTY(&buffer_list)) {
        PageEntry* entry = TAILQ_LAST((&buffer_list), ListHead);
        TAILQ_REMOVE(&buffer_list, entry, entry);
        id_to_page.erase(entry->page_id);
        entry->release();
        co_return entry;
    }
}

Lazy<void> HnswPageCache::load_from_disk(PageEntry *entry, ReqMetrics &req_metrics) {
    off_t offset = static_cast<off_t>(entry->page_id) * page_size;

    req_metrics.off_cpu();
    auto channel_id = entry->page_id % ssd_channel_num;
    (*channel_metrics)[channel_id].add_req();
    auto bytes_read = co_await HnswExecutor::UringContext::current_executor()
        .async_read(fd, entry->data, static_cast<unsigned>(page_size), offset, (*channel_metrics)[channel_id]);
    req_metrics.on_cpu();

    req_metrics.add_io();
    ++io_op_num;
    memory_transfer_bytes += static_cast<size_t>(bytes_read);
    if (bytes_read < 0) {
        throw std::runtime_error("failed to read page from disk");
    }

    entry->state = PageEntry::State::InCache;
    for (auto h: entry->waiters) {
        h.resume();
    }
    entry->waiters.clear();
}

// void HnswPageCache::load_from_disk_async(PageEntry *entry) {
//     off_t offset = static_cast<off_t>(entry->page_id) * page_size;
//     auto io_task = std::make_unique<HnswIOTask>(
//         fd,
//         entry->data,
//         offset,
//         page_size,
//         [this, entry]() {
//             ++io_op_num;
//             memory_transfer_bytes += page_size;
//             auto old_state = entry->state.exchange(PageEntry::State::InCache, std::memory_order_release);
//             if (old_state == PageEntry::State::LoadingHasWaiter)
//                 entry->state.notify_all();
//             sub_page_ref(entry);
//         }
//     );
//     io_backend.submit_io_task(std::move(io_task));
// }

}