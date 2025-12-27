#pragma once

#include "hnswcache.h"
#include "visited_list_pool.h"
#include "hnswlib.h"
#include <atomic>
#include <climits>
#include <cstddef>
#include <random>
#include <stdlib.h>
#include <assert.h>
#include <unordered_set>
#include <memory>
#include <utility>

namespace hnswlib {
typedef unsigned int tableint;
typedef unsigned int linklistsizeint;

static const unsigned char DELETE_MARK = 0x01;
template<typename dist_t>
class HierarchicalNSW;

class PointPageLevel0 {
public:
    template<typename dist_t>
    PointPageLevel0(PageHandler& ph, size_t base_offset, const HierarchicalNSW<dist_t> *hnsw)
        : page_handler(ph) {
        neighbor_count_offset = base_offset + hnsw->offsetLevel0_;
        neighbor_list_offset = neighbor_count_offset + 4;
        data_offset = base_offset + hnsw->offsetData_;
        label_offset = base_offset + hnsw->label_offset_;
        del_flag_offset = base_offset + hnsw->offsetLevel0_ + 2;
    }

    tableint *get_neighbor_list() const {
        return (tableint *)(page_handler.get_ptr() + neighbor_list_offset);
    }

    int get_neighbor_count() const {
        int count = *((unsigned short int *)(page_handler.get_ptr() + neighbor_count_offset));
        return count;
    }

    labeltype get_label() const {
        labeltype label;
        memcpy(&label, page_handler.get_ptr() + label_offset, sizeof(labeltype));
        return label;
    }

    char *get_data() const {
        return page_handler.get_ptr() + data_offset;
    }

    bool is_deleted() const {
        unsigned char del_flag = *((unsigned char *)(page_handler.get_ptr() + del_flag_offset)) & DELETE_MARK;
        return del_flag;
    }

private:
    PageHandler& page_handler;
    int neighbor_count_offset;
    int neighbor_list_offset;
    int data_offset;
    int label_offset;
    int del_flag_offset;
};

class PointPageHigherLevel {
public:
    PointPageHigherLevel(PageHandler&& ph, size_t base_offset)
        : page_handler(std::move(ph)) {
        neighbor_count_offset = base_offset;
        neighbor_list_offset = neighbor_count_offset + 4;
    }

    tableint *get_neighbor_list() const {
        return (tableint *)(page_handler.get_ptr() + neighbor_list_offset);
    }

    int get_neighbor_count() const {
        int count = *((int *)(page_handler.get_ptr() + neighbor_count_offset));
        return count;
    }

private:
    PageHandler page_handler;
    int neighbor_count_offset;
    int neighbor_list_offset;
};

template<typename dist_t>
class HierarchicalNSW : public AlgorithmInterface<dist_t> {
 public:
    static const tableint MAX_LABEL_OPERATION_LOCKS = 65536;

    size_t max_elements_{0};
    mutable std::atomic<size_t> cur_element_count{0};  // current number of elements
    size_t size_data_per_element_{0};
    size_t size_links_per_element_{0};
    mutable std::atomic<size_t> num_deleted_{0};  // number of deleted elements
    size_t M_{0};
    size_t maxM_{0};
    size_t maxM0_{0};
    size_t ef_construction_{0};
    size_t ef_{ 0 };

    double mult_{0.0}, revSize_{0.0};
    int maxlevel_{0};

    std::unique_ptr<VisitedListPool> visited_list_pool_{nullptr};

    // Locks operations with element by label value
    mutable std::vector<std::mutex> label_op_locks_;

    std::mutex global;
    std::vector<std::mutex> link_list_locks_;

    tableint enterpoint_node_{0};

    size_t size_links_level0_{0};
    size_t offsetData_{0}, offsetLevel0_{0}, label_offset_{ 0 };

    char *data_level0_memory_{nullptr};
    char **linkLists_{nullptr};
    std::vector<int> element_levels_;  // keeps level of each element

    size_t data_size_{0};

    DISTFUNC<dist_t> fstdistfunc_;
    void *dist_func_param_{nullptr};

    mutable std::mutex label_lookup_lock;  // lock for label_lookup_
    std::unordered_map<labeltype, tableint> label_lookup_;

    std::default_random_engine level_generator_;
    std::default_random_engine update_probability_generator_;

    mutable std::atomic<long> metric_distance_computations{0};
    mutable std::atomic<long> metric_hops{0};

    bool allow_replace_deleted_ = false;  // flag to replace deleted elements (marked as deleted) during insertions

    std::mutex deleted_elements_lock;  // lock for deleted_elements
    std::unordered_set<tableint> deleted_elements;  // contains internal ids of deleted elements

    // Page cache and on-demand loading support
    std::unique_ptr<HnswPageCache> page_cache;
    size_t page_size_{0};  // page size for on-demand loading
    size_t level0_elements_per_page_{0};  // number of elements per page in level 0
    page_id_t level0_first_page_id_{0};  // page id of the first element in level 0
    page_id_t link_offset_array_page_id_{0};  // page id of the link offset array

    std::vector<tableint> id_to_store_order;  // map internal id to store id
    std::vector<tableint> store_order_to_id;  // map store id to internal id

    HierarchicalNSW(
        SpaceInterface<dist_t> *s,
        const std::string &location,
        size_t cache_size = 0,
        bool nmslib = false,
        size_t max_elements = 0,
        bool allow_replace_deleted = false)
        : allow_replace_deleted_(allow_replace_deleted) {
        loadIndex(location, s, max_elements, cache_size);
    }


    HierarchicalNSW(
        SpaceInterface<dist_t> *s,
        size_t max_elements,
        size_t M = 16,
        size_t ef_construction = 200,
        size_t page_size = 4096,
        size_t random_seed = 100,
        bool allow_replace_deleted = false)
        : label_op_locks_(MAX_LABEL_OPERATION_LOCKS),
            link_list_locks_(max_elements),
            element_levels_(max_elements),
            page_size_(page_size),
            allow_replace_deleted_(allow_replace_deleted) {
        max_elements_ = max_elements;
        num_deleted_ = 0;
        data_size_ = s->get_data_size();
        fstdistfunc_ = s->get_dist_func();
        dist_func_param_ = s->get_dist_func_param();
        if ( M <= 10000 ) {
            M_ = M;
        } else {
            HNSWERR << "warning: M parameter exceeds 10000 which may lead to adverse effects." << std::endl;
            HNSWERR << "         Cap to 10000 will be applied for the rest of the processing." << std::endl;
            M_ = 10000;
        }
        maxM_ = M_;
        maxM0_ = M_ * 2;
        ef_construction_ = std::max(ef_construction, M_);
        ef_ = 10;

        level_generator_.seed(random_seed);
        update_probability_generator_.seed(random_seed + 1);

        size_links_level0_ = maxM0_ * sizeof(tableint) + sizeof(linklistsizeint);
        size_data_per_element_ = size_links_level0_ + data_size_ + sizeof(labeltype);
        offsetData_ = size_links_level0_;
        label_offset_ = size_links_level0_ + data_size_;
        offsetLevel0_ = 0;

        data_level0_memory_ = (char *) malloc(max_elements_ * size_data_per_element_);
        if (data_level0_memory_ == nullptr)
            throw std::runtime_error("Not enough memory");

        cur_element_count = 0;

        visited_list_pool_ = std::unique_ptr<VisitedListPool>(new VisitedListPool(1, max_elements));

        // initializations for special treatment of the first node
        enterpoint_node_ = -1;
        maxlevel_ = -1;

        linkLists_ = (char **) malloc(sizeof(void *) * max_elements_);
        if (linkLists_ == nullptr)
            throw std::runtime_error("Not enough memory: HierarchicalNSW failed to allocate linklists");
        size_links_per_element_ = maxM_ * sizeof(tableint) + sizeof(linklistsizeint);
        mult_ = 1 / log(1.0 * M_);
        revSize_ = 1.0 / mult_;
    }


    ~HierarchicalNSW() {
        clear();
    }

    void clear() {
        if (data_level0_memory_ != nullptr) {
            free(data_level0_memory_);
            data_level0_memory_ = nullptr;
        }
        if (element_levels_.size()) {
            for (tableint i = 0; i < cur_element_count; i++) {
                if (element_levels_[i] > 0)
                    free(linkLists_[i]);
            }
        }
        if (linkLists_ != nullptr) {
            free(linkLists_);
            linkLists_ = nullptr;
        }
        cur_element_count = 0;
        visited_list_pool_.reset(nullptr);
    }


