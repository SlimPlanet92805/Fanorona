// Iterative deepening + NegaScout over a shared transposition table,
// optionally across several threads (Lazy SMP).
//
// Lazy SMP is the right shape of parallelism here because the search is
// fundamentally sequential -- alpha-beta's whole point is that earlier results
// prune later ones, so splitting the tree cleanly is hard and mostly
// counterproductive. Instead every thread searches the same root, with helpers
// staggered onto different depths, and they cooperate only through the shared
// table: whatever one thread proves, the others get to skip. It scales less
// than linearly by design, but it is robust and it never changes the answer.
//
// Thread 1 is always the plain deterministic search, so `--threads=1` remains
// reproducible and can be diffed against the Java reference.
#include "fanorona.h"
#include "tt.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <thread>

namespace fanorona {

namespace {

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

constexpr int HISTORY_MAX = 1 << 24;

}  // namespace

Worker::Worker() : arena(new MoveArena()), history(46 * 46, 0) {}
Worker::~Worker() = default;

struct AIPlayer::RootMove {
    Move move;
    int score = -INF;
};

AIPlayer::AIPlayer(const SearchConfig& cfg) : config_(cfg), tt_(new TranspositionTable()) {
    tt_->resize(static_cast<size_t>(std::max(1, cfg.hashMB)));
}

AIPlayer::~AIPlayer() = default;

void AIPlayer::set_config(const SearchConfig& cfg) {
    bool resize = cfg.hashMB != config_.hashMB;
    config_ = cfg;
    if (resize) tt_->resize(static_cast<size_t>(std::max(1, cfg.hashMB)));
}

bool AIPlayer::out_of_time() const { return now_ms() > deadline_; }

/**
 * Reads the expected line of play back out of the transposition table.
 *
 * Purely diagnostic, but it is the most informative thing the log can show:
 * it is the engine explaining what it thinks is about to happen. Entries can
 * be evicted mid-line, so this stops as soon as the table stops answering
 * rather than pretending to a longer line than it can actually justify.
 */
std::string AIPlayer::narrative_pv(const GameState& root, int firstMove, int maxSteps) const {
    std::string out = "A:";
    GameState curr = root;
    int mid = firstMove;
    int startP = root.player;
    int lastP = startP;
    std::vector<u64> seen;

    for (int i = 0; i < maxSteps; i++) {
        if (std::find(seen.begin(), seen.end(), curr.zobristHash) != seen.end()) break;
        seen.push_back(curr.zobristHash);

        // Decode the move straight from its action id instead of regenerating
        // the position's entire move list just to look one up. The checks below
        // are only a safety net -- a stored best move came from a real search
        // of this position -- but step() indexes bitboards with `to`, so a
        // malformed id must never reach it.
        int from = -1, to = -1;
        if (mid != STOP_ACTION) {
            int norm = mid >= 360 ? mid - 360 : mid;
            from = norm / 8;
            int dir = norm % 8;
            if (norm < 0 || from >= NUM_POS) break;
            to = ADJ_INDEX[from][dir];
            if (to < 0) break;
            if (((curr.myPieces >> from) & 1ULL) == 0) break;
            if (((curr.myPieces | curr.oppPieces) >> to) & 1ULL) break;
        } else if (!curr.inCombo) {
            break;  // "stop" only exists mid-chain
        }

        if (curr.player != lastP) {
            out += (curr.player == startP) ? " | A:" : " | H:";
            lastP = curr.player;
        }
        if (mid == STOP_ACTION) {
            out += "Stop ";
        } else {
            out += POS_NAMES[from];
            out += "-";
            out += POS_NAMES[to];
            out += " ";
        }

        StepResult res = step(curr, mid);
        if (res.win) {
            out += "#WIN ";
            break;
        }
        curr = res.state;
        TTProbe e = tt_->probe(curr.zobristHash);
        if (!e.hit || e.bestMove < 0) break;
        mid = e.bestMove;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

void AIPlayer::bump_history(Worker& w, int from, int to, int bonus) {
    int& slot = w.history[from * 46 + to];
    int v = slot + bonus;
    if (v >= HISTORY_MAX) {
        // Halve the whole table rather than let it overflow. Relative order is
        // what the sort cares about, and halving preserves it.
        for (int& c : w.history) c >>= 1;
        v >>= 1;
    }
    slot = v;
}

int AIPlayer::negascout(Worker& w, const GameState& s, int d, int alpha, int beta, int ply) {
    // Checking the clock on every node would cost more than the search saves;
    // every 4096th is frequent enough to stop promptly.
    if ((w.nodes++ & 4095) == 0 && out_of_time()) stop_.store(true, std::memory_order_relaxed);
    if (stop_.load(std::memory_order_relaxed)) return alpha;

    TTProbe e = tt_->probe(s.zobristHash);
    if (e.hit && e.depth >= d) {
        w.ttHits++;
        if (e.flag == TT_EXACT) return e.score;
        if (e.flag == TT_LOWER) alpha = std::max(alpha, e.score);
        if (e.flag == TT_UPPER) beta = std::min(beta, e.score);
        if (alpha >= beta) return e.score;
    }
    if (s.oppPieces == 0) return MATE_SCORE - (50 - d);
    // Capture chains are extended at the same depth rather than resolved by a
    // separate quiescence search -- Fanorona's forced-capture rule means the
    // chain is the tactical sequence.
    if (d <= 0) return s.inCombo ? negascout(w, s, 1, alpha, beta, ply) : evaluate(s);

    // Ply, not depth, indexes the arena: combo extensions recurse without
    // decrementing depth, so depth alone would alias two live move lists.
    if (ply >= MAX_PLY - 1) return evaluate(s);
    Move* moves = w.arena->at(ply);
    int nMoves = generate_moves(s, moves);
    if (nMoves == 0) return -MATE_SCORE + (50 - d);

    int ttM = e.hit ? e.bestMove : -1;
    // Key extraction, not a subtracting comparator: history values grow
    // unbounded and a subtraction that overflowed would break the strict weak
    // ordering the sort requires, which is undefined behaviour in C++.
    auto key = [&](const Move& m) -> int64_t {
        if (m.actionId == ttM) return std::numeric_limits<int64_t>::max();
        return static_cast<int64_t>(m.victims()) * 1000LL + w.history[(m.from + 1) * 46 + (m.to + 1)];
    };
    std::stable_sort(moves, moves + nMoves,
                     [&](const Move& a, const Move& b) { return key(a) > key(b); });

    int bestV = -INF, bestA = -1, alphaO = alpha;
    for (int i = 0; i < nMoves; i++) {
        const Move& m = moves[i];
        StepResult res = step(s, m.actionId);
        int val;
        if (res.state.player == s.player) {
            val = negascout(w, res.state, d, alpha, beta, ply + 1);  // mid-chain
        } else if (i == 0) {
            val = -negascout(w, res.state, d - 1, -beta, -alpha, ply + 1);
        } else {
            val = -negascout(w, res.state, d - 1, -alpha - 1, -alpha, ply + 1);
            if (val > alpha && val < beta) val = -negascout(w, res.state, d - 1, -beta, -alpha, ply + 1);
        }
        if (stop_.load(std::memory_order_relaxed)) return alpha;
        if (val > bestV) {
            bestV = val;
            bestA = m.actionId;
        }
        if (val > alpha) {
            alpha = val;
            if (m.from >= 0) bump_history(w, m.from + 1, m.to + 1, d * d);
        }
        if (alpha >= beta) break;
    }

    if (!stop_.load(std::memory_order_relaxed))
        tt_->store(s.zobristHash, d, bestV,
                   (bestV <= alphaO ? TT_UPPER : (bestV >= beta ? TT_LOWER : TT_EXACT)), bestA);
    return bestV;
}

int AIPlayer::search_root(Worker& w, const GameState& root, std::vector<RootMove>& moves,
                          int d, bool amWinning, int& bestMoveOut) {
    int alpha = -INF, beta = INF;
    int bestS = -INF;
    bestMoveOut = moves[0].move.actionId;
    const int64_t iterationStartNodes = w.nodes;
    w.rootNodesBest = 0;

    for (size_t i = 0; i < moves.size(); i++) {
        const Move& m = moves[i].move;
        StepResult res = step(root, m.actionId);

        bool isLoop = std::find(globalHistory_.begin(), globalHistory_.end(),
                                res.state.zobristHash) != globalHistory_.end();
        int cur;
        if (isLoop) {
            // Repeating is only bad for the side that is winning; if we are
            // behind, a repetition is a fine outcome.
            cur = amWinning ? -25000 : 0;
        } else if (res.state.player == root.player) {
            cur = negascout(w, res.state, d, alpha, beta, 1);
        } else {
            cur = -negascout(w, res.state, d - 1, -beta, -alpha, 1);
        }

        if (stop_.load(std::memory_order_relaxed)) return bestS;
        if (i == 0) w.rootNodesBest = w.nodes - iterationStartNodes;
        moves[i].score = cur;
        if (cur > bestS) {
            bestS = cur;
            bestMoveOut = m.actionId;
        }
        alpha = std::max(alpha, cur);
        if (alpha >= beta) break;
    }
    return bestS;
}

SearchResult AIPlayer::think(const GameState& root) {
    SearchResult out;
    stop_.store(false, std::memory_order_relaxed);
    deadline_ = now_ms() + config_.timeLimitMs;
    tt_->new_generation();

    Move rootBuf[MAX_MOVES];
    int nRoot = generate_moves(root, rootBuf);
    if (nRoot == 0) {
        out.bestMove = STOP_ACTION;
        out.score = -MATE_SCORE;
        return out;
    }

    int myC = std::popcount(root.myPieces), oppC = std::popcount(root.oppPieces);
    bool amWinning = myC > oppC + 1;

    auto make_root_list = [&]() {
        std::vector<RootMove> v;
        v.reserve(nRoot);
        for (int i = 0; i < nRoot; i++) v.push_back(RootMove{rootBuf[i], -INF});
        std::stable_sort(v.begin(), v.end(), [](const RootMove& a, const RootMove& b) {
            return a.move.victims() > b.move.victims();
        });
        return v;
    };

    int threads = std::max(1, config_.threads);

    // ---- helper threads: same root, staggered depths, shared table ----
    std::vector<std::thread> helpers;
    std::vector<std::unique_ptr<Worker>> helperWorkers;
    for (int t = 1; t < threads; t++) {
        auto w = std::make_unique<Worker>();
        w->id = t;
        Worker* wp = w.get();
        helperWorkers.push_back(std::move(w));
        helpers.emplace_back([this, wp, &root, amWinning, make_root_list, t] {
            auto moves = make_root_list();
            // Rotate each helper's root ordering. Without this every thread
            // searches the same move first, walks the same subtree, and mostly
            // recomputes what its neighbours are already computing. Starting
            // them on different root moves spreads the threads across the tree,
            // so the shared table fills with genuinely new information.
            if (!moves.empty())
                std::rotate(moves.begin(),
                            moves.begin() + (t % static_cast<int>(moves.size())), moves.end());
            int dummy = 0;
            // Follow the main thread's current depth rather than iterating up
            // from 1. Helpers that grind through shallow depths the main thread
            // has long since passed produce table entries nobody will read, and
            // with enough of them that churn evicts the deep entries that
            // actually matter -- which is how this scaled *negatively* past
            // four threads before. Odd-numbered helpers run one ply ahead to
            // seed the next iteration.
            while (!stop_.load(std::memory_order_relaxed)) {
                int d = mainDepth_.load(std::memory_order_relaxed) + (t & 1);
                if (d < 1) d = 1;
                if (d > config_.maxDepth) break;
                search_root(*wp, root, moves, d, amWinning, dummy);
            }
        });
    }

    // ---- main thread: drives the result ----
    Worker main;
    main.id = 0;
    auto moves = make_root_list();
    int bestMove = moves[0].move.actionId;
    int score = 0;
    int reachedDepth = 0;
    int64_t lastIterationStart = 0;
    const char* stopReason = "MaxDepth";

    for (int d = 1; d <= config_.maxDepth; d++) {
        mainDepth_.store(d, std::memory_order_relaxed);
        if (d > 1) {
            // Previous iteration's best first: the single most valuable piece
            // of move ordering there is.
            int lastBest = bestMove;
            std::stable_sort(moves.begin(), moves.end(), [&](const RootMove& a, const RootMove& b) {
                int64_t ka = a.move.actionId == lastBest ? std::numeric_limits<int64_t>::max()
                                                         : a.move.victims();
                int64_t kb = b.move.actionId == lastBest ? std::numeric_limits<int64_t>::max()
                                                         : b.move.victims();
                return ka > kb;
            });
        }
        int bm = 0;
        lastIterationStart = main.nodes;
        int s = search_root(main, root, moves, d, amWinning, bm);
        if (stop_.load(std::memory_order_relaxed)) {
            stopReason = "Time";
            break;
        }
        score = s;
        bestMove = bm;
        reachedDepth = d;
        tt_->store(root.zobristHash, d, score, TT_EXACT, bestMove);
        if (std::abs(score) > MATE_THRESHOLD) {
            stopReason = "Mate";
            break;
        }
    }

    stop_.store(true, std::memory_order_relaxed);
    for (auto& h : helpers) h.join();

    out.bestMove = bestMove;
    out.score = score;
    out.depth = reachedDepth;
    out.stopReason = stopReason;
    // What we think they will answer with: look up the position our own move
    // leads to and take whatever the table believes is best there.
    {
        StepResult after = step(root, bestMove);
        TTProbe e = tt_->probe(after.state.zobristHash);
        out.predictedReply = (e.hit && e.bestMove >= 0) ? e.bestMove : -1;
    }
    out.rootNodesBest = main.rootNodesBest;
    out.iterationNodes = main.nodes - lastIterationStart;
    // Only reconstructed when something will actually display it.
    if (config_.wantPv) out.pv = narrative_pv(root, bestMove, 24);
    out.nodes = main.nodes;
    out.ttHits = main.ttHits;
    for (auto& w : helperWorkers) {
        out.nodes += w->nodes;
        out.ttHits += w->ttHits;
    }
    return out;
}

}  // namespace fanorona
