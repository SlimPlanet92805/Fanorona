// Mirror of Bench.java. Output on stdout must be byte-identical to the Java
// side's, so the parity check is a plain diff against bench/golden.txt.
#include "fanorona.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <utility>
#include <string>
#include <vector>

using namespace fanorona;

namespace {

constexpr int DEFAULT_DEPTH = 8;
constexpr const char* DEFAULT_POSITIONS = "bench/positions.txt";

std::vector<std::string> read_positions(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "Cannot read %s (generate it with the Java --bench-gen)\n", path.c_str());
        std::exit(2);
    }
    std::vector<std::string> out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        size_t e = line.find_last_not_of(" \t");
        std::string t = line.substr(b, e - b + 1);
        if (t.empty() || t[0] == '#') continue;
        out.push_back(t);
    }
    return out;
}

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

const char* arg_value(int argc, char** argv, const char* name) {
    size_t n = std::strlen(name);
    for (int i = 1; i < argc; i++) {
        if (std::strncmp(argv[i], name, n) == 0) {
            if (argv[i][n] == '=') return argv[i] + n + 1;
            if (argv[i][n] == '\0') return "";
        }
    }
    return nullptr;
}

// Counts leaf nodes of the pure move-generation tree: no transposition table,
// no evaluation, no ordering, only the rules. This is the correctness check
// that survives every search optimisation -- a faster table or a parallel
// search is meant to change search node counts, but must never change perft.
int64_t perft(const GameState& s, int depth) {
    std::vector<Move> moves = get_detailed_moves(s);
    if (depth <= 1) return static_cast<int64_t>(moves.size());
    int64_t n = 0;
    for (const Move& m : moves) n += perft(step(s, m.actionId).state, depth - 1);
    return n;
}

void run_perft(const std::string& path, int maxDepth) {
    auto positions = read_positions(path);
    std::printf("# perft maxDepth=%d positions=%zu\n", maxDepth, positions.size());
    for (size_t i = 0; i < positions.size(); i++) {
        GameState s = from_text(positions[i]);
        std::printf("%zu", i);
        for (int d = 1; d <= maxDepth; d++) std::printf(" %" PRId64, perft(s, d));
        std::printf("\n");
    }
}

// Plays two engine configurations against each other.
//
// Needed because "this evaluation change should make it stronger" is a
// hypothesis, not a result. Node counts and nanoseconds cannot answer it --
// only games can. Openings are randomised and colours alternate so neither
// side is judged on a single line of play.
struct MatchSide {
    int huntCap;   // eval_hunt_cap for this side; negative = legacy un-normalised sum
    const char* name;
};

struct GameOutcome {
    int winner = 0;      // +1 white, -1 black, 0 unresolved
    int materialWhite = 0, materialBlack = 0;
};

GameOutcome play_game(const MatchSide& white, const MatchSide& black, int movetimeMs,
                      uint64_t seed, int openingPlies, int plyCap) {
    GameState s = from_text(
        "wwwwwwwwwwwwwwwwww" "wbwb.wbwb" "bbbbbbbbbbbbbbbbbb" " 1 0 -1 -1 0");

    SearchConfig cfg;
    cfg.timeLimitMs = movetimeMs;
    cfg.maxDepth = 1000;
    cfg.threads = 1;
    cfg.hashMB = 32;
    AIPlayer aiW(cfg), aiB(cfg);

    std::mt19937_64 rng(seed);
    Move buf[MAX_MOVES];

    auto material = [](const GameState& g) {
        // Bitboards are side-to-move relative; report them as white/black.
        int mine = std::popcount(g.myPieces), theirs = std::popcount(g.oppPieces);
        return g.player == 1 ? std::pair<int, int>{mine, theirs}
                             : std::pair<int, int>{theirs, mine};
    };

    for (int ply = 0; ply < plyCap; ply++) {
        int n = generate_moves(s, buf);
        if (n == 0) {  // side to move has no reply: it loses
            auto [w, b] = material(s);
            return {-s.player, w, b};
        }

        int action;
        if (ply < openingPlies) {
            action = buf[rng() % n].actionId;      // randomised opening
        } else {
            const MatchSide& side = (s.player == 1) ? white : black;
            eval_hunt_cap = side.huntCap;          // the setting under test
            AIPlayer& ai = (s.player == 1) ? aiW : aiB;
            action = ai.think(s).bestMove;
            bool legal = false;
            for (int i = 0; i < n; i++) if (buf[i].actionId == action) legal = true;
            if (!legal) action = buf[0].actionId;
        }
        StepResult r = step(s, action);
        if (r.win) {
            auto [w, b] = material(r.state);
            return {r.state.player, w, b};         // mover wiped the opponent out
        }
        s = r.state;
    }
    // Fanorona draws a lot, and at these time controls most games hit the cap.
    // Reporting them all as "draw" throws away the information that one side
    // was up eight pieces, so unresolved games are adjudicated on material.
    auto [w, b] = material(s);
    return {0, w, b};
}

