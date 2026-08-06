// WebAssembly entry points.
//
// Exposes the same five operations the HTTP server does, as plain C functions
// taking and returning JSON strings, so the existing UI can talk to the engine
// in-process instead of over a socket. game.html is unchanged apart from which
// transport it picks.
//
// No pthreads. GitHub Pages cannot send the COOP/COEP headers that
// SharedArrayBuffer requires, and the search is perfectly capable
// single-threaded -- it was single-threaded until v1.2.0. Building without
// -pthread also keeps the module a single file with no worker bootstrap of its
// own, which is what lets the whole app ship as one HTML page.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "fanorona.h"
#include "trashtalk.h"
#include "tt.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define EXPORT extern "C" EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT extern "C"
#endif

using namespace fanorona;

namespace {

std::unique_ptr<AIPlayer> g_ai;
SearchConfig g_cfg;
bool g_english = false;
bool g_debug = false;
int g_lastScore = 0;
int g_pendingAIMove = -1;
int g_lastPredicted = -1;
std::string g_predFeedback = "Initial";

// Returned pointers stay valid until the next call. The JavaScript side copies
// the string out immediately, so one buffer per direction is enough and it
// avoids handing malloc'd pointers across the boundary for JS to free.
std::string g_out;

const char* give(std::string s) {
    g_out = std::move(s);
    return g_out.c_str();
}

// --- the same minimal JSON reading the HTTP server uses ---------------------

long long json_int(const std::string& body, const char* key, long long fallback) {
    std::string pat = "\"" + std::string(key) + "\"";
    size_t p = body.find(pat);
    if (p == std::string::npos) return fallback;
    p = body.find(':', p + pat.size());
    if (p == std::string::npos) return fallback;
    p++;
    while (p < body.size() && std::isspace((unsigned char)body[p])) p++;
    if (body.compare(p, 4, "null") == 0) return fallback;
    if (body.compare(p, 4, "true") == 0) return 1;
    if (body.compare(p, 5, "false") == 0) return 0;
    try {
        return std::stoll(body.substr(p, 24));
    } catch (...) {
        return fallback;
    }
}

std::vector<int> json_int_array(const std::string& body, const char* key) {
    std::vector<int> out;
    std::string pat = "\"" + std::string(key) + "\"";
    size_t p = body.find(pat);
    if (p == std::string::npos) return out;
    size_t open = body.find('[', p);
    size_t close = body.find(']', open);
    if (open == std::string::npos || close == std::string::npos) return out;
    std::string inner = body.substr(open + 1, close - open - 1);
    size_t i = 0;
    while (i < inner.size()) {
        size_t j = inner.find(',', i);
        if (j == std::string::npos) j = inner.size();
        try {
            out.push_back(std::stoi(inner.substr(i, j - i)));
        } catch (...) {
        }
        i = j + 1;
    }
    return out;
}

GameState state_from_json(const std::string& body) {
    std::vector<int> board = json_int_array(body, "board");
    board.resize(NUM_POS, 0);
    int player = static_cast<int>(json_int(body, "player", 1));

    GameState s;
    s.player = player;
    for (int i = 0; i < NUM_POS; i++) {
        if (board[i] == player) s.myPieces |= (1ULL << i);
        else if (board[i] != 0) s.oppPieces |= (1ULL << i);
    }
    s.inCombo = json_int(body, "inCombo", 0) != 0;
    s.comboPiece = static_cast<int>(json_int(body, "comboPiece", -1));
    s.prevPos = static_cast<int>(json_int(body, "prevPos", -1));
    for (int v : json_int_array(body, "visited")) s.visitedMask |= (1ULL << v);
    if (s.inCombo && s.prevPos != -1 && s.comboPiece != -1)
        s.lastDir = derive_last_dir(s.prevPos, s.comboPiece);
    s.init_hash();
    return s;
}

std::string json_escape(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '"') out += '\'';
        else if (c == '\n' || c == '\r') out += ' ';
        else if (c == '\\') out += '/';
        else out += c;
    }
    return out;
}

}  // namespace

