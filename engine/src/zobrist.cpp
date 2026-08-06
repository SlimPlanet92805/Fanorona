// Port of Zobrist.java.
//
// The Java side seeds its keys from `new Random(12345)`, so reproducing them
// means reproducing java.util.Random's linear congruential generator exactly.
// This matters twice over: the parity check compares position hashes directly,
// and an existing fanorona_memory.dat stays loadable by both builds only if the
// keys are bit-identical.
#include "fanorona.h"

#include <bit>

namespace fanorona::zobrist {

u64 P[NUM_POS][2];
u64 T;

namespace {

// java.util.Random. The details that bite: the seed is scrambled on
// construction, only 48 bits are kept, next() returns a *signed* 32-bit value,
// and nextLong() adds rather than ORs the two halves -- so the low word's sign
// bit borrows from the high word.
class JavaRandom {
public:
    explicit JavaRandom(int64_t seed)
        : seed_((static_cast<u64>(seed) ^ 0x5DEECE66DULL) & MASK) {}

    int32_t next(int bits) {
        seed_ = (seed_ * 0x5DEECE66DULL + 0xBULL) & MASK;
        return static_cast<int32_t>(seed_ >> (48 - bits));
    }

    int64_t next_long() {
        int64_t hi = static_cast<int64_t>(next(32)) << 32;
        return hi + static_cast<int64_t>(next(32));
    }

private:
    static constexpr u64 MASK = (1ULL << 48) - 1;
    u64 seed_;
};

}  // namespace

void init() {
    JavaRandom r(12345);
    for (int i = 0; i < NUM_POS; i++) {
        P[i][0] = static_cast<u64>(r.next_long());
        P[i][1] = static_cast<u64>(r.next_long());
    }
    T = static_cast<u64>(r.next_long());
}

u64 compute(const GameState& s) {
    u64 h = 0, m = s.myPieces, o = s.oppPieces;
    while (m != 0) {
        h ^= P[std::countr_zero(m)][0];
        m &= (m - 1);
    }
    while (o != 0) {
        h ^= P[std::countr_zero(o)][1];
        o &= (o - 1);
    }
    if (s.player == -1) h ^= T;
    return h;
}

}  // namespace fanorona::zobrist
