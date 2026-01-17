#include "../../hnswlib/hnswlib.h"


int main() {
    int dim = 16;               // Dimension of the elements
    int max_elements = 10000;   // Maximum number of elements, should be known beforehand
    int M = 16;                 // Tightly connected with internal dimensionality of the data
                                // strongly affects the memory consumption
    int ef_construction = 200;  // Controls index search speed/build speed tradeoff

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
    delete alg_hnsw;

    // Deserialize index and check recall
    alg_hnsw = new hnswlib::HierarchicalNSW<float>(&space, hnsw_path, 48 * 16384);
    float correct = 0;
    for (int i = 0; i < max_elements; i++) {
        std::priority_queue<std::pair<float, hnswlib::labeltype>> result = alg_hnsw->searchKnn(data + i * dim, 1);
        hnswlib::labeltype label = result.top().second;
        if (label == i) correct++;
    }

    auto latencies = alg_hnsw->get_latency_ms();
    double avg_latency = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
    double qps = alg_hnsw->get_qps(avg_latency, 4);
    double avg_depth_mean = alg_hnsw->get_avg_depth_mean();
    double avg_depth_std = alg_hnsw->get_avg_depth_std();

    float recall = (float)correct / max_elements;
    std::cout << "Recall of deserialized index: " << recall << "\n";
    std::cout 
        << "Cache hit rate: "
        << alg_hnsw->get_cache_hit_rate() << "\n"
        << "IO operations: "
        << alg_hnsw->get_io_op_num() << "\n"
        << "Memory transfer (KB): "
        << alg_hnsw->get_memory_transfer_kb() << "\n"
        << "Average latency (ms): "
        << avg_latency << "\n"
        << "QPS: "
        << qps << "\n"
        << "Avg channel depth: "
        << avg_depth_mean << " +/- " << avg_depth_std << "\n";

    delete[] data;
    delete alg_hnsw;
    return 0;
}