EXPORT void fan_init(int timeMs, int hashMB, int english) {
    init_tables();
    zobrist::init();
    init_eval();
    g_cfg.timeLimitMs = timeMs > 0 ? timeMs : 1000;
    g_cfg.maxDepth = 1000;
    g_cfg.threads = 1;
    g_cfg.hashMB = hashMB > 0 ? hashMB : 64;
    g_english = english != 0;
    g_ai.reset(new AIPlayer(g_cfg));
}

EXPORT const char* fan_config(const char* json) {
    if (json != nullptr && *json != '\0') {
        std::string body(json);
        g_cfg.timeLimitMs = json_int(body, "time", g_cfg.timeLimitMs);
        g_cfg.maxDepth = (int)json_int(body, "depth", g_cfg.maxDepth);
        g_cfg.hashMB = (int)json_int(body, "hash", g_cfg.hashMB);
        if (g_cfg.timeLimitMs < 1) g_cfg.timeLimitMs = 1;
        if (g_cfg.maxDepth < 1) g_cfg.maxDepth = 1;
        if (g_cfg.hashMB < 1) g_cfg.hashMB = 1;
        g_english = json_int(body, "english", g_english) != 0;
        g_debug = json_int(body, "debug", g_debug) != 0;
        g_cfg.wantPv = g_debug;  // nothing reads it in trash-talk mode
        g_ai->set_config(g_cfg);
    }
    char buf[256];
    // threads is pinned at 1 and cores reported as 1 so the UI hides the
    // slider rather than offering a control that cannot do anything here.
    std::snprintf(buf, sizeof(buf),
                  "{\"time\":%lld,\"depth\":%d,\"threads\":1,\"hash\":%d,"
                  "\"english\":%s,\"debug\":%s,\"cores\":1,\"eval\":\"%s\",\"wasm\":true}",
                  (long long)g_cfg.timeLimitMs, g_cfg.maxDepth, g_cfg.hashMB,
                  g_english ? "true" : "false", g_debug ? "true" : "false",
                  eval_backend_name());
    return give(buf);
}

EXPORT const char* fan_restart() {
    g_ai->reset_game();
    g_predFeedback = "Reset";
    g_lastPredicted = -1;
    g_lastScore = 0;
    g_pendingAIMove = -1;
    return give("{\"status\":\"ok\"}");
}

EXPORT const char* fan_get_state(const char* json) {
    GameState s = state_from_json(json);
    Move buf[MAX_MOVES];
    int n = generate_moves(s, buf);
    std::string out = "{\"moves\": [";
    for (int i = 0; i < n; i++) {
        const Move& m = buf[i];
        if (i) out += ",";
        out += "{\"action_id\":" + std::to_string(m.actionId) +
               ",\"from\":" + std::to_string(m.from) +
               ",\"to\":" + std::to_string(m.to) +
               ",\"type\":\"" + to_string(m.type) + "\",\"victims\":[";
        u64 v = m.victimMask;
        bool first = true;
        while (v) {
            if (!first) out += ",";
            out += std::to_string(std::countr_zero(v));
            first = false;
            v &= (v - 1);
        }
        out += "]}";
    }
    return give(out + "]}");
}

EXPORT const char* fan_move(const char* json) {
    std::string body(json);
    GameState s = state_from_json(body);
    int action = (int)json_int(body, "action_id", STOP_ACTION);

    if (action == g_pendingAIMove) {
        g_pendingAIMove = -1;
        g_predFeedback = "Wait...";
    } else if (g_lastPredicted != -1) {
        g_predFeedback = (action == g_lastPredicted) ? "Hit🎯" : "Miss🧐";
    }
    g_ai->record_state(s.zobristHash);
    StepResult res = step(s, action);
    g_ai->record_state(res.state.zobristHash);

    std::string board = "[";
    for (int i = 0; i < NUM_POS; i++) {
        u64 bit = 1ULL << i;
        int owner = (res.state.myPieces & bit) ? res.state.player
                    : (res.state.oppPieces & bit) ? -res.state.player : 0;
        if (i) board += ",";
        board += std::to_string(owner);
    }
    board += "]";

    std::string visited = "[";
    u64 v = res.state.visitedMask;
    bool first = true;
    while (v) {
        if (!first) visited += ",";
        visited += std::to_string(std::countr_zero(v));
        first = false;
        v &= (v - 1);
    }
    visited += "]";

    return give("{\"board\":" + board + ",\"player\":" + std::to_string(res.state.player) +
                ",\"inCombo\":" + (res.state.inCombo ? "true" : "false") +
                ",\"comboPiece\":" + std::to_string(res.state.comboPiece) +
                ",\"prevPos\":" + std::to_string(res.state.prevPos) +
                ",\"win\":" + (res.win ? "true" : "false") +
                ",\"visited\":" + visited + "}");
}