    struct CompareByFirst {
        constexpr bool operator()(std::pair<dist_t, tableint> const& a,
            std::pair<dist_t, tableint> const& b) const noexcept {
            return a.first < b.first;
        }
    };


    void setEf(size_t ef) {
        ef_ = ef;
    }

    // return: (page_id, offset_in_page)
    std::pair<page_id_t, size_t> get_level0_offset(tableint internal_id) const {
        page_id_t pageid = level0_first_page_id_ + internal_id / level0_elements_per_page_;
        size_t offset_in_page = (internal_id % level0_elements_per_page_) * size_data_per_element_;
        return std::make_pair(pageid, offset_in_page);
    }

    // return the link list offset of internal_id
    std::pair<page_id_t, size_t> get_higher_level_offset(tableint internal_id, int level) const {
        // Calculate page and offset for the link list
        size_t link_offset_entry_offset = internal_id * sizeof(size_t);
        size_t link_offset_entry_page_id = link_offset_array_page_id_ + link_offset_entry_offset / page_size_;
        size_t link_offset_entry_offset_in_page = link_offset_entry_offset % page_size_;

        // Load the page containing the link offset entry
        auto link_array_page = page_cache->get_page(link_offset_entry_page_id);

        // Get the offset of this element's link list
        size_t *link_offset_ptr = (size_t *)(link_array_page.get_ptr() + link_offset_entry_offset_in_page);
        size_t link_offset = *link_offset_ptr;

        // Calculate page and offset for the link list
        size_t page_id = link_offset / page_size_;
        size_t offset_in_page = link_offset % page_size_ + (level - 1) * size_links_per_element_;

        return std::make_pair(page_id, offset_in_page);
    }

    inline std::mutex& getLabelOpMutex(labeltype label) const {
        // calculate hash
        size_t lock_id = label & (MAX_LABEL_OPERATION_LOCKS - 1);
        return label_op_locks_[lock_id];
    }


    inline labeltype getExternalLabel(tableint internal_id) const {
        labeltype return_label;
        memcpy(&return_label, (data_level0_memory_ + internal_id * size_data_per_element_ + label_offset_), sizeof(labeltype));
        return return_label;
    }


    inline void setExternalLabel(tableint internal_id, labeltype label) const {
        memcpy((data_level0_memory_ + internal_id * size_data_per_element_ + label_offset_), &label, sizeof(labeltype));
    }


    inline labeltype *getExternalLabeLp(tableint internal_id) const {
        return (labeltype *) (data_level0_memory_ + internal_id * size_data_per_element_ + label_offset_);
    }


    inline char *getDataByInternalId(tableint internal_id) const {
        return (data_level0_memory_ + internal_id * size_data_per_element_ + offsetData_);
    }


    int getRandomLevel(double reverse_size) {
        std::uniform_real_distribution<double> distribution(0.0, 1.0);
        double r = -log(distribution(level_generator_)) * reverse_size;
        return (int) r;
    }

    size_t getMaxElements() {
        return max_elements_;
    }

    size_t getCurrentElementCount() {
        return cur_element_count;
    }

    size_t getDeletedCount() {
        return num_deleted_;
    }

    float get_cache_hit_rate() const {
        if (page_cache) {
            return page_cache->get_cache_hit_rate();
        }
        return 0.0f;
    }

    size_t get_io_op_num() const {
        if (page_cache) {
            return page_cache->get_io_op_num();
        }
        return 0;
    }

    float get_memory_transfer_kb() const {
        if (page_cache) {
            return page_cache->get_memory_transfer_kb();
        }
        return 0.0f;
    }

    void reset_metrics_counter() {
        if (page_cache) {
            page_cache->reset_metrics_counter();
        }
    }

    std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst>
    searchBaseLayer(tableint ep_id, const void *data_point, int layer) {
        VisitedList *vl = visited_list_pool_->getFreeVisitedList();
        vl_type *visited_array = vl->mass;
        vl_type visited_array_tag = vl->curV;

        std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst> top_candidates;
        std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst> candidateSet;

        dist_t lowerBound;
        if (!isMarkedDeleted(ep_id)) {
            dist_t dist = fstdistfunc_(data_point, getDataByInternalId(ep_id), dist_func_param_);
            top_candidates.emplace(dist, ep_id);
            lowerBound = dist;
            candidateSet.emplace(-dist, ep_id);
        } else {
            lowerBound = std::numeric_limits<dist_t>::max();
            candidateSet.emplace(-lowerBound, ep_id);
        }
        visited_array[ep_id] = visited_array_tag;

        while (!candidateSet.empty()) {
            std::pair<dist_t, tableint> curr_el_pair = candidateSet.top();
            if ((-curr_el_pair.first) > lowerBound && top_candidates.size() == ef_construction_) {
                break;
            }
            candidateSet.pop();

            tableint curNodeNum = curr_el_pair.second;

            std::unique_lock <std::mutex> lock(link_list_locks_[curNodeNum]);

            int *data;  // = (int *)(linkList0_ + curNodeNum * size_links_per_element0_);
            if (layer == 0) {
                data = (int*)get_linklist0(curNodeNum);
            } else {
                data = (int*)get_linklist(curNodeNum, layer);
//                    data = (int *) (linkLists_[curNodeNum] + (layer - 1) * size_links_per_element_);
            }
            size_t size = getListCount((linklistsizeint*)data);
            tableint *datal = (tableint *) (data + 1);
#ifdef USE_SSE
            _mm_prefetch((char *) (visited_array + *(data + 1)), _MM_HINT_T0);
            _mm_prefetch((char *) (visited_array + *(data + 1) + 64), _MM_HINT_T0);
            _mm_prefetch(getDataByInternalId(*datal), _MM_HINT_T0);
            _mm_prefetch(getDataByInternalId(*(datal + 1)), _MM_HINT_T0);
#endif

            for (size_t j = 0; j < size; j++) {
                tableint candidate_id = *(datal + j);
//                    if (candidate_id == 0) continue;
#ifdef USE_SSE
                _mm_prefetch((char *) (visited_array + *(datal + j + 1)), _MM_HINT_T0);
                _mm_prefetch(getDataByInternalId(*(datal + j + 1)), _MM_HINT_T0);
#endif
                if (visited_array[candidate_id] == visited_array_tag) continue;
                visited_array[candidate_id] = visited_array_tag;
                char *currObj1 = (getDataByInternalId(candidate_id));

                dist_t dist1 = fstdistfunc_(data_point, currObj1, dist_func_param_);
                if (top_candidates.size() < ef_construction_ || lowerBound > dist1) {
                    candidateSet.emplace(-dist1, candidate_id);
#ifdef USE_SSE
                    _mm_prefetch(getDataByInternalId(candidateSet.top().second), _MM_HINT_T0);
#endif

                    if (!isMarkedDeleted(candidate_id))
                        top_candidates.emplace(dist1, candidate_id);

                    if (top_candidates.size() > ef_construction_)
                        top_candidates.pop();

                    if (!top_candidates.empty())
                        lowerBound = top_candidates.top().first;
                }
            }
        }
        visited_list_pool_->releaseVisitedList(vl);

        return top_candidates;
    }


