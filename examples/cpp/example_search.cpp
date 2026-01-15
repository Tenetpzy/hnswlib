#include "../../hnswlib/hnswlib.h"
#include "async_simple/coro/SyncAwait.h"
#include "async_simple/coro/Lazy.h"
#include "async_simple/coro/Collect.h"

#include <unistd.h>
#include <vector>

using namespace async_simple::coro;

int main() {
    int dim = 16;               // Dimension of the elements
    int max_elements = 10000;   // Maximum number of elements, should be known beforehand
    int M = 16;                 // Tightly connected with internal dimensionality of the data
                                // strongly affects the memory consumption
    int ef_construction = 200;  // Controls index search speed/build speed tradeoff
    int cache_page_num = 48;
    int search_beam_width = 2;

    // Initing index
    hnswlib::L2Space space(dim);
    hnswlib::HierarchicalNSW<float>* alg_hnsw = new hnswlib::HierarchicalNSW<float>(&space, max_elements, M, ef_construction);

    // Generate random data
    std::mt19937 rng;
    rng.seed(47);
    std::uniform_real_distribution<> distrib_real;
    float* data = new float[dim * max_elements];
    for (int i = 0; i < dim * max_elements; i++) {
        data[i] = distrib_real(rng);
    }

    // Add data to index
    for (int i = 0; i < max_elements; i++) {
        alg_hnsw->addPoint(data + i * dim, i);
    }

    // // Query the elements for themselves and measure recall
    // float correct = 0;
    // for (int i = 0; i < max_elements; i++) {
    //     std::priority_queue<std::pair<float, hnswlib::labeltype>> result = alg_hnsw->searchKnn(data + i * dim, 1);
    //     hnswlib::labeltype label = result.top().second;
    //     if (label == i) correct++;
    // }
    // float recall = correct / max_elements;
    // std::cout << "Recall: " << recall << "\n";

    // Serialize index
    std::string hnsw_path = "hnsw.bin";
    alg_hnsw->saveIndex(hnsw_path);
    // output hnsw.bin size
    std::ifstream in(hnsw_path, std::ifstream::ate | std::ifstream::binary);
    std::cout << "Serialized index size: " << in.tellg() << " bytes\n";
    sleep(3);

    delete alg_hnsw;

    // Deserialize index and check recall
    alg_hnsw = new hnswlib::HierarchicalNSW<float>(&space, hnsw_path, cache_page_num * 16 * 1024, 4);
    float correct = 0;
    auto executors = alg_hnsw->executors();
    std::vector<RescheduleLazy<std::priority_queue<std::pair<float, unsigned long>>>> tasks;
    for (int i = 0; i < max_elements; i++) {
        if (i % 100 == 0) {
            // std::cout << "Searching for element " << i << "/" << max_elements << "\n";
            // std::cout << "Queued tasks: " << tasks.size() << "\n";
        }
        tasks.push_back(alg_hnsw->searchKnn(data + i * dim, 1).via(executors[i % executors.size()]));
        // std::priority_queue<std::pair<float, hnswlib::labeltype>> result = async_simple::coro::syncAwait(alg_hnsw->searchKnn(data + i * dim, 1).via(alg_hnsw->executors()[0]));
        // hnswlib::labeltype label = result.top().second;
        // if (label == i) correct++;
    }

    int max_concurrency = cache_page_num / search_beam_width;
    auto results = async_simple::coro::syncAwait(collectAllWindowedPara(max_concurrency, false, std::move(tasks)));
    for (int i = 0; i < max_elements; i++) {
        hnswlib::labeltype label = results[i].value().top().second;
        if (label == i) correct++;
    }

    float recall = (float)correct / max_elements;
    std::cout << "Recall of deserialized index: " << recall << "\n";
    std::cout 
        << "Cache hit rate: "
        << alg_hnsw->get_cache_hit_rate() << "\n"
        << "IO operations: "
        << alg_hnsw->get_io_op_num() << "\n"
        << "Memory transfer (KB): "
        << alg_hnsw->get_memory_transfer_kb() << "\n";

    delete[] data;
    delete alg_hnsw;
    return 0;
}
