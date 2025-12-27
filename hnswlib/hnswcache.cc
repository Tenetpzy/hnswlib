#include "hnswcache.h"
#include <atomic>
#include <cassert>
#include <fcntl.h>
#include <mutex>
#include <stdexcept>
#include <unistd.h>

namespace hnswlib {

PageHandler::~PageHandler() {
    if (cache && entry)
        cache->sub_page_ref(this->entry);
}

PageHandler& PageHandler::operator=(PageHandler&& other) noexcept {
    if (this != &other) {
        if (cache && entry) {
            cache->sub_page_ref(this->entry);
        }
        cache = other.cache;
        entry = other.entry;
        other.cache = nullptr;
        other.entry = nullptr;
    }
    return *this;
}

ReadAheadPageHandler::~ReadAheadPageHandler() {
    if (cache && entry)
        cache->sub_page_ref(this->entry);
}

ReadAheadPageHandler& ReadAheadPageHandler::operator=(ReadAheadPageHandler&& other) noexcept {
    if (this != &other) {
        if (cache && entry) {
            cache->sub_page_ref(this->entry);
        }
        cache = other.cache;
        entry = other.entry;
        other.cache = nullptr;
        other.entry = nullptr;
    }
    return *this;
}

HnswPageCache::HnswPageCache(const std::string &location, size_t page_size, size_t cache_size) {
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

    TAILQ_INIT(&lru_list);

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

PageHandler HnswPageCache::get_page(page_id_t page_id) {
    std::unique_lock<std::mutex> lock(lru_mutex);

    // Check if the page is already in cache
    auto it = id_to_page.find(page_id);
    if (it != id_to_page.end()) {
        // Page exists in cache
        ++cache_hits;
        PageEntry* entry = it->second;
        add_page_ref(entry);
        lock.unlock();

        // Wait for page to be ready if it's still loading
        entry->wait_until_ready();
        return PageHandler(this, entry);
    }

    // Page not in cache, need to load it
    ++cache_miss;
    PageEntry* entry = nullptr;

    // Try to get an available page entry from the pool
    if (!avail_page_entries.empty()) {
        entry = avail_page_entries.back();
        avail_page_entries.pop_back();
    } else {
        // No available entries, try to evict one from LRU list
        if (!TAILQ_EMPTY(&lru_list)) {
            entry = evict_one_for_use();
        } else {
            // No evictable entries, wait until one becomes available
            evict_cv.wait(lock, [this] { return !TAILQ_EMPTY(&lru_list); });
            // After waking up, evict from LRU list (guaranteed to be non-empty)
            entry = evict_one_for_use();
        }
    }

    if (entry) {
        // Initialize the new page entry
        entry->page_id = page_id;

        // don't need to lock entry->mtx here since no other thread can access this entry yet
        entry->state = PageEntry::State::Loading;
        id_to_page[page_id] = entry;
        add_page_ref(entry);

        // Load page from disk without holding the lru lock
        lock.unlock();
        load_from_disk(entry);

        return PageHandler(this, entry);
    }

    // Should not reach here
    throw std::runtime_error("Unexpected failure to get or load page");
}

std::optional<ReadAheadPageHandler> HnswPageCache::readahead_page(page_id_t page_id) {
    std::unique_lock<std::mutex> lock(lru_mutex);

    // Check if the page is already in cache
    auto it = id_to_page.find(page_id);
    if (it != id_to_page.end()) {
        PageEntry* entry = it->second;
        add_page_ref(entry);
        return ReadAheadPageHandler(this, entry);
    }

    // Page not in cache, need to load it
    PageEntry* entry = nullptr;

    // Try to get an available page entry from the pool
    if (!avail_page_entries.empty()) {
        entry = avail_page_entries.back();
        avail_page_entries.pop_back();
    } else {
        // No available entries, try to evict one from LRU list
        if (!TAILQ_EMPTY(&lru_list)) {
            entry = evict_one_for_use();
        } else {
            // No evictable entries, cannot readahead now
            return std::nullopt;
        }
    }

    if (entry) {
        // Initialize the new page entry
        entry->page_id = page_id;

        // don't need to lock entry->mtx here since no other thread can access this entry yet
        entry->state = PageEntry::State::Loading;
        id_to_page[page_id] = entry;
        add_page_ref(entry);  // for readahead handler
        add_page_ref(entry);  // for readahead thread, when Loading, entry cannot be evicted

        // Load page from disk without holding the lru lock
        lock.unlock();
        load_from_disk_async(entry);
        
        return ReadAheadPageHandler(this, entry);
    }

    return std::nullopt;
}

void HnswPageCache::readahead_page_async(page_id_t page_id) {
    std::unique_lock<std::mutex> lock(lru_mutex);

    // Check if the page is already in cache
    auto it = id_to_page.find(page_id);
    if (it != id_to_page.end()) {
        return;
    }

    // Page not in cache, need to load it
    PageEntry* entry = nullptr;

    // Try to get an available page entry from the pool
    if (!avail_page_entries.empty()) {
        entry = avail_page_entries.back();
        avail_page_entries.pop_back();
    } else {
        // No available entries, try to evict one from LRU list
        if (!TAILQ_EMPTY(&lru_list)) {
            entry = evict_one_for_use();
        } else {
            // No evictable entries, cannot readahead now
            return;
        }
    }

    if (entry) {
        // Initialize the new page entry
        entry->page_id = page_id;

        // don't need to lock entry->mtx here since no other thread can access this entry yet
        entry->state = PageEntry::State::Loading;
        id_to_page[page_id] = entry;
        add_page_ref(entry);  // for readahead thread, when Loading, entry cannot be evicted

        // Load page from disk without holding the lru lock
        lock.unlock();
        load_from_disk_async(entry);
        return;
    }

    return;
}

// caller should hold lru_mutex
void HnswPageCache::add_page_ref(PageEntry *entry) {
    auto ori = entry->ref_count.fetch_add(1, std::memory_order_relaxed);
    // If ref_count was 0, move from lru_list to using_entry
    if (ori == 0) {
        pin(entry);
    }
}

// caller should not hold any lock
void HnswPageCache::sub_page_ref(PageEntry *entry) {
    auto ori = entry->ref_count.fetch_sub(1, std::memory_order_relaxed);
    
    // If ref_count becomes 0, move from using_entry to lru_list
    if (ori == 1) {
        std::unique_lock<std::mutex> lock(lru_mutex);
        if (entry->ref_count == 0) {
            unpin(entry);
        }
    }
}

// caller should hold lru_mutex
void HnswPageCache::pin(PageEntry *entry) {
    assert(entry->ref_count > 0);
    // std::cout << "Pinning page " << entry->page_id << std::endl;
    if (entry->in_lru_list) {
        // std::cout << "Removing Page " << entry->page_id << " from LRU list" << std::endl;
        TAILQ_REMOVE(&lru_list, entry, entry);
        entry->in_lru_list = false;
    }
}

// caller should hold lru_mutex
void HnswPageCache::unpin(PageEntry *entry) {
    // std::cout << "Unpinning page " << entry->page_id << std::endl;
    assert(entry->ref_count == 0);
    if (!entry->in_lru_list) {
        TAILQ_INSERT_HEAD(&lru_list, entry, entry);
        entry->in_lru_list = true;
    }

    // Notify evict_cv that an evictable entry is available
    evict_cv.notify_one();
}

// caller should hold lru_mutex and ensure lru_list is not empty
PageEntry* HnswPageCache::evict_one_for_use() {
    PageEntry* entry = TAILQ_LAST((&lru_list), LRUList);
    TAILQ_REMOVE(&lru_list, entry, entry);
    id_to_page.erase(entry->page_id);
    entry->release();
    return entry;
}

void HnswPageCache::load_from_disk(PageEntry *entry) {
    // Use pread for thread-safe, atomic read from specific offset
    off_t offset = static_cast<off_t>(entry->page_id) * page_size;
    ssize_t bytes_read = ::pread(fd, entry->data, page_size, offset);
    ++io_op_num;
    memory_transfer_bytes += static_cast<size_t>(bytes_read);
    auto old_state = entry->state.exchange(PageEntry::State::InCache, std::memory_order_release);
    if (old_state == PageEntry::State::LoadingHasWaiter)
        entry->state.notify_all();
    if (bytes_read < 0) {
        throw std::runtime_error("failed to read page from disk");
    }
}

void HnswPageCache::load_from_disk_async(PageEntry *entry) {
    off_t offset = static_cast<off_t>(entry->page_id) * page_size;
    auto io_task = std::make_unique<HnswIOTask>(
        fd,
        entry->data,
        offset,
        page_size,
        [this, entry]() {
            ++io_op_num;
            memory_transfer_bytes += page_size;
            auto old_state = entry->state.exchange(PageEntry::State::InCache, std::memory_order_release);
            if (old_state == PageEntry::State::LoadingHasWaiter)
                entry->state.notify_all();
            sub_page_ref(entry);
        }
    );
    io_backend.submit_io_task(std::move(io_task));
}

}