    // bare_bone_search means there is no check for deletions and stop condition is ignored in return of extra performance
    template <bool bare_bone_search = true, bool collect_metrics = false>
    std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst>
    searchBaseLayerST(
        tableint ep_id,
        const void *data_point,
        size_t ef,
        BaseFilterFunctor* isIdAllowed = nullptr,
        BaseSearchStopCondition<dist_t>* stop_condition = nullptr) const {
        VisitedList *vl = visited_list_pool_->getFreeVisitedList();
        vl_type *visited_array = vl->mass;
        vl_type visited_array_tag = vl->curV;

        std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst> top_candidates;
        std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst> candidate_set;
        
        // std::unordered_map<page_id_t, PageHandler> cur_level_node_pages;
        // std::unordered_map<page_id_t, ReadAheadPageHandler> cur_level_node_readahead_pages;
        // // assign to ⬆ when searching into next layer
        // std::unordered_map<page_id_t, PageHandler> cur_level_neighbor_pages;
        // std::unordered_map<page_id_t, ReadAheadPageHandler> cur_level_neighbor_readahead_pages;
        // // assign to ⬆ when searching into next layer
        // std::unordered_map<page_id_t, ReadAheadPageHandler> cand_node_neighbor_readahead_pages;  // dist(cand_neighbor, cur) == 2, clear when searching next layer

        // auto get_page = [this](
        //     std::unordered_map<page_id_t, PageHandler>& pages, 
        //     std::unordered_map<page_id_t, ReadAheadPageHandler>& readahead_pages,
        //     page_id_t page_id) -> PageHandler& {
        //     if (pages.contains(page_id)) {
        //         return pages.at(page_id);
        //     }
        //     else if (readahead_pages.contains(page_id)) {
        //         auto readahead_handler = std::move(readahead_pages.at(page_id));
        //         readahead_pages.erase(page_id);
        //         auto handler = std::move(readahead_handler).wait_ready();
        //         pages.emplace(page_id, std::move(handler));
        //         return pages.at(page_id);
        //     }
        //     else {
        //         auto handler = page_cache->get_page(page_id);
        //         pages.emplace(page_id, std::move(handler));
        //         return pages.at(page_id);
        //     }
        // };

        // auto readahead_page = [this](
        //     std::unordered_map<page_id_t, ReadAheadPageHandler>& readahead_pages,
        //     page_id_t page_id) {
        //     if (readahead_pages.contains(page_id)) {
        //         return;
        //     }
        //     auto handler = page_cache->readahead_page(page_id);
        //     if (handler) {
        //         readahead_pages.emplace(page_id, std::move(handler.value()));
        //     }
        // };

        dist_t lowerBound;
        {
            auto [ep_page_id, ep_offset] = get_level0_offset(ep_id);
            auto ep_page_handler = page_cache->get_page(ep_page_id);
            PointPageLevel0 ep_page(ep_page_handler, ep_offset, this); 
            if (bare_bone_search || 
                // (!isMarkedDeleted(ep_id) && ((!isIdAllowed) || (*isIdAllowed)(getExternalLabel(ep_id))))) {
                (!isMarkedDeleted(ep_id) && ((!isIdAllowed) || (*isIdAllowed)(ep_page.get_label())))) {
                // char* ep_data = getDataByInternalId(ep_id);
                char* ep_data = ep_page.get_data();
                dist_t dist = fstdistfunc_(data_point, ep_data, dist_func_param_);
                lowerBound = dist;
                top_candidates.emplace(dist, ep_id);
                if (!bare_bone_search && stop_condition) {
                    // stop_condition->add_point_to_result(getExternalLabel(ep_id), ep_data, dist);
                    stop_condition->add_point_to_result(ep_page.get_label(), ep_data, dist);
                }
                candidate_set.emplace(-dist, ep_id);

                // it will be assigned to cur_level_node_pages when searching begins
                // cur_level_neighbor_pages.emplace(ep_page_id, std::move(ep_page_handler));
            } else {
                lowerBound = std::numeric_limits<dist_t>::max();
                candidate_set.emplace(-lowerBound, ep_id);
            }
        }

        visited_array[ep_id] = visited_array_tag;

        bool flag_stop_search = false;
        while (!candidate_set.empty() && !flag_stop_search) {
            // layer based search for using page cache
            // Update Layer pages and readahead pages data structure
            // std::swap(cur_level_node_pages, cur_level_neighbor_pages);  // last level neighbor pages become current level node pages
            // std::swap(cur_level_node_readahead_pages, cur_level_neighbor_readahead_pages); // last level neighbor readahead pages become current level node readahead pages
            // std::swap(cur_level_neighbor_readahead_pages, cand_node_neighbor_readahead_pages);  // last level candidate neighbor readahead pages become current level neighbor readahead pages
            // cur_level_neighbor_pages.clear();  // clear last level node pages
            // cand_node_neighbor_readahead_pages.clear();  // clear last level neighbor readahead pages

            auto layer_count = candidate_set.size();
            while (layer_count--) {
                std::pair<dist_t, tableint> current_node_pair = candidate_set.top();
                dist_t candidate_dist = -current_node_pair.first;

                if (bare_bone_search) {
                    flag_stop_search = candidate_dist > lowerBound;
                } else {
                    if (stop_condition) {
                        flag_stop_search = stop_condition->should_stop_search(candidate_dist, lowerBound);
                    } else {
                        flag_stop_search = candidate_dist > lowerBound && top_candidates.size() == ef;
                    }
                }
                if (flag_stop_search) {
                    break;
                }
                candidate_set.pop();

                tableint current_node_id = current_node_pair.second;
                auto [cur_page_id, cur_off] = get_level0_offset(current_node_id);
                auto cur_node_page_handler = page_cache->get_page(cur_page_id);
                PointPageLevel0 current_node_page(cur_node_page_handler, cur_off, this);

                // int *data = (int *) get_linklist0(current_node_id);
                int* data = (int *) current_node_page.get_neighbor_list();
                // size_t size = getListCount((linklistsizeint*)data);
                size_t size = current_node_page.get_neighbor_count();
    //                bool cur_node_deleted = isMarkedDeleted(current_node_id);
                if (collect_metrics) {
                    metric_hops++;
                    metric_distance_computations+=size;
                }

    // TODO: Replace SSE to page cache readahead
    #ifdef USE_SSE
                // _mm_prefetch((char *) (visited_array + *(data + 1)), _MM_HINT_T0);
                _mm_prefetch((char *) (visited_array + *data), _MM_HINT_T0);
                // _mm_prefetch((char *) (visited_array + *(data + 1) + 64), _MM_HINT_T0);
                _mm_prefetch((char *) (visited_array + *data + 64), _MM_HINT_T0);
                // _mm_prefetch(data_level0_memory_ + (*(data + 1)) * size_data_per_element_ + offsetData_, _MM_HINT_T0);
                // _mm_prefetch((char *) (data + 2), _MM_HINT_T0);
                _mm_prefetch((char *) (data + 1), _MM_HINT_T0);
    #endif

                // for (size_t j = 1; j <= size; j++) {
                for (size_t j = 0; j < size; j++) {
                    int candidate_id = *(data + j);
    //                    if (candidate_id == 0) continue;
    #ifdef USE_SSE
                    _mm_prefetch((char *) (visited_array + *(data + j + 1)), _MM_HINT_T0);
                    // _mm_prefetch(data_level0_memory_ + (*(data + j + 1)) * size_data_per_element_ + offsetData_,
                    //                 _MM_HINT_T0);  ////////////
    #endif
                    
                    if (visited_array[candidate_id] == visited_array_tag)
                        continue;

                    visited_array[candidate_id] = visited_array_tag;

                    auto [cand_page_id, cand_off] = get_level0_offset(candidate_id);

                    // readahead next cand page
                    // if (j + 1 < size) {
                    //     int next_cand_id = *(data + j + 1);
                    //     auto [next_cand_page_id, next_cand_off] = get_level0_offset(next_cand_id);
                    //     if (cand_page_id != next_cand_page_id) {
                    //         page_cache->readahead_page_async(next_cand_page_id);
                    //     }
                    // }

                    auto cand_page_handler = page_cache->get_page(cand_page_id);
                    PointPageLevel0 cand_page(cand_page_handler, cand_off, this);

                    // char *currObj1 = (getDataByInternalId(candidate_id));
                    char *currObj1 = cand_page.get_data();
                    dist_t dist = fstdistfunc_(data_point, currObj1, dist_func_param_);

                    bool flag_consider_candidate;
                    if (!bare_bone_search && stop_condition) {
                        flag_consider_candidate = stop_condition->should_consider_candidate(dist, lowerBound);
                    } else {
                        flag_consider_candidate = top_candidates.size() < ef || lowerBound > dist;
                    }

                    if (!flag_consider_candidate)
                        continue;
                    
                    candidate_set.emplace(-dist, candidate_id);
// #ifdef USE_SSE
//                         _mm_prefetch(data_level0_memory_ + candidate_set.top().second * size_data_per_element_ +
//                                         offsetLevel0_,  ///////////
//                                         _MM_HINT_T0);  ////////////////////////
// #endif

                    if (bare_bone_search || 
                        // (!isMarkedDeleted(candidate_id) && ((!isIdAllowed) || (*isIdAllowed)(getExternalLabel(candidate_id))))) {
                        (!isMarkedDeleted(candidate_id) && ((!isIdAllowed) || (*isIdAllowed)(cand_page.get_label())))) {
                        top_candidates.emplace(dist, candidate_id);
                        if (!bare_bone_search && stop_condition) {
                            // stop_condition->add_point_to_result(getExternalLabel(candidate_id), currObj1, dist);
                            stop_condition->add_point_to_result(cand_page.get_label(), currObj1, dist);
                        }
                    }

                    bool flag_remove_extra = false;
                    if (!bare_bone_search && stop_condition) {
                        flag_remove_extra = stop_condition->should_remove_extra();
                    } else {
                        flag_remove_extra = top_candidates.size() > ef;
                    }
                    while (flag_remove_extra) {
                        tableint id = top_candidates.top().second;
                        top_candidates.pop();
                        if (!bare_bone_search && stop_condition) {
                            auto [page_id, off] = get_level0_offset(id);
                            auto page_handler = page_cache->get_page(page_id);
                            PointPageLevel0 page(page_handler, off, this);
                            // stop_condition->remove_point_from_result(getExternalLabel(id), getDataByInternalId(id), dist);
                            stop_condition->remove_point_from_result(page.get_label(), page.get_data(), dist);
                            flag_remove_extra = stop_condition->should_remove_extra();
                        } else {
                            flag_remove_extra = top_candidates.size() > ef;
                        }
                    }

                    if (!top_candidates.empty())
                        lowerBound = top_candidates.top().first;
                }
            }
        }

        visited_list_pool_->releaseVisitedList(vl);
        return top_candidates;
    }


