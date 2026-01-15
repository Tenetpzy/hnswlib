#pragma once

#include <coroutine>
#include <cstdint>
#include <deque>
#include <sys/queue.h>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "async_simple/coro/Lazy.h"
#include "executor.h"

namespace hnswlib {

static constexpr size_t CACHE_LINE_SIZE = 64;

typedef unsigned int page_id_t;
class HnswPageCache;
class HnswPageCacheDispatcher;
TAILQ_HEAD(ListHead, PageEntry);

using async_simple::coro::Lazy;

class PageEntry {
    enum class State {
        InCache,
        Loading,
        LoadingHasWaiter,
        NotInCache
    };

    uint32_t ref_count;
    State state;
    char *data;
    ListHead *list_head;  // which list the entry is in, nullptr if using
    uint32_t access_count;  // for LRU-K
    page_id_t page_id;
    TAILQ_ENTRY(PageEntry) entry;
    std::vector<std::coroutine_handle<>> waiters;

    PageEntry(): ref_count(0), state(State::NotInCache), data(nullptr), list_head(nullptr), access_count(0) {}

    void release() {
        ref_count = 0;
        state = State::NotInCache;
        list_head = nullptr;
        access_count = 0;
    }

    // void wait_until_ready() {
    //     state only has Loading -> LoadingHasWaiter -> InCache tranfer
    //     auto state_val = state.load(std::memory_order_acquire);
    //     if (state_val == State::InCache) {
    //         return;
    //     } else if (state_val == State::Loading) {
    //         if (state.compare_exchange_strong(state_val, State::LoadingHasWaiter, std::memory_order_seq_cst)) {
    //             // changed state Loading -> LoadingHasWaiter
    //             state.wait(State::LoadingHasWaiter, std::memory_order_acquire);
    //         } else if (state_val == State::LoadingHasWaiter) {
    //             // other thread changed state Loading -> LoadingHasWaiter
    //             state.wait(State::LoadingHasWaiter, std::memory_order_acquire);
    //         } 
    //         // else if state_val == InCache, just return
    //     } else {  // state_val == LoadingHasWaiter
    //         state.wait(State::LoadingHasWaiter, std::memory_order_acquire);
    //     }
    // }

    friend class HnswPageCache;
    friend class PageHandler;
    friend class ReadAheadPageHandler;
    friend class PageLoadingAwaiter;
    friend class HnswPageCacheDispatcher;
};

/*
 * A PageHandler adds reference count by 1 to a page in HnswPageCache
 * sub refcount by 1 when PageHandler destroyed(Actually is an RAII of a page reference count)
 * Only the reference of page equals to zero, the page can be evicted from cache
 */
class PageHandler {
public:

    // Should call sub_page_ref of HnswPageCache to decrease the reference count
    ~PageHandler();

    PageHandler(const PageHandler&) = delete;
    PageHandler& operator=(const PageHandler&) = delete;
    PageHandler(PageHandler&& other) noexcept: cache(other.cache), entry(other.entry) {
        other.cache = nullptr;
        other.entry = nullptr;
    }
    PageHandler& operator=(PageHandler&& other) noexcept;

    // Just for simplify, don't consider safety
    char* get_ptr() const noexcept {
        return entry->data;
    }

private:
    // Once PageHandler is created, it increases the reference count of the page in cache
    // Note for the race condition of evict and reference increase when you implement HnswPageCache
    PageHandler(HnswPageCacheDispatcher *cache, PageEntry *entry): cache(cache), entry(entry) {}

    HnswPageCacheDispatcher *cache;
    PageEntry *entry;

    friend class HnswPageCache;
    friend class ReadAheadPageHandler;
};

/*
 * The return handler of readahead_page
 * Caller can wait for page ready through this handler
 */
// class ReadAheadPageHandler {
// public:
//     // Should call sub_page_ref of HnswPageCache to decrease the reference count
//     ~ReadAheadPageHandler();

//     ReadAheadPageHandler(const ReadAheadPageHandler&) = delete;
//     ReadAheadPageHandler& operator=(const ReadAheadPageHandler&) = delete;
//     ReadAheadPageHandler(ReadAheadPageHandler&& other) noexcept: cache(other.cache), entry(other.entry) {
//         other.cache = nullptr;
//         other.entry = nullptr;
//     }
//     ReadAheadPageHandler& operator=(ReadAheadPageHandler&& other) noexcept;

//     // Wait until the page is ready in cache
//     PageHandler wait_ready() && {
//         if (entry) {
//             entry->wait_until_ready();
//         }
//         auto ret = PageHandler(cache, entry);
//         cache = nullptr;
//         entry = nullptr;
//         return ret;
//     }

// private:
//     ReadAheadPageHandler(HnswPageCache *cache, PageEntry *entry): cache(cache), entry(entry) {}

//     HnswPageCache *cache;
//     PageEntry *entry;

//     friend class HnswPageCache;
// };

/*
 * HnswPageCache manages the pages of HNSW index file on disk
 */
class HnswPageCache {
public:
    /*
     * location: hnsw index file location
     * page_size: size in byte of each page
     * cache_size: total available byte for cache
     */
    HnswPageCache(const std::string &location, size_t page_size, size_t cache_size, HnswPageCacheDispatcher *dispatcher);
    ~HnswPageCache();

    /*
     * Get page by page id 
     * If the page is not in cache, load it from disk, block caller until page ready, the page's refcount should be initialized as 1
     *
     * If Cache is full, evict some page with refcount zero to make space for new page
     *
     * If the page is in readying status, block caller until page ready, then increase the page's refcount by 1
     * (If multiple threads try to get the same page which is not in cache, only one thread loads it from disk, other threads block until page ready)
     * 
     * If the page is in cache and ready, just increase the page's refcount by 1
     */
    Lazy<PageHandler> get_page(page_id_t page_id);

