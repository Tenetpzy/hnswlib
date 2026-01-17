#pragma once

#include <sys/queue.h>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <cmath>
#include "metric.h"

namespace hnswlib {

typedef unsigned int page_id_t;
class HnswPageCache;

class PageEntry {
    enum class State {
        InCache,
        Loading,
        NotInCache
    };

    page_id_t page_id;
    char *data;
    std::atomic_uint32_t ref_count;

    TAILQ_ENTRY(PageEntry) entry;
    bool in_lru_list;  // protected by lru lock

    State state;
    std::condition_variable cv; // for waiting entry loading threads
    std::mutex mtx; // for state and cv

    PageEntry(): data(nullptr), ref_count(0), in_lru_list(false), state(State::NotInCache) {}

    void release() {
        ref_count = 0;
        in_lru_list = false;
        state = State::NotInCache;
    }

    friend class HnswPageCache;
    friend class PageHandler;
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
    PageHandler(HnswPageCache *cache, PageEntry *entry): cache(cache), entry(entry) {}

    HnswPageCache *cache;
    PageEntry *entry;

    friend class HnswPageCache;
};

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
    HnswPageCache(const std::string &location, size_t page_size, size_t cache_size);
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
    PageHandler get_page(page_id_t page_id, ReqMetrics& req_metrics);

    // /*
    //  * Async prefetch the page into cache, should not block caller
    //  * Note: You should implement a threadpool or something to support async loading
    //  * If the page is already in cache, do nothing
    //  * If the page is not in cache, load it from disk asynchronously
    //  * The page's refcount should be initialized as 0
    //  */
    // void readahead_page(page_id_t page_id);

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

    double get_avg_depth_mean() const {
        if (channel_metrics.empty()) return 0.0;
        double sum = 0.0;
        for (const auto &metrics : channel_metrics) {
            sum += metrics.get_avg_depth();
        }
        return sum / channel_metrics.size();
    }

    double get_avg_depth_std() const {
        if (channel_metrics.empty()) return 0.0;
        double mean = get_avg_depth_mean();
        double variance_sum = 0.0;
        for (const auto &metrics : channel_metrics) {
            double diff = metrics.get_avg_depth() - mean;
            variance_sum += diff * diff;
        }
        return std::sqrt(variance_sum / channel_metrics.size());
    }

    void reset_metrics_counter() {
        cache_hits = 0;
        cache_miss = 0;
        io_op_num = 0;
        memory_transfer_bytes = 0;
        for (auto &metrics : channel_metrics) {
            metrics.reset();
        }
    }

    size_t get_page_num() const {
        return page_count;
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

    PageEntry* evict_one_for_use();

    void load_from_disk(PageEntry *entry);

    TAILQ_HEAD(LRUList, PageEntry);

private:
    std::string location;

    std::unique_ptr<char[]> page_data_pool;
    size_t page_size;

    int fd; // cached file descriptor for loading pages

    std::unique_ptr<PageEntry[]> page_entries;
    size_t page_count;
    std::vector<PageEntry*> avail_page_entries;

    LRUList lru_list; // list of evictable page entries (ref_count == 0)
    std::unordered_map<page_id_t, PageEntry*> id_to_page; // map page_id to lru_list or using entry iterator

    std::condition_variable evict_cv; // for evicting thread to wait for evictable page entry
    std::mutex lru_mutex;
    std::vector<SSDChannelMetrics> channel_metrics;

    size_t cache_hits{0}, cache_miss{0};
    std::atomic_size_t io_op_num{0};
    std::atomic_size_t memory_transfer_bytes{0};

    friend class PageHandler;
};

} // namespace hnswlib