    void getNeighborsByHeuristic2(
        std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst> &top_candidates,
        const size_t M) {
        if (top_candidates.size() < M) {
            return;
        }

        std::priority_queue<std::pair<dist_t, tableint>> queue_closest;
        std::vector<std::pair<dist_t, tableint>> return_list;
        while (top_candidates.size() > 0) {
            queue_closest.emplace(-top_candidates.top().first, top_candidates.top().second);
            top_candidates.pop();
        }

        while (queue_closest.size()) {
            if (return_list.size() >= M)
                break;
            std::pair<dist_t, tableint> curent_pair = queue_closest.top();
            dist_t dist_to_query = -curent_pair.first;
            queue_closest.pop();
            bool good = true;

            for (std::pair<dist_t, tableint> second_pair : return_list) {
                dist_t curdist =
                        fstdistfunc_(getDataByInternalId(second_pair.second),
                                        getDataByInternalId(curent_pair.second),
                                        dist_func_param_);
                if (curdist < dist_to_query) {
                    good = false;
                    break;
                }
            }
            if (good) {
                return_list.push_back(curent_pair);
            }
        }

        for (std::pair<dist_t, tableint> curent_pair : return_list) {
            top_candidates.emplace(-curent_pair.first, curent_pair.second);
        }
    }


    linklistsizeint *get_linklist0(tableint internal_id) const {
        return (linklistsizeint *) (data_level0_memory_ + internal_id * size_data_per_element_ + offsetLevel0_);
    }


    linklistsizeint *get_linklist0(tableint internal_id, char *data_level0_memory_) const {
        return (linklistsizeint *) (data_level0_memory_ + internal_id * size_data_per_element_ + offsetLevel0_);
    }


    linklistsizeint *get_linklist(tableint internal_id, int level) const {
        return (linklistsizeint *) (linkLists_[internal_id] + (level - 1) * size_links_per_element_);
    }


    linklistsizeint *get_linklist_at_level(tableint internal_id, int level) const {
        return level == 0 ? get_linklist0(internal_id) : get_linklist(internal_id, level);
    }


    tableint mutuallyConnectNewElement(
        const void *data_point,
        tableint cur_c,
        std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst> &top_candidates,
        int level,
        bool isUpdate) {
        size_t Mcurmax = level ? maxM_ : maxM0_;
        getNeighborsByHeuristic2(top_candidates, M_);
        if (top_candidates.size() > M_)
            throw std::runtime_error("Should be not be more than M_ candidates returned by the heuristic");

        std::vector<tableint> selectedNeighbors;
        selectedNeighbors.reserve(M_);
        while (top_candidates.size() > 0) {
            selectedNeighbors.push_back(top_candidates.top().second);
            top_candidates.pop();
        }

        tableint next_closest_entry_point = selectedNeighbors.back();

        {
            // lock only during the update
            // because during the addition the lock for cur_c is already acquired
            std::unique_lock <std::mutex> lock(link_list_locks_[cur_c], std::defer_lock);
            if (isUpdate) {
                lock.lock();
            }
            linklistsizeint *ll_cur;
            if (level == 0)
                ll_cur = get_linklist0(cur_c);
            else
                ll_cur = get_linklist(cur_c, level);

            if (*ll_cur && !isUpdate) {
                throw std::runtime_error("The newly inserted element should have blank link list");
            }
            setListCount(ll_cur, selectedNeighbors.size());
            tableint *data = (tableint *) (ll_cur + 1);
            for (size_t idx = 0; idx < selectedNeighbors.size(); idx++) {
                if (data[idx] && !isUpdate)
                    throw std::runtime_error("Possible memory corruption");
                if (level > element_levels_[selectedNeighbors[idx]])
                    throw std::runtime_error("Trying to make a link on a non-existent level");

                data[idx] = selectedNeighbors[idx];
            }
        }

        for (size_t idx = 0; idx < selectedNeighbors.size(); idx++) {
            std::unique_lock <std::mutex> lock(link_list_locks_[selectedNeighbors[idx]]);

            linklistsizeint *ll_other;
            if (level == 0)
                ll_other = get_linklist0(selectedNeighbors[idx]);
            else
                ll_other = get_linklist(selectedNeighbors[idx], level);

            size_t sz_link_list_other = getListCount(ll_other);

            if (sz_link_list_other > Mcurmax)
                throw std::runtime_error("Bad value of sz_link_list_other");
            if (selectedNeighbors[idx] == cur_c)
                throw std::runtime_error("Trying to connect an element to itself");
            if (level > element_levels_[selectedNeighbors[idx]])
                throw std::runtime_error("Trying to make a link on a non-existent level");

            tableint *data = (tableint *) (ll_other + 1);

            bool is_cur_c_present = false;
            if (isUpdate) {
                for (size_t j = 0; j < sz_link_list_other; j++) {
                    if (data[j] == cur_c) {
                        is_cur_c_present = true;
                        break;
                    }
                }
            }

            // If cur_c is already present in the neighboring connections of `selectedNeighbors[idx]` then no need to modify any connections or run the heuristics.
            if (!is_cur_c_present) {
                if (sz_link_list_other < Mcurmax) {
                    data[sz_link_list_other] = cur_c;
                    setListCount(ll_other, sz_link_list_other + 1);
                } else {
                    // finding the "weakest" element to replace it with the new one
                    dist_t d_max = fstdistfunc_(getDataByInternalId(cur_c), getDataByInternalId(selectedNeighbors[idx]),
                                                dist_func_param_);
                    // Heuristic:
                    std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst> candidates;
                    candidates.emplace(d_max, cur_c);

                    for (size_t j = 0; j < sz_link_list_other; j++) {
                        candidates.emplace(
                                fstdistfunc_(getDataByInternalId(data[j]), getDataByInternalId(selectedNeighbors[idx]),
                                                dist_func_param_), data[j]);
                    }

                    getNeighborsByHeuristic2(candidates, Mcurmax);

                    int indx = 0;
                    while (candidates.size() > 0) {
                        data[indx] = candidates.top().second;
                        candidates.pop();
                        indx++;
                    }

                    setListCount(ll_other, indx);
                    // Nearest K:
                    /*int indx = -1;
                    for (int j = 0; j < sz_link_list_other; j++) {
                        dist_t d = fstdistfunc_(getDataByInternalId(data[j]), getDataByInternalId(rez[idx]), dist_func_param_);
                        if (d > d_max) {
                            indx = j;
                            d_max = d;
                        }
                    }
                    if (indx >= 0) {
                        data[indx] = cur_c;
                    } */
                }
            }
        }

        return next_closest_entry_point;
    }


    void resizeIndex(size_t new_max_elements) {
        if (new_max_elements < cur_element_count)
            throw std::runtime_error("Cannot resize, max element is less than the current number of elements");

        visited_list_pool_.reset(new VisitedListPool(1, new_max_elements));

        element_levels_.resize(new_max_elements);

        std::vector<std::mutex>(new_max_elements).swap(link_list_locks_);

        // Reallocate base layer
        char * data_level0_memory_new = (char *) realloc(data_level0_memory_, new_max_elements * size_data_per_element_);
        if (data_level0_memory_new == nullptr)
            throw std::runtime_error("Not enough memory: resizeIndex failed to allocate base layer");
        data_level0_memory_ = data_level0_memory_new;

        // Reallocate all other layers
        char ** linkLists_new = (char **) realloc(linkLists_, sizeof(void *) * new_max_elements);
        if (linkLists_new == nullptr)
            throw std::runtime_error("Not enough memory: resizeIndex failed to allocate other layers");
        linkLists_ = linkLists_new;

        max_elements_ = new_max_elements;
    }

