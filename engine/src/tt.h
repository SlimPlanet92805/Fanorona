// Shared transposition table.
//
// Replaces the std::unordered_map the port started with, which was the single
// largest cost in the engine: a boxed node per entry, a pointer chase per
// probe, and unbounded growth that measurably slowed the search down as a game
// went on (5.4M nps at depth 7 decaying to 3.0M by depth 9).
//
// This is a fixed-size, power-of-two, 4-way set-associative table. Each bucket
// is exactly one 64-byte cache line, so a probe touches one line instead of
// chasing pointers, and memory use is decided up front instead of growing
// without limit.
//
// Concurrency uses Hyatt's lockless scheme rather than a mutex: the key is
// stored XOR-ed with the payload, so a torn read from a racing writer fails the
// key comparison and is discarded as a miss. Wrong answers are impossible;
// the cost of a race is one wasted probe.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fanorona {

using u64 = uint64_t;

enum : uint8_t { TT_EXACT = 0, TT_LOWER = 1, TT_UPPER = 2 };

struct TTProbe {
    bool hit = false;
    int score = 0;
    int depth = 0;
    int bestMove = -1;
    uint8_t flag = TT_EXACT;
};

class TranspositionTable {
public:
    void resize(size_t megabytes) {
        size_t bytes = megabytes * 1024 * 1024;
        size_t buckets = bytes / sizeof(Bucket);
        size_t pow2 = 1;
        while (pow2 * 2 <= buckets) pow2 *= 2;
        if (pow2 < 1024) pow2 = 1024;
        buckets_.reset(new Bucket[pow2]);
        count_ = pow2;
        mask_ = pow2 - 1;
        clear();
    }

    void clear() {
        std::memset(static_cast<void*>(buckets_.get()), 0, count_ * sizeof(Bucket));
        generation_ = 0;
    }

    /** Call once per search so older entries can be aged out. */
    void new_generation() { generation_ = (generation_ + 1) & GEN_MASK; }

    size_t entries() const { return count_ * WAYS; }

    /**
     * Approximate occupancy, sampled rather than counted. A running total would
     * mean an atomic increment on every store, which with many search threads
     * turns one cache line into a global bottleneck -- it cost more than the
     * table lookups it was measuring.
     */
    size_t filled_permille() const {
        size_t sample = std::min<size_t>(count_, 2048), used = 0;
        for (size_t i = 0; i < sample; i++)
            for (int j = 0; j < WAYS; j++)
                if (buckets_[i].slot[j].data.load(std::memory_order_relaxed) != 0) used++;
        return sample ? (used * 1000) / (sample * WAYS) : 0;
    }

    TTProbe probe(u64 key) const {
        const Bucket& b = buckets_[key & mask_];
        for (int i = 0; i < WAYS; i++) {
            u64 data = b.slot[i].data.load(std::memory_order_relaxed);
            u64 stored = b.slot[i].keyXorData.load(std::memory_order_relaxed);
            if ((stored ^ data) == key && data != 0) {
                TTProbe p;
                p.hit = true;
                p.score = static_cast<int32_t>(data & 0xFFFFFFFFULL);
                p.bestMove = static_cast<int16_t>((data >> 32) & 0xFFFFULL);
                p.depth = static_cast<int8_t>((data >> 48) & 0xFFULL);
                p.flag = static_cast<uint8_t>((data >> 56) & 0x3ULL);
                return p;
            }
        }
        return {};
    }

    void store(u64 key, int depth, int score, uint8_t flag, int bestMove) {
        if (depth < -128) depth = -128;
        if (depth > 127) depth = 127;
        Bucket& b = buckets_[key & mask_];

        int victim = 0;
        int worst = INT32_MAX;
        for (int i = 0; i < WAYS; i++) {
            u64 data = b.slot[i].data.load(std::memory_order_relaxed);
            u64 stored = b.slot[i].keyXorData.load(std::memory_order_relaxed);
            if (data == 0) {  // empty slot: take it
                victim = i;
                worst = INT32_MIN;
                break;
            }
            if ((stored ^ data) == key) {  // same position: always refresh
                victim = i;
                worst = INT32_MIN;
                break;
            }
            // Otherwise keep what is most worth keeping. Depth is the value of
            // an entry -- a depth-20 result cost far more to compute than a
            // depth-2 one -- but a deep entry from a stale generation is about
            // a position the search has moved past, so age counts against it.
            // This is why plain LRU would be wrong here: recency is a poor
            // proxy for how expensive an entry was to produce.
            int entryDepth = static_cast<int8_t>((data >> 48) & 0xFFULL);
            int entryGen = static_cast<int>((data >> 58) & GEN_MASK);
            int age = (generation_ - entryGen) & GEN_MASK;
            int value = entryDepth - age * 8;
            if (value < worst) {
                worst = value;
                victim = i;
            }
        }

        u64 data = (static_cast<u64>(static_cast<uint32_t>(score))) |
                   (static_cast<u64>(static_cast<uint16_t>(bestMove)) << 32) |
                   (static_cast<u64>(static_cast<uint8_t>(depth)) << 48) |
                   (static_cast<u64>(flag & 0x3) << 56) |
                   (static_cast<u64>(generation_ & GEN_MASK) << 58);
        if (data == 0) data = 1;  // 0 is reserved to mean "empty"
        b.slot[victim].data.store(data, std::memory_order_relaxed);
        b.slot[victim].keyXorData.store(key ^ data, std::memory_order_relaxed);
    }

