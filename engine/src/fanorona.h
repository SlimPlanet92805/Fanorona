// C++20 Fanorona engine.
//
// Verified against the Java reference implementation in src/main/java/org/willy/
// by scripts/check-parity.py. Two invariants govern changes here:
//
//   * perft (pure move generation) must NEVER change. It is the rule oracle.
//   * search node counts must not change for optimisations that are meant to be
//     pure speedups -- incremental hashing, allocation removal, vectorised
//     evaluation. If those move the node count, they changed behaviour and are
//     wrong. Only deliberate search changes (the flat transposition table,
//     parallel search) are allowed to move it, and then the golden file is
//     re-baselined on purpose.
#pragma once

#include <atomic>
#include <bit>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fanorona {

constexpr int NUM_POS = 45;
constexpr int ROWS = 5;
constexpr int COLS = 9;
constexpr int STOP_ACTION = 720;

constexpr int INF = 100000000;
constexpr int MATE_SCORE = 90000000;
constexpr int MATE_THRESHOLD = 80000000;

using u64 = uint64_t;

// ---------------------------------------------------------------- tables

extern int ADJ_INDEX[NUM_POS][8];
extern const int OPPOSITE_DIR[8];
extern const int POS_VAL[NUM_POS];
extern int POS_R[NUM_POS];
extern int POS_C[NUM_POS];
extern char POS_NAMES[NUM_POS][4];  // "a5", "b5", ... for readable move lists

void init_tables();

// ---------------------------------------------------------------- state

struct GameState {
    u64 myPieces = 0, oppPieces = 0;

    // Two running hashes so the Zobrist key can be maintained incrementally
    // across a turn change. Bitboards are stored relative to the side to move,
    // so ending a turn swaps them -- which under a single hash would move every
    // piece to the other key table and force a full recompute. hA is the hash
    // as stored (mine -> table 0, theirs -> table 1); hB is the same position
    // with the tables exchanged. Ending a turn is then just swap(hA, hB).
    u64 hA = 0, hB = 0;

    int player = 0;
    bool inCombo = false;
    int comboPiece = -1, prevPos = -1;
    u64 visitedMask = 0;
    int lastDir = -1;
    u64 zobristHash = 0;  // hA ^ (player == -1 ? T : 0)

    void init_hash();
};

enum class MoveType : int8_t { Approach, Withdrawal, Move, Stop };
const char* to_string(MoveType t);

// Victims as a bitmask rather than a list: the search only ever needs the count
// (for ordering) and the set (to clear from the board), and this keeps Move
// small enough to live in a flat arena instead of the heap.
struct Move {
    int16_t actionId = 0;
    int8_t from = -1, to = -1;
    MoveType type = MoveType::Move;
    u64 victimMask = 0;

    int victims() const { return std::popcount(victimMask); }
};

struct StepResult {
    GameState state;
    bool win = false;
};

// Upper bound on moves from one position: at most 22 pieces x 8 directions x
// {approach, withdrawal}. Quiet-move generation produces strictly fewer.
constexpr int MAX_MOVES = 384;
constexpr int MAX_PLY = 128;

// Per-thread scratch space for move generation, indexed by ply. Removing the
// per-node heap allocation is one of the larger wins in the search.
struct MoveArena {
    MoveArena() : buf(static_cast<size_t>(MAX_PLY) * MAX_MOVES) {}
    Move* at(int ply) { return buf.data() + static_cast<size_t>(ply) * MAX_MOVES; }
    std::vector<Move> buf;
};

int generate_moves(const GameState& s, Move* out);
std::vector<Move> get_detailed_moves(const GameState& s);  // convenience, for tools
bool has_capture_moves(const GameState& s, int p);
StepResult step(const GameState& s, int action);
int derive_last_dir(int prevPos, int comboPiece);

// ---------------------------------------------------------------- zobrist

namespace zobrist {
extern u64 P[NUM_POS][2];
extern u64 T;
void init();
u64 compute(const GameState& s);
}  // namespace zobrist

// ---------------------------------------------------------------- eval