    size_t indexFileSize() const {
        size_t size = 0;

        // Original metadata
        size += sizeof(offsetLevel0_);
        size += sizeof(max_elements_);
        size += sizeof(cur_element_count);
        size += sizeof(size_data_per_element_);
        size += sizeof(label_offset_);
        size += sizeof(offsetData_);
        size += sizeof(maxlevel_);
        size += sizeof(enterpoint_node_);
        size += sizeof(maxM_);
        size += sizeof(maxM0_);
        size += sizeof(M_);
        size += sizeof(mult_);
        size += sizeof(ef_construction_);

        // Page cache metadata
        size += sizeof(page_size_);
        size += sizeof(level0_first_page_id_);
        size += sizeof(level0_elements_per_page_);
        size += sizeof(link_offset_array_page_id_);

        // Calculate page-aligned size for level 0 data
        size_t page_size = page_size_ > 0 ? page_size_ : 4096;
        size_t level0_elements_per_page = page_size / size_data_per_element_;
        if (level0_elements_per_page == 0) {
            level0_elements_per_page = 1;
        }

        // Padding to page boundary before level 0 data
        size_t padding = page_size - (size % page_size);
        if (padding == page_size) padding = 0;
        size += padding;

        // Level 0 data
        size += cur_element_count * size_data_per_element_;

        // Higher level links (accounting for page alignment)
        for (size_t i = 0; i < cur_element_count; i++) {
            unsigned int linkListSize = element_levels_[i] > 0 ? size_links_per_element_ * element_levels_[i] : 0;
            size_t total_size = sizeof(linkListSize) + linkListSize;

            // Check if we need padding for page alignment
            size_t offset_in_page = size % page_size;
            if (offset_in_page + total_size > page_size) {
                size += page_size - offset_in_page; // Pad to page boundary
            }

            size += total_size;
        }

        // Link offset array
        size += cur_element_count * sizeof(size_t);

        return size;
    }

    void saveIndex(const std::string &location) {
        reorder_vertices_with_degree_ascending_bfs();

        std::ofstream output(location, std::ios::binary);

        // Write original metadata
        writeBinaryPOD(output, offsetLevel0_);
        writeBinaryPOD(output, max_elements_);
        writeBinaryPOD(output, cur_element_count);
        writeBinaryPOD(output, size_data_per_element_);
        writeBinaryPOD(output, label_offset_);
        writeBinaryPOD(output, offsetData_);
        writeBinaryPOD(output, maxlevel_);
        writeBinaryPOD(output, enterpoint_node_);
        writeBinaryPOD(output, maxM_);

        writeBinaryPOD(output, maxM0_);
        writeBinaryPOD(output, M_);
        writeBinaryPOD(output, mult_);
        writeBinaryPOD(output, ef_construction_);

        // Write page cache metadata (default values for backward compatibility)
        // If page_size_ is 0, this is being saved from an index created without page cache support
        // In this case, we use a default page size and write the old format
        size_t page_size = page_size_ > 0 ? page_size_ : 32768;  // default 32KB page
        writeBinaryPOD(output, page_size);

        // Calculate level 0 page-aligned layout
        page_id_t level0_first_page_id = 0;
        size_t level0_elements_per_page = page_size / size_data_per_element_;
        if (level0_elements_per_page == 0) {
            // Element is larger than page size, put one element per page
            level0_elements_per_page = 1;
        }
        size_t level0_first_page_id_pos = output.tellp();
        writeBinaryPOD(output, level0_first_page_id);
        writeBinaryPOD(output, level0_elements_per_page);

        // Reserve space for link offset array metadata
        size_t link_offset_array_page_id_pos = output.tellp();
        page_id_t link_offset_array_page_id = 0;
        writeBinaryPOD(output, link_offset_array_page_id);

        // Pad to page boundary for level 0 data using seek
        size_t level0_start_pos = output.tellp();
        size_t padding = page_size - (level0_start_pos % page_size);
        if (padding == page_size) padding = 0;
        level0_start_pos += padding;

        // Update level 0 first page id after padding
        level0_first_page_id = level0_start_pos / page_size;
        output.seekp(level0_first_page_id_pos);
        writeBinaryPOD(output, level0_first_page_id);
        output.seekp(level0_start_pos);

        // Write level 0 data with page alignment
        for (size_t i = 0; i < cur_element_count; i++) {
            // Check if element fits in current page
            size_t offset_in_page = output.tellp() % page_size;

            // If element doesn't fit in current page, pad to next page
            if (offset_in_page + size_data_per_element_ > page_size) {
                size_t pad_size = page_size - offset_in_page;
                output.seekp(pad_size, std::ios::cur);
            }

            // Write element data
            int origin_id = store_order_to_id[i];
            const char* element_data = data_level0_memory_ + origin_id * size_data_per_element_;
            output.write(element_data, size_data_per_element_);
        }

        // Track link offsets for each element
        std::vector<size_t> link_offsets;
        link_offsets.reserve(cur_element_count);

        // Write higher level links and record their offsets (with page alignment)
        for (size_t i = 0; i < cur_element_count; i++) {
            size_t current_pos = output.tellp();
            int origin_id = store_order_to_id[i];
            unsigned int linkListSize = element_levels_[origin_id] > 0 ? size_links_per_element_ * element_levels_[origin_id] : 0;

            // Check if the link list fits in current page
            size_t offset_in_page = current_pos % page_size;
            if (offset_in_page + linkListSize > page_size) {
                // Pad to page boundary using seek
                size_t pad_size = page_size - offset_in_page;
                output.seekp(pad_size, std::ios::cur);
                current_pos = output.tellp();
            }
            link_offsets.push_back(current_pos);

            if (linkListSize)
                output.write(linkLists_[origin_id], linkListSize);
        }

        // Pad to page boundary for link offset array
        size_t link_offset_array_offset = output.tellp();
        size_t link_array_padding = page_size - (link_offset_array_offset % page_size);
        if (link_array_padding == page_size) link_array_padding = 0;
        if (link_array_padding > 0) {
            output.seekp(link_array_padding, std::ios::cur);
            link_offset_array_offset += link_array_padding;
        }

        // Calculate page id for link offset array
        link_offset_array_page_id = link_offset_array_offset / page_size;

        // Write link offset array
        for (size_t i = 0; i < cur_element_count; i++) {
            writeBinaryPOD(output, link_offsets[i]);
        }

        // Update link offset array metadata in header
        output.seekp(link_offset_array_page_id_pos);
        writeBinaryPOD(output, link_offset_array_page_id);

        output.close();
    }


    void loadIndex(const std::string &location, SpaceInterface<dist_t> *s, size_t max_elements_i = 0, size_t cache_size_i = 0) {
        std::ifstream input(location, std::ios::binary);

        if (!input.is_open())
            throw std::runtime_error("Cannot open file");

        clear();

        // Read original metadata
        readBinaryPOD(input, offsetLevel0_);
        readBinaryPOD(input, max_elements_);
        readBinaryPOD(input, cur_element_count);

        size_t max_elements = max_elements_i;
        if (max_elements < cur_element_count)
            max_elements = max_elements_;
        max_elements_ = max_elements;
        readBinaryPOD(input, size_data_per_element_);
        readBinaryPOD(input, label_offset_);
        readBinaryPOD(input, offsetData_);
        readBinaryPOD(input, maxlevel_);
        readBinaryPOD(input, enterpoint_node_);

        readBinaryPOD(input, maxM_);
        readBinaryPOD(input, maxM0_);
        readBinaryPOD(input, M_);
        readBinaryPOD(input, mult_);
        readBinaryPOD(input, ef_construction_);

        // Read page cache metadata
        readBinaryPOD(input, page_size_);
        readBinaryPOD(input, level0_first_page_id_);
        readBinaryPOD(input, level0_elements_per_page_);
        readBinaryPOD(input, link_offset_array_page_id_);

        data_size_ = s->get_data_size();
        fstdistfunc_ = s->get_dist_func();
        dist_func_param_ = s->get_dist_func_param();

        // Initialize page cache for on-demand loading
        size_t cache_size = cache_size_i;
        if (cache_size == 0) {
            // Default cache size: 256MB or max_elements * page_size, whichever is smaller
            cache_size = std::min(max_elements_ * page_size_, static_cast<size_t>(256 * 1024 * 1024));
        }
        page_cache = std::unique_ptr<HnswPageCache>(new HnswPageCache(location, page_size_, cache_size));

        size_links_per_element_ = maxM_ * sizeof(tableint) + sizeof(linklistsizeint);
        size_links_level0_ = maxM0_ * sizeof(tableint) + sizeof(linklistsizeint);

        std::vector<std::mutex>(max_elements).swap(link_list_locks_);
        std::vector<std::mutex>(MAX_LABEL_OPERATION_LOCKS).swap(label_op_locks_);

        visited_list_pool_.reset(new VisitedListPool(1, max_elements));

        // Don't load element data, link lists, or level 0 data
        // These will be loaded on-demand during search operations

        revSize_ = 1.0 / mult_;
        ef_ = 10;

        // Note: In on-demand loading mode, we don't pre-load any element data
        // The actual element data (vectors, labels, links) will be loaded via page cache
        // when needed during search operations

        input.close();

        return;
    }