    /**
     * The deepest `maxEntries` entries, serialised.
     *
     * "Most valuable" means deepest, not most recent. A depth-20 result is the
     * distilled product of millions of nodes; a depth-2 result costs almost
     * nothing to recompute. Keeping by recency -- an LRU -- would throw away
     * exactly the expensive knowledge that makes the engine faster next time
     * and keep the cheap noise instead.
     *
     * Returning bytes rather than writing a file is what lets the browser build
     * persist too: the same blob goes to disk natively and into IndexedDB on
     * the web.
     */
    std::vector<uint8_t> export_bytes(size_t maxEntries) const {
        auto depth_of = [](u64 d) { return static_cast<int>(static_cast<int8_t>((d >> 48) & 0xFF)); };

        // Pick the cut-off with a histogram rather than by gathering every live
        // entry and sorting. A full table holds millions of them, and the
        // gather alone allocated tens of megabytes -- enough to exhaust the
        // WebAssembly heap, where the whole point is to save the thing.
        // Depth is one signed byte, so 256 counters describe the distribution
        // exactly, and only the entries that make the cut are ever collected.
        size_t hist[256] = {};
        for (size_t i = 0; i < count_; i++) {
            for (int j = 0; j < WAYS; j++) {
                u64 data = buckets_[i].slot[j].data.load(std::memory_order_relaxed);
                if (data != 0) hist[(data >> 48) & 0xFF]++;
            }
        }
        // Walk depths from deepest to shallowest until the budget is used up.
        int cutoff = -128;
        size_t budget = maxEntries, taken = 0;
        for (int d = 127; d >= -128; d--) {
            size_t c = hist[static_cast<uint8_t>(d)];
            if (taken + c > budget) {
                cutoff = d;
                break;
            }
            taken += c;
            cutoff = d;
            if (taken == budget) break;
        }

        std::vector<std::pair<u64, u64>> live;  // (key, data)
        live.reserve(std::min(maxEntries, taken + hist[static_cast<uint8_t>(cutoff)]) + 1);
        for (size_t i = 0; i < count_ && live.size() < maxEntries; i++) {
            for (int j = 0; j < WAYS; j++) {
                u64 data = buckets_[i].slot[j].data.load(std::memory_order_relaxed);
                if (data == 0 || depth_of(data) < cutoff) continue;
                u64 key = buckets_[i].slot[j].keyXorData.load(std::memory_order_relaxed) ^ data;
                live.emplace_back(key, data);
                if (live.size() >= maxEntries) break;
            }
        }

        std::vector<uint8_t> out(16 + live.size() * 16);
        uint32_t magic = MAGIC, version = VERSION;
        uint64_t n = live.size();
        std::memcpy(out.data(), &magic, 4);
        std::memcpy(out.data() + 4, &version, 4);
        std::memcpy(out.data() + 8, &n, 8);
        size_t off = 16;
        for (const auto& [key, data] : live) {
            std::memcpy(out.data() + off, &key, 8);
            std::memcpy(out.data() + off + 8, &data, 8);
            off += 16;
        }
        return out;
    }

    /** @return number of entries restored, or -1 if the blob was unusable. */
    long long import_bytes(const uint8_t* data, size_t len) {
        if (data == nullptr || len < 16) return -1;
        uint32_t magic, version;
        uint64_t n;
        std::memcpy(&magic, data, 4);
        std::memcpy(&version, data + 4, 4);
        std::memcpy(&n, data + 8, 8);
        // A cache is never worth migrating: on any mismatch just start empty.
        if (magic != MAGIC || version != VERSION) return -1;
        if (n > (len - 16) / 16) n = (len - 16) / 16;

        long long restored = 0;
        for (uint64_t i = 0; i < n; i++) {
            u64 key, d;
            std::memcpy(&key, data + 16 + i * 16, 8);
            std::memcpy(&d, data + 16 + i * 16 + 8, 8);
            if (place_loaded(key, d)) restored++;
        }
        return restored;
    }

    /** Writes {@link export_bytes} to disk. */
    bool save(const std::string& path, size_t maxEntries) const {
        std::vector<uint8_t> blob = export_bytes(maxEntries);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(blob.data()),
                  static_cast<std::streamsize>(blob.size()));
        return out.good();
    }

    /** @return number of entries restored, or -1 if the file was unusable. */
    long long load(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return -1;
        std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        return import_bytes(blob.data(), blob.size());
    }

private:
    /** Deepest wins the slot, so a full file cannot evict its own best. */
    bool place_loaded(u64 key, u64 data) {
        Bucket& b = buckets_[key & mask_];
        int victim = 0, worst = INT32_MAX;
        for (int j = 0; j < WAYS; j++) {
            u64 d = b.slot[j].data.load(std::memory_order_relaxed);
            int v = (d == 0) ? INT32_MIN : static_cast<int8_t>((d >> 48) & 0xFF);
            if (v < worst) {
                worst = v;
                victim = j;
            }
        }
        if (worst > static_cast<int8_t>((data >> 48) & 0xFF)) return false;
        b.slot[victim].data.store(data, std::memory_order_relaxed);
        b.slot[victim].keyXorData.store(key ^ data, std::memory_order_relaxed);
        return true;
    }

    static constexpr uint32_t MAGIC = 0x544E4146;  // "FANT"
    static constexpr uint32_t VERSION = 1;
    static constexpr int WAYS = 4;
    static constexpr int GEN_MASK = 0x3F;

    struct Slot {
        std::atomic<u64> keyXorData{0};
        std::atomic<u64> data{0};
    };
    struct alignas(64) Bucket {
        Slot slot[WAYS];
    };
    static_assert(sizeof(Bucket) == 64, "bucket must be one cache line");

    std::unique_ptr<Bucket[]> buckets_;
    size_t count_ = 0, mask_ = 0;
    int generation_ = 0;
};

}  // namespace fanorona