EXPORT const char* fan_ai(const char* json) {
    GameState s = state_from_json(json);
    SearchResult r = g_ai->think(s);

    bool isMate = std::abs(r.score) > MATE_THRESHOLD;
    int prevScore = g_lastScore;
    g_lastScore = r.score;
    g_pendingAIMove = r.bestMove;
    g_lastPredicted = r.predictedReply;

    double knps = g_cfg.timeLimitMs > 0 ? (double)r.nodes / g_cfg.timeLimitMs : 0.0;
    char stats[1024];
    std::snprintf(stats, sizeof(stats),
                  "[%s] D:%d N:%lldk NPS:~%.0fk | Hits:%.0f%% | Root:%.0f%% | Pred:%s | %s",
                  r.stopReason, r.depth, (long long)(r.nodes / 1000), knps,
                  r.nodes > 0 ? (r.ttHits * 100.0) / r.nodes : 0.0,
                  r.iterationNodes > 0 ? (r.rootNodesBest * 100.0) / r.iterationNodes : 0.0,
                  g_predFeedback.c_str(), r.pv.c_str());

    std::string pv = g_debug ? std::string(stats)
                             : std::string(trash_talk(r.score, prevScore, isMate,
                                                      g_predFeedback, g_english));

    return give("{\"action_id\": " + std::to_string(r.bestMove) +
                ", \"score\": " + std::to_string(r.score) +
                ", \"strategy\": \"" + strategy_name(r.score) +
                "\", \"pv\": \"" + json_escape(pv) + "\"}");
}

// --- persistence -----------------------------------------------------------
//
// The browser build kept nothing between visits: every refresh threw away
// everything the engine had worked out. The desktop build has always saved its
// table, and there is no reason the web one should not -- IndexedDB holds a few
// megabytes without complaint. These hand the same blob save() writes to disk
// across to JavaScript, which stores and restores it.

namespace {
std::vector<uint8_t> g_exportBuf;
}

/** Serialises the table and returns the byte length; see fan_export_ptr. */
EXPORT int fan_export(int maxEntries) {
    if (maxEntries <= 0) maxEntries = 200000;
    g_exportBuf = g_ai->tt().export_bytes(static_cast<size_t>(maxEntries));
    return static_cast<int>(g_exportBuf.size());
}

EXPORT const uint8_t* fan_export_ptr() { return g_exportBuf.data(); }

/** @return entries restored, or -1 if the blob was not usable. */
EXPORT int fan_import(const uint8_t* data, int len) {
    if (len <= 0) return -1;
    return static_cast<int>(g_ai->tt().import_bytes(data, static_cast<size_t>(len)));
}

/**
 * Drops everything the engine has learned. The stored copy is the worker's
 * problem -- it owns IndexedDB -- so this only wipes the live table; clearing
 * one without the other would just restore it on the next load.
 */
EXPORT void fan_clear() {
    g_ai->tt().clear();
    g_exportBuf.clear();
    g_exportBuf.shrink_to_fit();
}

EXPORT const char* fan_memory_stats() {
    size_t permille = g_ai->tt().filled_permille();
    size_t approx = g_ai->tt().entries() * permille / 1000;
    return give("{\"count\": " + std::to_string(approx) + "}");
}