    /*
     * Async prefetch the page into cache, should not block caller
     * If the page is already in cache, just return the handler with increased refcount
     * If the page is not in cache, load it from disk asynchronously
     * If the cache is full and cannot be evicted, just return empty optional
     * ReadAheadPageHandler holds an reference count of page
     * Caller can wait on the ReadAheadPageHandler until page ready
     */
    // std::optional<ReadAheadPageHandler> readahead_page(page_id_t page_id);
    // void readahead_page_async(page_id_t page_id);

    size_t get_avail_page_count() const {
        return avail_page_entries.size();
    }

    float get_cache_hit_rate() const {
        size_t total = cache_hits + cache_miss;
        if (total == 0) return 0.0f;
        return static_cast<float>(cache_hits) / static_cast<float>(total);
    }

    size_t get_io_op_num() const {
        return io_op_num;
    }

    float get_memory_transfer_kb() const {
        return static_cast<float>(memory_transfer_bytes) / 1024.0f;
    }

    void reset_metrics_counter() {
        cache_hits = 0;
        cache_miss = 0;
        io_op_num = 0;
        memory_transfer_bytes = 0;
    }

private:
    /*
     * Decrease the reference count of the page
     * If the reference count reaches zero, the page can be evicted from cache
     */
    void sub_page_ref(PageEntry *entry);

    /*
     * Increase the reference count of the page
     */
    void add_page_ref(PageEntry *entry);

    void pin(PageEntry *entry);

    void unpin(PageEntry *entry);

    Lazy<PageEntry*> evict_one_for_use();

    Lazy<void> load_from_disk(PageEntry *entry);

    void load_from_disk_async(PageEntry *entry);

private:
    static constexpr int K = 3;  // LRU-K

    HnswPageCacheDispatcher *dispatcher;

    std::string location;

    std::unique_ptr<char[]> page_data_pool;
    size_t page_size;

    int fd; // cached file descriptor for loading pages

    std::unique_ptr<PageEntry[]> page_entries;
    size_t page_count;
    std::vector<PageEntry*> avail_page_entries;

    // lists of evictable page entries (ref_count == 0)
    ListHead buffer_list;
    ListHead history_list;
    std::unordered_map<page_id_t, PageEntry*> id_to_page; // map page_id to list or using entry iterator
    std::deque<std::coroutine_handle<>> evict_waiters;

    size_t cache_hits{0}, cache_miss{0};
    size_t io_op_num{0};
    size_t memory_transfer_bytes{0};

    friend class ReadAheadPageHandler;
    friend class EvictAwaiter;
    friend class HnswPageCacheDispatcher;
};

class HnswPageCacheDispatcher {
public:
    HnswPageCacheDispatcher(std::vector<HnswExecutor::UringExecutor*> &executors, 
        const std::string &location, size_t page_size, size_t cache_size)
        : executors(executors) {
        size_t per_cache_size = cache_size / executors.size();
        for (size_t i = 0; i < executors.size(); i++) {
            caches.emplace_back(std::make_unique<HnswPageCache>(location, page_size, per_cache_size, this));
        }
    }

    Lazy<PageHandler> get_page(page_id_t page_id) {
        size_t idx = page_id % executors.size();
        if (executors[idx]->currentThreadInExecutor()) {
            co_return co_await caches[idx]->get_page(page_id);
        } else {
            // via返回rescheduledLazy，它始终会把当前协程挂起并在另一个executor启动
            co_return co_await caches[idx]->get_page(page_id).via(executors[idx]);
        }
    }

    const HnswExecutor::UringExecutor* run_on(page_id_t page_id) const {
        size_t idx = page_id % executors.size();
        return executors[idx];
    }

    float get_cache_hit_rate() const {
        size_t total_hits = 0;
        size_t total_requests = 0;
        for (const auto &cache : caches) {
            total_hits += cache->cache_hits;
            total_requests += cache->cache_hits + cache->cache_miss;
        }
        if (total_requests == 0) return 0.0f;
        return static_cast<float>(total_hits) / static_cast<float>(total_requests);
    }

    float get_memory_transfer_kb() const {
        size_t total_bytes = 0;
        for (const auto &cache : caches) {
            total_bytes += cache->memory_transfer_bytes;
        }
        return static_cast<float>(total_bytes) / 1024.0f;
    }

    size_t get_io_op_num() const {
        size_t total_io_op = 0;
        for (const auto &cache : caches) {
            total_io_op += cache->io_op_num;
        }
        return total_io_op;
    }

    void reset_metrics_counter() {
        for (const auto &cache : caches) {
            cache->reset_metrics_counter();
        }
    }

private:

    Lazy<void> put_page_coro(PageEntry *entry, HnswPageCache *cache) {
        cache->sub_page_ref(entry);
        co_return;
    }

    void put_page(PageEntry *entry) {
        size_t idx = entry->page_id % executors.size();
        if (executors[idx]->currentThreadInExecutor()) {
            caches[idx]->sub_page_ref(entry);
        } else {
            auto cache = caches[idx].get();
            put_page_coro(entry, cache).via(executors[idx]).start();
        }
    }

private:
    std::vector<HnswExecutor::UringExecutor*> &executors;
    std::vector<std::unique_ptr<HnswPageCache>> caches;

    friend class PageHandler;
};

} // namespace hnswlib