    template<typename data_t>
    std::vector<data_t> getDataByLabel(labeltype label) const {
        // lock all operations with element by label
        std::unique_lock <std::mutex> lock_label(getLabelOpMutex(label));
        
        std::unique_lock <std::mutex> lock_table(label_lookup_lock);
        auto search = label_lookup_.find(label);
        if (search == label_lookup_.end() || isMarkedDeleted(search->second)) {
            throw std::runtime_error("Label not found");
        }
        tableint internalId = search->second;
        lock_table.unlock();

        char* data_ptrv = getDataByInternalId(internalId);
        size_t dim = *((size_t *) dist_func_param_);
        std::vector<data_t> data;
        data_t* data_ptr = (data_t*) data_ptrv;
        for (size_t i = 0; i < dim; i++) {
            data.push_back(*data_ptr);
            data_ptr += 1;
        }
        return data;
    }


    /*
    * Marks an element with the given label deleted, does NOT really change the current graph.
    */
    void markDelete(labeltype label) {
        // lock all operations with element by label
        std::unique_lock <std::mutex> lock_label(getLabelOpMutex(label));

        std::unique_lock <std::mutex> lock_table(label_lookup_lock);
        auto search = label_lookup_.find(label);
        if (search == label_lookup_.end()) {
            throw std::runtime_error("Label not found");
        }
        tableint internalId = search->second;
        lock_table.unlock();

        markDeletedInternal(internalId);
    }


    /*
    * Uses the last 16 bits of the memory for the linked list size to store the mark,
    * whereas maxM0_ has to be limited to the lower 16 bits, however, still large enough in almost all cases.
    */
    void markDeletedInternal(tableint internalId) {
        assert(internalId < cur_element_count);
        if (!isMarkedDeleted(internalId)) {
            unsigned char *ll_cur = ((unsigned char *)get_linklist0(internalId))+2;
            *ll_cur |= DELETE_MARK;
            num_deleted_ += 1;
            if (allow_replace_deleted_) {
                std::unique_lock <std::mutex> lock_deleted_elements(deleted_elements_lock);
                deleted_elements.insert(internalId);
            }
        } else {
            throw std::runtime_error("The requested to delete element is already deleted");
        }
    }


    /*
    * Removes the deleted mark of the node, does NOT really change the current graph.
    * 
    * Note: the method is not safe to use when replacement of deleted elements is enabled,
    *  because elements marked as deleted can be completely removed by addPoint
    */
    void unmarkDelete(labeltype label) {
        // lock all operations with element by label
        std::unique_lock <std::mutex> lock_label(getLabelOpMutex(label));

        std::unique_lock <std::mutex> lock_table(label_lookup_lock);
        auto search = label_lookup_.find(label);
        if (search == label_lookup_.end()) {
            throw std::runtime_error("Label not found");
        }
        tableint internalId = search->second;
        lock_table.unlock();

        unmarkDeletedInternal(internalId);
    }



    /*
    * Remove the deleted mark of the node.
    */
    void unmarkDeletedInternal(tableint internalId) {
        assert(internalId < cur_element_count);
        if (isMarkedDeleted(internalId)) {
            unsigned char *ll_cur = ((unsigned char *)get_linklist0(internalId)) + 2;
            *ll_cur &= ~DELETE_MARK;
            num_deleted_ -= 1;
            if (allow_replace_deleted_) {
                std::unique_lock <std::mutex> lock_deleted_elements(deleted_elements_lock);
                deleted_elements.erase(internalId);
            }
        } else {
            throw std::runtime_error("The requested to undelete element is not deleted");
        }
    }


    /*
    * Checks the first 16 bits of the memory to see if the element is marked deleted.
    */
    bool isMarkedDeleted(tableint internalId) const {
        unsigned char *ll_cur = ((unsigned char*)get_linklist0(internalId)) + 2;
        return *ll_cur & DELETE_MARK;
    }


    unsigned short int getListCount(linklistsizeint * ptr) const {
        return *((unsigned short int *)ptr);
    }


    void setListCount(linklistsizeint * ptr, unsigned short int size) const {
        *((unsigned short int*)(ptr))=*((unsigned short int *)&size);
    }


    /*
    * Adds point. Updates the point if it is already in the index.
    * If replacement of deleted elements is enabled: replaces previously deleted point if any, updating it with new point
    */
    void addPoint(const void *data_point, labeltype label, bool replace_deleted = false) {
        if ((allow_replace_deleted_ == false) && (replace_deleted == true)) {
            throw std::runtime_error("Replacement of deleted elements is disabled in constructor");
        }

        // lock all operations with element by label
        std::unique_lock <std::mutex> lock_label(getLabelOpMutex(label));
        if (!replace_deleted) {
            addPoint(data_point, label, -1);
            return;
        }
        // check if there is vacant place
        tableint internal_id_replaced;
        std::unique_lock <std::mutex> lock_deleted_elements(deleted_elements_lock);
        bool is_vacant_place = !deleted_elements.empty();
        if (is_vacant_place) {
            internal_id_replaced = *deleted_elements.begin();
            deleted_elements.erase(internal_id_replaced);
        }
        lock_deleted_elements.unlock();

        // if there is no vacant place then add or update point
        // else add point to vacant place
        if (!is_vacant_place) {
            addPoint(data_point, label, -1);
        } else {
            // we assume that there are no concurrent operations on deleted element
            labeltype label_replaced = getExternalLabel(internal_id_replaced);
            setExternalLabel(internal_id_replaced, label);

            std::unique_lock <std::mutex> lock_table(label_lookup_lock);
            label_lookup_.erase(label_replaced);
            label_lookup_[label] = internal_id_replaced;
            lock_table.unlock();

            unmarkDeletedInternal(internal_id_replaced);
            updatePoint(data_point, internal_id_replaced, 1.0);
        }
    }


    void updatePoint(const void *dataPoint, tableint internalId, float updateNeighborProbability) {
        // update the feature vector associated with existing point with new vector
        memcpy(getDataByInternalId(internalId), dataPoint, data_size_);

        int maxLevelCopy = maxlevel_;
        tableint entryPointCopy = enterpoint_node_;
        // If point to be updated is entry point and graph just contains single element then just return.
        if (entryPointCopy == internalId && cur_element_count == 1)
            return;

        int elemLevel = element_levels_[internalId];
        std::uniform_real_distribution<float> distribution(0.0, 1.0);
        for (int layer = 0; layer <= elemLevel; layer++) {
            std::unordered_set<tableint> sCand;
            std::unordered_set<tableint> sNeigh;
            std::vector<tableint> listOneHop = getConnectionsWithLock(internalId, layer);
            if (listOneHop.size() == 0)
                continue;

            sCand.insert(internalId);

            for (auto&& elOneHop : listOneHop) {
                sCand.insert(elOneHop);

                if (distribution(update_probability_generator_) > updateNeighborProbability)
                    continue;

                sNeigh.insert(elOneHop);

                std::vector<tableint> listTwoHop = getConnectionsWithLock(elOneHop, layer);
                for (auto&& elTwoHop : listTwoHop) {
                    sCand.insert(elTwoHop);
                }
            }

            for (auto&& neigh : sNeigh) {
                // if (neigh == internalId)
                //     continue;

                std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst> candidates;
                size_t size = sCand.find(neigh) == sCand.end() ? sCand.size() : sCand.size() - 1;  // sCand guaranteed to have size >= 1
                size_t elementsToKeep = std::min(ef_construction_, size);
                for (auto&& cand : sCand) {
                    if (cand == neigh)
                        continue;

                    dist_t distance = fstdistfunc_(getDataByInternalId(neigh), getDataByInternalId(cand), dist_func_param_);
                    if (candidates.size() < elementsToKeep) {
                        candidates.emplace(distance, cand);
                    } else {
                        if (distance < candidates.top().first) {
                            candidates.pop();
                            candidates.emplace(distance, cand);
                        }
                    }
                }

                // Retrieve neighbours using heuristic and set connections.
                getNeighborsByHeuristic2(candidates, layer == 0 ? maxM0_ : maxM_);

                {
                    std::unique_lock <std::mutex> lock(link_list_locks_[neigh]);
                    linklistsizeint *ll_cur;
                    ll_cur = get_linklist_at_level(neigh, layer);
                    size_t candSize = candidates.size();
                    setListCount(ll_cur, candSize);
                    tableint *data = (tableint *) (ll_cur + 1);
                    for (size_t idx = 0; idx < candSize; idx++) {
                        data[idx] = candidates.top().second;
                        candidates.pop();
                    }
                }
            }
        }

        repairConnectionsForUpdate(dataPoint, entryPointCopy, internalId, elemLevel, maxLevelCopy);
    }