// Chosen at startup: AVX-512 where available, else AVX2/scalar. The hot part is
// a mask-weighted sum over the board, which maps directly onto a mask register.
extern int (*evaluate)(const GameState& s);
/** Ceiling on the endgame "hunt them down" term. See eval.cpp. */
extern int eval_hunt_cap;
void init_eval(bool forceScalar = false);
const char* eval_backend_name();

// ---------------------------------------------------------------- codec

GameState from_text(const std::string& line);
std::string to_text(const GameState& s);

// ---------------------------------------------------------------- search

struct SearchConfig {
    /** Hard cap on iterative-deepening depth. */
    int maxDepth = 1000;
    /** Wall-clock budget per move. The main lever for how strong the AI feels. */
    int64_t timeLimitMs = 1000;
    /** Search threads (Lazy SMP). 1 keeps the search bit-for-bit deterministic. */
    int threads = 1;
    /** Transposition table size. */
    int hashMB = 256;
    /**
     * Whether to reconstruct the principal variation for the log.
     *
     * Off by default because it is not free: it walks the table replaying
     * moves on scratch boards. In trash-talk mode nothing ever reads it, so
     * paying for it on every move was waste.
     */
    bool wantPv = false;
};

struct SearchResult {
    int bestMove = 0;
    int score = 0;
    int depth = 0;
    int64_t nodes = 0;
    int64_t ttHits = 0;
    /** Why iterative deepening stopped: "Time", "MaxDepth" or "Mate". */
    const char* stopReason = "MaxDepth";
    /** The line the engine expects, read back out of the table. Empty unless
     *  SearchConfig::wantPv was set. */
    std::string pv;
    /** Effort spent on the first root move, and on the last iteration overall,
     *  which together give the "Root:%" move-ordering quality figure. */
    int64_t rootNodesBest = 0, iterationNodes = 0;
    /**
     * The reply the engine expects from the opponent, or -1 if it has no view.
     * Read from the table entry for the position after our own move; comparing
     * it against what the human actually plays is what drives the Hit/Miss
     * marker in the log.
     */
    int predictedReply = -1;
};

class TranspositionTable;

// Per-thread search state. Everything mutable in the search lives here so the
// only thing shared between threads is the transposition table, which is what
// makes Lazy SMP work: the threads cooperate purely by filling in each other's
// table entries.
struct Worker {
    Worker();
    ~Worker();
    std::unique_ptr<MoveArena> arena;
    std::vector<int> history;  // 46*46, flattened
    int64_t nodes = 0, ttHits = 0;
    /**
     * Nodes spent on the first root move of the last iteration. Reported as
     * "Root:%" -- when move ordering is working, the best move is searched
     * first and takes the bulk of the effort, so a high figure means the
     * ordering is doing its job and a low one means the search keeps changing
     * its mind.
     */
    int64_t rootNodesBest = 0;
    int id = 0;
};

class AIPlayer {
public:
    explicit AIPlayer(const SearchConfig& cfg);
    ~AIPlayer();

    SearchResult think(const GameState& root);

    /** Positions already seen in the real game, for repetition detection. */
    void record_state(u64 hash) { globalHistory_.push_back(hash); }
    void reset_game() { globalHistory_.clear(); }

    const SearchConfig& config() const { return config_; }
    TranspositionTable& tt() { return *tt_; }

    /**
     * Applies new settings between moves. Resizing the table necessarily
     * discards what it holds, so that only happens when the size really
     * changed -- adjusting thinking time must not throw away the learning.
     */
    void set_config(const SearchConfig& cfg);

private:
    struct RootMove;

    int negascout(Worker& w, const GameState& s, int d, int alpha, int beta, int ply);
    int search_root(Worker& w, const GameState& root, std::vector<RootMove>& moves,
                    int d, bool amWinning, int& bestMoveOut);
    void bump_history(Worker& w, int from, int to, int bonus);
    bool out_of_time() const;
    std::string narrative_pv(const GameState& root, int firstMove, int maxSteps) const;

    SearchConfig config_;
    std::unique_ptr<TranspositionTable> tt_;
    std::vector<u64> globalHistory_;

    // Shared across threads for the duration of one search.
    std::atomic<bool> stop_{false};
    std::atomic<int> mainDepth_{1};
    int64_t deadline_ = 0;
};

}  // namespace fanorona
