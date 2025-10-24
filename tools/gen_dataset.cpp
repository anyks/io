#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>

// Generate deterministic dataset of binary payload files with sizes in [min_mb, max_mb] (MB).
// Ensures the total size of all generated files does not exceed max_total_mb (default: 100 MB).
// Usage: gen_dataset <out_dir> <count> <min_mb> <max_mb> <seed> [max_total_mb]

static void fill_payload(std::vector<char> &buf, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> dist(0, 255);
    for (auto &c : buf) c = static_cast<char>(dist(rng));
}

int main(int argc, char **argv) {
    if (argc < 6) {
        std::cerr << "Usage: " << argv[0] << " <out_dir> <count> <min_mb> <max_mb> <seed> [max_total_mb]\n";
        return 2;
    }
    std::filesystem::path out_dir = argv[1];
    uint64_t count = std::strtoull(argv[2], nullptr, 10);
    int min_mb = std::atoi(argv[3]);
    int max_mb = std::atoi(argv[4]);
    uint64_t seed = std::strtoull(argv[5], nullptr, 10);
    uint64_t max_total_mb = 100; // default cap
    if (argc >= 7) {
        max_total_mb = std::strtoull(argv[6], nullptr, 10);
    }
    if (count == 0 || min_mb <= 0 || max_mb < min_mb) {
        std::cerr << "Invalid arguments" << std::endl;
        return 2;
    }
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
        std::cerr << "Failed to create output dir: " << ec.message() << std::endl;
        return 3;
    }

    std::mt19937 size_rng(static_cast<uint32_t>(seed));
    std::uniform_int_distribution<int> dist_mb(min_mb, max_mb);

    const uint64_t cap_bytes = max_total_mb * 1024ull * 1024ull;
    uint64_t total_bytes = 0;

    for (uint64_t i = 0; i < count; ++i) {
        if (total_bytes >= cap_bytes) break;
        int mb = dist_mb(size_rng);
        uint64_t sz = static_cast<uint64_t>(mb) * 1024ull * 1024ull;
        // If adding this file exceeds the cap, trim or stop
        if (total_bytes + sz > cap_bytes) {
            uint64_t remain = cap_bytes - total_bytes;
            if (remain == 0) break;
            // Try not to create tiny files; but if remain < 1MB and this is the first file, still write what's left
            sz = remain;
        }
        std::vector<char> buf(static_cast<size_t>(sz));
        uint64_t msg_seed = seed ^ (i * 0x9E3779B185EBCA87ull);
        fill_payload(buf, msg_seed);
        char name[256];
        unsigned long long mb_l = static_cast<unsigned long long>(sz / (1024ull * 1024ull));
        if (mb_l == 0 && sz > 0) mb_l = 1; // label at least 1MB when trimmed below 1MB
        std::snprintf(name, sizeof(name), "payload_%010llu_%02lluMB.bin", (unsigned long long)i, mb_l);
        std::filesystem::path fp = out_dir / name;
        std::ofstream ofs(fp, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            std::cerr << "Failed to open file for write: " << fp << std::endl;
            return 4;
        }
        ofs.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        if (!ofs) {
            std::cerr << "Write failed: " << fp << std::endl;
            return 5;
        }
        std::cout << fp << "\n";
        total_bytes += sz;
    }
    return 0;
}
