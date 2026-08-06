// Position evaluation, with a vectorised fast path.
//
// The evaluation is a mask-weighted sum: for every occupied square, add that
// square's table value. Scalar code walks the set bits one at a time, so it
// costs one iteration per piece -- up to 44 per call, at a leaf node, which is
// the hottest place in the engine.
//
// A 45-square board fits in a 64-bit mask, and AVX-512 takes mask registers as
// first-class operands. So the whole loop collapses into: use the bitboard *as*
// a mask, zero out the unoccupied lanes of a constant byte vector, and
// horizontally sum with VPSADBW. Four instructions instead of a loop, with no
// data-dependent branching.
#include "fanorona.h"

#include <algorithm>
#include <bit>
#include <cstdlib>

#if defined(__x86_64__) || defined(_M_X64)
#define FANORONA_X86 1
#include <immintrin.h>
#endif

namespace fanorona {

int (*evaluate)(const GameState&) = nullptr;

/**
 * Ceiling on the "close the distance" term.
 *
 * The term exists so a winning engine hunts the survivors down instead of
 * shuffling. As written in the Java original it *summed* a distance for each
 * of one's own pieces to one arbitrarily chosen enemy stone -- the lowest
 * indexed one -- so with twenty-odd pieces on the board it reached -528,
 * outweighing five pieces of material. That is not a tie-breaker, it is a
 * distortion.
 *
 * Gating it on the enemy piece count was measured and bought nothing (200
 * games, 49.8% vs 50.2%; and the AVX-512 evaluator already makes the term
 * nearly free, so there was no speedup either). What was actually wrong is the
 * magnitude, so that is what is bounded: the sum is divided by one's own piece
 * count, making it a *mean* distance, and then clamped here. The gradient is
 * unchanged -- moving toward the enemy still scores better -- but the term can
 * no longer outweigh material.
 *
 * The mean is bounded by the board diameter anyway (max Manhattan distance 12,
 * doubled = 24), so the default is not binding; it exists as a tuning knob and
 * as a hard guarantee.
 */
int eval_hunt_cap = 50;

namespace {

// Byte-wide copies of the lookup tables, padded to 64 lanes. Values are small
// (POS_VAL <= 9, rows <= 4, cols <= 8) so a byte per square is plenty, and the
// per-lane sums stay well inside what VPSADBW accumulates.
alignas(64) int8_t POS_VAL_B[64];
alignas(64) int8_t POS_R_B[64];
alignas(64) int8_t POS_C_B[64];

constexpr u64 BOARD_MASK = (1ULL << NUM_POS) - 1;

// Shared by both evaluators so they cannot drift apart. A negative cap selects
// the original un-normalised sum; that exists only so `--match` can play the
// old evaluation against the new one, and is never used in a shipped build.
inline int hunt_penalty(int dSum, int myC) {
    if (eval_hunt_cap < 0) return dSum * 2;
    return std::min(dSum * 2 / myC, eval_hunt_cap);
}

int evaluate_scalar(const GameState& s) {
    int myC = std::popcount(s.myPieces), oppC = std::popcount(s.oppPieces);
    int sc = (myC - oppC) * 100;
    u64 t = s.myPieces;
    while (t != 0) {
        sc += POS_VAL[std::countr_zero(t)];
        t &= (t - 1);
    }
    t = s.oppPieces;
    while (t != 0) {
        sc -= POS_VAL[std::countr_zero(t)];
        t &= (t - 1);
    }
    // When ahead, close the distance to the surviving enemy piece; without this
    // the engine can be up material and shuffle forever instead of finishing.
    if (myC > oppC && oppC > 0) {
        int enemyP = std::countr_zero(s.oppPieces);
        int er = POS_R[enemyP], ec = POS_C[enemyP];
        u64 my = s.myPieces;
        int dSum = 0;
        while (my != 0) {
            int p = std::countr_zero(my);
            dSum += std::abs(POS_R[p] - er) + std::abs(POS_C[p] - ec);
            my &= (my - 1);
        }
        sc -= hunt_penalty(dSum, myC);
    }
    return sc;
}

#ifdef FANORONA_X86

__attribute__((target("avx512f,avx512bw")))
inline int hsum_masked(__mmask64 m, __m512i values) {
    __m512i sel = _mm512_maskz_mov_epi8(m, values);
    // VPSADBW: eight independent sums of eight bytes each, widened to u64.
    __m512i sums = _mm512_sad_epu8(sel, _mm512_setzero_si512());
    return static_cast<int>(_mm512_reduce_add_epi64(sums));
}

__attribute__((target("avx512f,avx512bw")))
int evaluate_avx512(const GameState& s) {
    const __m512i vals = _mm512_load_si512(POS_VAL_B);
    __mmask64 mm = static_cast<__mmask64>(s.myPieces & BOARD_MASK);
    __mmask64 om = static_cast<__mmask64>(s.oppPieces & BOARD_MASK);

    int myC = std::popcount(s.myPieces), oppC = std::popcount(s.oppPieces);
    int sc = (myC - oppC) * 100;
    sc += hsum_masked(mm, vals);
    sc -= hsum_masked(om, vals);

    if (myC > oppC && oppC > 0) {
        int enemyP = std::countr_zero(s.oppPieces);
        // |row - er| + |col - ec| for all 64 lanes at once, then mask and sum.
        __m512i dr = _mm512_abs_epi8(
            _mm512_sub_epi8(_mm512_load_si512(POS_R_B), _mm512_set1_epi8((char)POS_R[enemyP])));
        __m512i dc = _mm512_abs_epi8(
            _mm512_sub_epi8(_mm512_load_si512(POS_C_B), _mm512_set1_epi8((char)POS_C[enemyP])));
        int dSum = hsum_masked(mm, _mm512_add_epi8(dr, dc));
        // Scalar post-step on the already-reduced sum; the vector work above is
        // untouched. Must match evaluate_scalar bit for bit.
        sc -= hunt_penalty(dSum, myC);
    }
    return sc;
}

bool have_avx512() {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw");
}

#endif  // FANORONA_X86

const char* g_backend = "scalar";

}  // namespace

void init_eval(bool forceScalar) {
    for (int i = 0; i < 64; i++) {
        POS_VAL_B[i] = (i < NUM_POS) ? static_cast<int8_t>(POS_VAL[i]) : 0;
        POS_R_B[i] = (i < NUM_POS) ? static_cast<int8_t>(POS_R[i]) : 0;
        POS_C_B[i] = (i < NUM_POS) ? static_cast<int8_t>(POS_C[i]) : 0;
    }
#ifdef FANORONA_X86
    if (!forceScalar && have_avx512()) {
        evaluate = &evaluate_avx512;
        g_backend = "avx512";
        return;
    }
#endif
    evaluate = &evaluate_scalar;
    g_backend = "scalar";
}

const char* eval_backend_name() { return g_backend; }

}  // namespace fanorona