    void repairConnectionsForUpdate(
        const void *dataPoint,
        tableint entryPointInternalId,
        tableint dataPointInternalId,
        int dataPointLevel,
        int maxLevel) {
        tableint currObj = entryPointInternalId;
        if (dataPointLevel < maxLevel) {
            dist_t curdist = fstdistfunc_(dataPoint, getDataByInternalId(currObj), dist_func_param_);
            for (int level = maxLevel; level > dataPointLevel; level--) {
                bool changed = true;
                while (changed) {
                    changed = false;
                    unsigned int *data;
                    std::unique_lock <std::mutex> lock(link_list_locks_[currObj]);
                    data = get_linklist_at_level(currObj, level);
                    int size = getListCount(data);
                    tableint *datal = (tableint *) (data + 1);
#ifdef USE_SSE
                    _mm_prefetch(getDataByInternalId(*datal), _MM_HINT_T0);
#endif
                    for (int i = 0; i < size; i++) {
#ifdef USE_SSE
                        _mm_prefetch(getDataByInternalId(*(datal + i + 1)), _MM_HINT_T0);
#endif
                        tableint cand = datal[i];
                        dist_t d = fstdistfunc_(dataPoint, getDataByInternalId(cand), dist_func_param_);
                        if (d < curdist) {
                            curdist = d;
                            currObj = cand;
                            changed = true;
                        }
                    }
                }
            }
        }

        if (dataPointLevel > maxLevel)
            throw std::runtime_error("Level of item to be updated cannot be bigger than max level");

        for (int level = dataPointLevel; level >= 0; level--) {
            std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst> topCandidates = searchBaseLayer(
                    currObj, dataPoint, level);

            std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst> filteredTopCandidates;
            while (topCandidates.size() > 0) {
                if (topCandidates.top().second != dataPointInternalId)
                    filteredTopCandidates.push(topCandidates.top());

                topCandidates.pop();
            }

            // Since element_levels_ is being used to get `dataPointLevel`, there could be cases where `topCandidates` could just contains entry point itself.
            // To prevent self loops, the `topCandidates` is filtered and thus can be empty.
            if (filteredTopCandidates.size() > 0) {
                bool epDeleted = isMarkedDeleted(entryPointInternalId);
                if (epDeleted) {
                    filteredTopCandidates.emplace(fstdistfunc_(dataPoint, getDataByInternalId(entryPointInternalId), dist_func_param_), entryPointInternalId);
                    if (filteredTopCandidates.size() > ef_construction_)
                        filteredTopCandidates.pop();
                }

                currObj = mutuallyConnectNewElement(dataPoint, dataPointInternalId, filteredTopCandidates, level, true);
            }
        }
    }


    std::vector<tableint> getConnectionsWithLock(tableint internalId, int level) {
        std::unique_lock <std::mutex> lock(link_list_locks_[internalId]);
        unsigned int *data = get_linklist_at_level(internalId, level);
        int size = getListCount(data);
        std::vector<tableint> result(size);
        tableint *ll = (tableint *) (data + 1);
        memcpy(result.data(), ll, size * sizeof(tableint));
        return result;
    }


    tableint addPoint(const void *data_point, labeltype label, int level) {
        tableint cur_c = 0;
        {
            // Checking if the element with the same label already exists
            // if so, updating it *instead* of creating a new element.
            std::unique_lock <std::mutex> lock_table(label_lookup_lock);
            auto search = label_lookup_.find(label);
            if (search != label_lookup_.end()) {
                tableint existingInternalId = search->second;
                if (allow_replace_deleted_) {
                    if (isMarkedDeleted(existingInternalId)) {
                        throw std::runtime_error("Can't use addPoint to update deleted elements if replacement of deleted elements is enabled.");
                    }
                }
                lock_table.unlock();

                if (isMarkedDeleted(existingInternalId)) {
                    unmarkDeletedInternal(existingInternalId);
                }
                updatePoint(data_point, existingInternalId, 1.0);

                return existingInternalId;
            }

            if (cur_element_count >= max_elements_) {
                throw std::runtime_error("The number of elements exceeds the specified limit");
            }

            cur_c = cur_element_count;
            cur_element_count++;
            label_lookup_[label] = cur_c;
        }

        std::unique_lock <std::mutex> lock_el(link_list_locks_[cur_c]);
        int curlevel = getRandomLevel(mult_);
        if (level > 0)
            curlevel = level;

        element_levels_[cur_c] = curlevel;

        std::unique_lock <std::mutex> templock(global);
        int maxlevelcopy = maxlevel_;
        if (curlevel <= maxlevelcopy)
            templock.unlock();
        tableint currObj = enterpoint_node_;
        tableint enterpoint_copy = enterpoint_node_;

        memset(data_level0_memory_ + cur_c * size_data_per_element_ + offsetLevel0_, 0, size_data_per_element_);

        // Initialisation of the data and label
        memcpy(getExternalLabeLp(cur_c), &label, sizeof(labeltype));
        memcpy(getDataByInternalId(cur_c), data_point, data_size_);

        if (curlevel) {
            linkLists_[cur_c] = (char *) malloc(size_links_per_element_ * curlevel + 1);
            if (linkLists_[cur_c] == nullptr)
                throw std::runtime_error("Not enough memory: addPoint failed to allocate linklist");
            memset(linkLists_[cur_c], 0, size_links_per_element_ * curlevel + 1);
        }

        if ((signed)currObj != -1) {
            if (curlevel < maxlevelcopy) {
                dist_t curdist = fstdistfunc_(data_point, getDataByInternalId(currObj), dist_func_param_);
                for (int level = maxlevelcopy; level > curlevel; level--) {
                    bool changed = true;
                    while (changed) {
                        changed = false;
                        unsigned int *data;
                        std::unique_lock <std::mutex> lock(link_list_locks_[currObj]);
                        data = get_linklist(currObj, level);
                        int size = getListCount(data);

                        tableint *datal = (tableint *) (data + 1);
                        for (int i = 0; i < size; i++) {
                            tableint cand = datal[i];
                            if (cand < 0 || cand > max_elements_)
                                throw std::runtime_error("cand error");
                            dist_t d = fstdistfunc_(data_point, getDataByInternalId(cand), dist_func_param_);
                            if (d < curdist) {
                                curdist = d;
                                currObj = cand;
                                changed = true;
                            }
                        }
                    }
                }
            }

            bool epDeleted = isMarkedDeleted(enterpoint_copy);
            for (int level = std::min(curlevel, maxlevelcopy); level >= 0; level--) {
                if (level > maxlevelcopy || level < 0)  // possible?
                    throw std::runtime_error("Level error");

                std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst> top_candidates = searchBaseLayer(
                        currObj, data_point, level);
                if (epDeleted) {
                    top_candidates.emplace(fstdistfunc_(data_point, getDataByInternalId(enterpoint_copy), dist_func_param_), enterpoint_copy);
                    if (top_candidates.size() > ef_construction_)
                        top_candidates.pop();
                }
                currObj = mutuallyConnectNewElement(data_point, cur_c, top_candidates, level, false);
            }
        } else {
            // Do nothing for the first element
            enterpoint_node_ = 0;
            maxlevel_ = curlevel;
        }

        // Releasing lock for the maximum level
        if (curlevel > maxlevelcopy) {
            enterpoint_node_ = cur_c;
            maxlevel_ = curlevel;
        }
        return cur_c;
    }