void run_match(int games, int movetimeMs, int huntA, int huntB, int openingPlies, int plyCap) {
    MatchSide A{huntA, "A"}, B{huntB, "B"};
    int winA = 0, winB = 0, draw = 0, adjA = 0, adjB = 0;
    long long marginSum = 0;
    std::printf("# match: A(huntcap=%d) vs B(huntcap=%d), %d games, %dms/move, "
                "%d random opening plies, %d ply cap\n",
                huntA, huntB, games, movetimeMs, openingPlies, plyCap);
    for (int g = 0; g < games; g++) {
        // Same seed for the pair with colours swapped, so both configurations
        // play the identical opening from both sides and an easy start cannot
        // flatter whichever engine happened to draw it.
        bool aIsWhite = (g % 2) == 0;
        uint64_t seed = 0x9E3779B97F4A7C15ULL * (g / 2 + 1);
        GameOutcome o = aIsWhite ? play_game(A, B, movetimeMs, seed, openingPlies, plyCap)
                                 : play_game(B, A, movetimeMs, seed, openingPlies, plyCap);
        int aWinner = aIsWhite ? o.winner : -o.winner;
        int aMargin = aIsWhite ? (o.materialWhite - o.materialBlack)
                               : (o.materialBlack - o.materialWhite);
        marginSum += aMargin;
        const char* label;
        if (aWinner > 0)      { winA++; label = "A wins"; }
        else if (aWinner < 0) { winB++; label = "B wins"; }
        else if (aMargin > 0) { adjA++; draw++; label = "cap, A ahead"; }
        else if (aMargin < 0) { adjB++; draw++; label = "cap, B ahead"; }
        else                  { draw++;        label = "cap, level"; }
        std::printf("  game %2d: %-13s (material %+d)\n", g + 1, label, aMargin);
        std::fflush(stdout);
    }
    double pct = games ? (winA + 0.5 * draw) * 100.0 / games : 0.0;
    std::printf("# decisive: A %d, B %d | capped: %d (A ahead %d, B ahead %d)\n",
                winA, winB, draw, adjA, adjB);
    std::printf("# A scores %.1f%% | mean material margin %+.2f\n",
                pct, games ? (double)marginSum / games : 0.0);
}