    void reorder_vertices_with_degree_ascending_bfs() {
        int min_degree = INT_MAX;
        int cur_id;
        for (int i = 0; i < cur_element_count; ++i) {
            unsigned int *data = get_linklist0(i);
            int degree = getListCount(data);
            if (degree < min_degree) {
                min_degree = degree;
                cur_id = i;
            }
        }

        std::vector<bool> visited(cur_element_count, false);
        id_to_store_order.resize(cur_element_count);
        store_order_to_id.clear();

        std::queue<int> bfs_queue;
        std::vector<std::pair<int, int>> degree_id_vec;  // (degree, id)
        bfs_queue.push(cur_id);
        visited[cur_id] = true;

        while (!bfs_queue.empty()) {
            int count = bfs_queue.size();
            degree_id_vec.clear();
            for (int i = 0; i < count; ++i) {
                int node_id = bfs_queue.front();
                bfs_queue.pop();
                unsigned int *data = get_linklist0(node_id);
                int degree = getListCount(data);
                degree_id_vec.emplace_back(degree, node_id);
            }

            // sort by degree ascending
            std::sort(degree_id_vec.begin(), degree_id_vec.end());
            for (auto &[_, node_id] : degree_id_vec) {
                id_to_store_order[node_id] = store_order_to_id.size();
                store_order_to_id.emplace_back(node_id);

                unsigned int *data = get_linklist0(node_id);
                int degree = getListCount(data);
                tableint *datal = (tableint *) (data + 1);
                for (int i = 0; i < degree; i++) {
                    tableint neighbor_id = datal[i];
                    if (!visited[neighbor_id]) {
                        visited[neighbor_id] = true;
                        bfs_queue.push(neighbor_id);
                    }
                }
            }
        }

        assert(store_order_to_id.size() == cur_element_count);
        assert(id_to_store_order.size() == cur_element_count);

        // reorder all data structures according to store order
        for (int i = 0; i < cur_element_count; ++i) {
            // reorder level 0 linklist
            unsigned int *data = get_linklist0(i);
            int degree = getListCount(data);
            tableint *datal = (tableint *) (data + 1);
            for (int i = 0; i < degree; i++) {
                datal[i] = id_to_store_order[datal[i]];
            }

            // reorder higher level linklists
            int elem_level = element_levels_[i];
            for (int level = 1; level <= elem_level; ++level) {
                unsigned int *data = get_linklist(i, level);
                int degree = getListCount(data);
                tableint *datal = (tableint *) (data + 1);
                for (int i = 0; i < degree; i++) {
                    datal[i] = id_to_store_order[datal[i]];
                }
            }
        }

        // reorder entrypoint
        enterpoint_node_ = id_to_store_order[enterpoint_node_];
    }

    std::priority_queue<std::pair<dist_t, labeltype >>
    searchKnn(const void *query_data, size_t k, BaseFilterFunctor* isIdAllowed = nullptr) const {
        std::priority_queue<std::pair<dist_t, labeltype >> result;
        if (cur_element_count == 0) return result;

        tableint currObj = enterpoint_node_;
        dist_t curdist;
        {
            auto [page_id, offset] = get_level0_offset(enterpoint_node_);
            auto page_handler = page_cache->get_page(page_id);
            PointPageLevel0 page(page_handler, offset, this);
            curdist = fstdistfunc_(query_data, page.get_data(), dist_func_param_);
        }

        for (int level = maxlevel_; level > 0; level--) {
            bool changed = true;
            while (changed) {
                changed = false;

                // unsigned int *data = (unsigned int *) get_linklist(currObj, level);
                // int size = getListCount(data);
                auto [page_id, offset] = get_higher_level_offset(currObj, level);
                PointPageHigherLevel page(page_cache->get_page(page_id), offset);
                int size = page.get_neighbor_count();

                metric_hops++;
                metric_distance_computations+=size;

                // tableint *datal = (tableint *) (data + 1);
                tableint *datal = page.get_neighbor_list();
                for (int i = 0; i < size; i++) {
                    tableint cand = datal[i];
                    if (cand < 0 || cand > max_elements_)
                        throw std::runtime_error("cand error");

                    auto [page_id, offset] = get_level0_offset(cand);
                    auto page_handler = page_cache->get_page(page_id);
                    PointPageLevel0 cand_page(page_handler, offset, this);
                    // dist_t d = fstdistfunc_(query_data, getDataByInternalId(cand), dist_func_param_);
                    dist_t d = fstdistfunc_(query_data, cand_page.get_data(), dist_func_param_);

                    if (d < curdist) {
                        curdist = d;
                        currObj = cand;
                        changed = true;
                    }
                }
            }
        }

        std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst> top_candidates;
        bool bare_bone_search = !num_deleted_ && !isIdAllowed;
        if (bare_bone_search) {
            top_candidates = searchBaseLayerST<true>(
                    currObj, query_data, std::max(ef_, k), isIdAllowed);
        } else {
            top_candidates = searchBaseLayerST<false>(
                    currObj, query_data, std::max(ef_, k), isIdAllowed);
        }

        while (top_candidates.size() > k) {
            top_candidates.pop();
        }
        while (top_candidates.size() > 0) {
            std::pair<dist_t, tableint> rez = top_candidates.top();
            auto [page_id, offset] = get_level0_offset(rez.second);
            auto page_handler = page_cache->get_page(page_id);
            PointPageLevel0 page(page_handler, offset, this);
            // result.push(std::pair<dist_t, labeltype>(rez.first, getExternalLabel(rez.second)));
            result.push(std::pair<dist_t, labeltype>(rez.first, page.get_label()));
            top_candidates.pop();
        }
        return result;
    }


    std::vector<std::pair<dist_t, labeltype >>
    searchStopConditionClosest(
        const void *query_data,
        BaseSearchStopCondition<dist_t>& stop_condition,
        BaseFilterFunctor* isIdAllowed = nullptr) const {
        std::vector<std::pair<dist_t, labeltype >> result;
        if (cur_element_count == 0) return result;

        tableint currObj = enterpoint_node_;
        dist_t curdist = fstdistfunc_(query_data, getDataByInternalId(enterpoint_node_), dist_func_param_);

        for (int level = maxlevel_; level > 0; level--) {
            bool changed = true;
            while (changed) {
                changed = false;
                unsigned int *data;

                data = (unsigned int *) get_linklist(currObj, level);
                int size = getListCount(data);
                metric_hops++;
                metric_distance_computations+=size;

                tableint *datal = (tableint *) (data + 1);
                for (int i = 0; i < size; i++) {
                    tableint cand = datal[i];
                    if (cand < 0 || cand > max_elements_)
                        throw std::runtime_error("cand error");
                    dist_t d = fstdistfunc_(query_data, getDataByInternalId(cand), dist_func_param_);

                    if (d < curdist) {
                        curdist = d;
                        currObj = cand;
                        changed = true;
                    }
                }
            }
        }

        std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst> top_candidates;
        top_candidates = searchBaseLayerST<false>(currObj, query_data, 0, isIdAllowed, &stop_condition);

        size_t sz = top_candidates.size();
        result.resize(sz);
        while (!top_candidates.empty()) {
            result[--sz] = top_candidates.top();
            top_candidates.pop();
        }

        stop_condition.filter_results(result);

        return result;
    }


    void checkIntegrity() {
        int connections_checked = 0;
        std::vector <int > inbound_connections_num(cur_element_count, 0);
        for (int i = 0; i < cur_element_count; i++) {
            for (int l = 0; l <= element_levels_[i]; l++) {
                linklistsizeint *ll_cur = get_linklist_at_level(i, l);
                int size = getListCount(ll_cur);
                tableint *data = (tableint *) (ll_cur + 1);
                std::unordered_set<tableint> s;
                for (int j = 0; j < size; j++) {
                    assert(data[j] < cur_element_count);
                    assert(data[j] != i);
                    inbound_connections_num[data[j]]++;
                    s.insert(data[j]);
                    connections_checked++;
                }
                assert(s.size() == size);
            }
        }
        if (cur_element_count > 1) {
            int min1 = inbound_connections_num[0], max1 = inbound_connections_num[0];
            for (int i=0; i < cur_element_count; i++) {
                assert(inbound_connections_num[i] > 0);
                min1 = std::min(inbound_connections_num[i], min1);
                max1 = std::max(inbound_connections_num[i], max1);
            }
            std::cout << "Min inbound: " << min1 << ", Max inbound:" << max1 << "\n";
        }
        std::cout << "integrity ok, checked " << connections_checked << " connections\n";
    }
};
}  // namespace hnswlib