void dump(const std::string& what, const std::string& path) {
    if (what == "zobrist") {
        for (int i = 0; i < NUM_POS; i++) {
            std::printf("P %d 0 %016llx\n", i, (unsigned long long)zobrist::P[i][0]);
            std::printf("P %d 1 %016llx\n", i, (unsigned long long)zobrist::P[i][1]);
        }
        std::printf("T %016llx\n", (unsigned long long)zobrist::T);
        return;
    }
    auto positions = read_positions(path);
    if (what == "codec") {
        int bad = 0;
        for (const auto& line : positions) {
            GameState s = from_text(line);
            std::string back = to_text(s);
            if (back != line) {
                bad++;
                std::printf("MISMATCH\n  in : %s\n  out: %s\n", line.c_str(), back.c_str());
            }
        }
        std::printf(bad == 0 ? "# codec round-trip OK\n" : "# %d MISMATCHES\n", bad);
    } else if (what == "movegen") {
        for (const auto& line : positions) {
            GameState s = from_text(line);
            std::printf("pos %016llx\n", (unsigned long long)s.zobristHash);
            for (const Move& m : get_detailed_moves(s))
                std::printf("  move id=%d from=%d to=%d type=%s victims=%d\n",
                            m.actionId, m.from, m.to, to_string(m.type), m.victims());
        }
    } else if (what == "step") {
        for (const auto& line : positions) {
            GameState s = from_text(line);
            std::printf("pos %016llx\n", (unsigned long long)s.zobristHash);
            for (const Move& m : get_detailed_moves(s)) {
                StepResult r = step(s, m.actionId);
                std::printf("  %d -> %s win=%s hash=%016llx lastDir=%d\n",
                            m.actionId, to_text(r.state).c_str(), r.win ? "true" : "false",
                            (unsigned long long)r.state.zobristHash, r.state.lastDir);
            }
        }
    } else if (what == "eval") {
        for (const auto& line : positions) {
            GameState s = from_text(line);
            std::printf("%016llx %d\n", (unsigned long long)s.zobristHash, evaluate(s));
            for (const Move& m : get_detailed_moves(s)) {
                GameState c = step(s, m.actionId).state;
                std::printf("  %d %016llx %d\n", m.actionId,
                            (unsigned long long)c.zobristHash, evaluate(c));
            }
        }
    } else {
        std::fprintf(stderr, "unknown dump '%s'; expected zobrist|codec|movegen|step|eval\n",
                     what.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    init_tables();
    zobrist::init();
    bool forceScalar = false;
    if (const char* e = arg_value(argc, argv, "--eval"); e && std::strcmp(e, "scalar") == 0) forceScalar = true;
    init_eval(forceScalar);
    if (const char* h = arg_value(argc, argv, "--hunt-cap"); h && *h) eval_hunt_cap = std::atoi(h);

    std::string path = DEFAULT_POSITIONS;
    if (const char* p = arg_value(argc, argv, "--positions"); p && *p) path = p;

    if (const char* d = arg_value(argc, argv, "--bench-dump"); d && *d) {
        dump(d, path);
        return 0;
    }
    if (const char* d = arg_value(argc, argv, "--bench-perft"); d) {
        run_perft(path, (*d) ? std::atoi(d) : 4);
        return 0;
    }
    if (arg_value(argc, argv, "--match")) {
        auto num = [&](const char* n, int def) {
            const char* v = arg_value(argc, argv, n);
            return (v && *v) ? std::atoi(v) : def;
        };
        run_match(num("--games", 20), num("--movetime", 100), num("--huntA", 50),
                  num("--huntB", -1), num("--opening", 6), num("--plycap", 200));
        return 0;
    }

    int depth = DEFAULT_DEPTH;
    if (const char* d = arg_value(argc, argv, "--depth"); d && *d) depth = std::atoi(d);
    int threads = 1;
    if (const char* t = arg_value(argc, argv, "--threads"); t && *t) threads = std::atoi(t);
    int hashMB = 256;
    if (const char* h = arg_value(argc, argv, "--hash"); h && *h) hashMB = std::atoi(h);

    // Fixed time instead of fixed depth: this is the comparison that maps onto
    // play strength, since what matters in a game is how deep an engine gets
    // within the move budget it is given.
    int movetime = 0;
    if (const char* m = arg_value(argc, argv, "--movetime"); m && *m) movetime = std::atoi(m);

    auto positions = read_positions(path);
    if (movetime > 0)
        std::printf("# fanorona bench movetime=%dms positions=%zu\n", movetime, positions.size());
    else
        std::printf("# fanorona bench depth=%d positions=%zu\n", depth, positions.size());
    if (threads != 1 || hashMB != 256 || movetime > 0)
        std::fprintf(stderr, "# threads=%d hash=%dMB eval=%s\n", threads, hashMB, eval_backend_name());

    int64_t totalNodes = 0, totalHits = 0, totalMs = 0, totalDepth = 0;
    for (size_t i = 0; i < positions.size(); i++) {
        GameState s = from_text(positions[i]);

        SearchConfig cfg;
        cfg.maxDepth = movetime > 0 ? 1000 : depth;
        cfg.timeLimitMs = movetime > 0 ? movetime : std::numeric_limits<int32_t>::max();
        cfg.threads = threads;
        cfg.hashMB = hashMB;
        AIPlayer ai(cfg);

        int64_t t0 = now_ms();
        SearchResult r = ai.think(s);
        totalMs += now_ms() - t0;

        if (movetime > 0)
            std::printf("%zu depth=%d nodes=%" PRId64 " score=%d bestMove=%d\n",
                        i, r.depth, r.nodes, r.score, r.bestMove);
        else
            std::printf("%zu nodes=%" PRId64 " ttHits=%" PRId64 " score=%d bestMove=%d\n",
                        i, r.nodes, r.ttHits, r.score, r.bestMove);
        totalNodes += r.nodes;
        totalHits += r.ttHits;
        totalDepth += r.depth;
    }
    if (movetime > 0)
        std::printf("# total depth=%" PRId64 " nodes=%" PRId64 "\n", totalDepth, totalNodes);
    else
        std::printf("# total nodes=%" PRId64 " ttHits=%" PRId64 "\n", totalNodes, totalHits);

    // Timing to stderr so stdout stays diffable against the golden file.
    std::fprintf(stderr, "# wall %" PRId64 " ms, %.0f knps\n", totalMs,
                 totalMs > 0 ? totalNodes / (double)totalMs : 0.0);
    return 0;
}
