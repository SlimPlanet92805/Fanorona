// Standalone Fanorona server: the C++ engine behind the same five HTTP
// endpoints the Java server exposed, so src/main/resources/game.html runs
// against it unmodified.
//
// The JSON here is hand-rolled for exactly the handful of shapes this protocol
// uses. That is not a shortcut for its own sake -- it keeps the binary
// dependency-free, which is the whole point of a release you can hand someone
// as a single file.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "fanorona.h"
#include "trashtalk.h"
#include "tt.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define INVALID_SOCKET (-1)
#define CLOSESOCK close
#endif

using namespace fanorona;

namespace {

// ------------------------------------------------------------------ config

struct Options {
    int port = 8080;
    int timeMs = 1000;
    int depth = 1000;
    int threads = 1;
    int hashMB = 256;
    bool english = false;
    bool debug = false;
    bool openBrowser = true;
    // Whether what the engine learns survives the process. Off means the table
    // is neither loaded at boot nor written on exit -- every session starts
    // from nothing, which is what you want when measuring, or when you would
    // rather the opponent not remember how it lost last time.
    bool memory = true;
    size_t saveEntries = 4'000'000;
    std::string memoryFile = "fanorona_memory.bin";
    std::string htmlFile = "game.html";
};

// ------------------------------------------------------------------ tiny JSON

// Pulls one numeric/boolean field out of a flat JSON object. The client only
// ever sends flat objects plus two integer arrays, so a full parser would be
// dead weight.
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
    std::stringstream ss(inner);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        try {
            out.push_back(std::stoi(tok));
        } catch (...) {
        }
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

// ------------------------------------------------------------------ game glue

Options g_opt;
std::unique_ptr<AIPlayer> g_ai;
std::mutex g_aiMutex;  // one search at a time; the search itself is parallel
int g_lastScore = 0;
int g_pendingAIMove = -1;
int g_lastPredicted = -1;
std::string g_predFeedback = "Initial";
std::string g_html;

std::string moves_to_json(const GameState& s) {
    Move buf[MAX_MOVES];
    int n = generate_moves(s, buf);
    std::string out = "[";
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
            int idx = std::countr_zero(v);
            if (!first) out += ",";
            out += std::to_string(idx);
            first = false;
            v &= (v - 1);
        }
        out += "]}";
    }
    return out + "]";
}

std::string board_state_json(const StepResult& res) {
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

    return "{\"board\":" + board + ",\"player\":" + std::to_string(res.state.player) +
           ",\"inCombo\":" + (res.state.inCombo ? "true" : "false") +
           ",\"comboPiece\":" + std::to_string(res.state.comboPiece) +
           ",\"prevPos\":" + std::to_string(res.state.prevPos) +
           ",\"win\":" + (res.win ? "true" : "false") +
           ",\"visited\":" + visited + "}";
}

std::string handle_ai(const std::string& body) {
    GameState s = state_from_json(body);
    std::lock_guard<std::mutex> lock(g_aiMutex);

    auto t0 = std::chrono::steady_clock::now();
    SearchResult r = g_ai->think(s);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();

    bool isMate = std::abs(r.score) > MATE_THRESHOLD;
    int prevScore = g_lastScore;
    g_lastScore = r.score;
    g_pendingAIMove = r.bestMove;
    g_lastPredicted = r.predictedReply;

    // Guard the divide: a mate found instantly takes 0 ms, and reporting
    // "NPS:0k" for the fastest search of the game reads as a bug.
    double knps = ms > 0 ? (double)r.nodes / ms : (double)r.nodes;
    double hitPct = r.nodes > 0 ? (r.ttHits * 100.0) / r.nodes : 0.0;
    double rootPct = r.iterationNodes > 0 ? (r.rootNodesBest * 100.0) / r.iterationNodes : 0.0;

    char stats[1024];
    std::snprintf(stats, sizeof(stats),
                  "[%s] D:%d N:%lldk NPS:%.0fk | Hits:%.0f%% | Root:%.0f%% | T:%d | Pred:%s | %s",
                  r.stopReason, r.depth, (long long)(r.nodes / 1000), knps, hitPct, rootPct,
                  g_opt.threads, g_predFeedback.c_str(), r.pv.c_str());
    std::printf("> Score(%d): %s\n", r.score, stats);
    std::fflush(stdout);

    std::string pv = g_opt.debug
                         ? std::string(stats)
                         : std::string(trash_talk(r.score, prevScore, isMate,
                                                  g_predFeedback, g_opt.english));

    return "{\"action_id\": " + std::to_string(r.bestMove) +
           ", \"score\": " + std::to_string(r.score) +
           ", \"strategy\": \"" + strategy_name(r.score) +
           "\", \"pv\": \"" + json_escape(pv) + "\"}";
}

// ------------------------------------------------------------------ http

struct Request {
    std::string method, path, body;
};

void send_all(socket_t c, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = ::send(c, data.data() + sent, (int)(data.size() - sent), 0);
        if (n <= 0) return;
        sent += n;
    }
}

void respond(socket_t c, const std::string& body, const char* type) {
    std::string head = "HTTP/1.1 200 OK\r\nContent-Type: ";
    head += type;
    head += "\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
    send_all(c, head + body);
}

bool read_request(socket_t c, Request& req) {
    std::string data;
    char buf[8192];
    size_t headerEnd = std::string::npos;
    while (true) {
        int n = ::recv(c, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        data.append(buf, n);
        headerEnd = data.find("\r\n\r\n");
        if (headerEnd != std::string::npos) break;
        if (data.size() > (1 << 20)) return false;
    }
    std::istringstream head(data.substr(0, headerEnd));
    std::string line;
    std::getline(head, line);
    std::istringstream first(line);
    first >> req.method >> req.path;

    size_t contentLength = 0;
    while (std::getline(head, line)) {
        if (line.size() > 15) {
            std::string lower = line;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char ch) { return (char)std::tolower(ch); });
            if (lower.rfind("content-length:", 0) == 0)
                contentLength = std::stoul(line.substr(15));
        }
    }
    req.body = data.substr(headerEnd + 4);
    while (req.body.size() < contentLength) {
        int n = ::recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.body.append(buf, n);
    }
    return true;
}

void handle_client(socket_t c) {
    Request req;
    if (read_request(c, req)) {
        if (req.path == "/" || req.path == "/index.html") {
            respond(c, g_html, "text/html; charset=utf-8");
        } else if (req.path == "/restart") {
            g_ai->reset_game();
            g_predFeedback = "Reset";
            g_lastPredicted = -1;
            g_lastScore = 0;
            respond(c, "{\"status\": \"ok\"}", "application/json");
        } else if (req.path == "/get_state") {
            GameState s = state_from_json(req.body);
            respond(c, "{\"moves\": " + moves_to_json(s) + "}", "application/json");
        } else if (req.path == "/move") {
            GameState s = state_from_json(req.body);
            int action = (int)json_int(req.body, "action_id", STOP_ACTION);
            // Prediction feedback, mirroring analyzeHumanMove in the Java version.
            if (action == g_pendingAIMove) {
                g_pendingAIMove = -1;
                g_predFeedback = "Wait...";
            } else if (g_lastPredicted != -1) {
                g_predFeedback = (action == g_lastPredicted) ? "Hit🎯" : "Miss🧐";
            }
            g_ai->record_state(s.zobristHash);
            StepResult res = step(s, action);
            g_ai->record_state(res.state.zobristHash);
            respond(c, board_state_json(res), "application/json");
        } else if (req.path == "/ai") {
            std::string json;
            try {
                json = handle_ai(req.body);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "AI error: %s\n", e.what());
                json = "{\"error\":\"AI Logic Error\"}";
            }
            respond(c, json, "application/json");
        } else if (req.path == "/config") {
            // GET reports the live settings so the UI can show real values
            // rather than its own guesses; POST applies them between moves.
            if (req.method == "POST") {
                std::lock_guard<std::mutex> lock(g_aiMutex);
                SearchConfig cfg = g_ai->config();
                cfg.timeLimitMs = json_int(req.body, "time", cfg.timeLimitMs);
                cfg.maxDepth = (int)json_int(req.body, "depth", cfg.maxDepth);
                cfg.threads = (int)json_int(req.body, "threads", cfg.threads);
                cfg.hashMB = (int)json_int(req.body, "hash", cfg.hashMB);
                if (cfg.timeLimitMs < 1) cfg.timeLimitMs = 1;
                if (cfg.maxDepth < 1) cfg.maxDepth = 1;
                if (cfg.threads < 1) cfg.threads = 1;
                if (cfg.threads > 256) cfg.threads = 256;
                if (cfg.hashMB < 1) cfg.hashMB = 1;
                g_opt.threads = cfg.threads;
                g_opt.timeMs = (int)cfg.timeLimitMs;
                g_opt.depth = cfg.maxDepth;
                g_opt.hashMB = cfg.hashMB;
                g_opt.english = json_int(req.body, "english", g_opt.english) != 0;
                g_opt.debug = json_int(req.body, "debug", g_opt.debug) != 0;
                g_opt.memory = json_int(req.body, "memory", g_opt.memory) != 0;
                cfg.wantPv = g_opt.debug;
                g_ai->set_config(cfg);
                std::printf("Config: time=%dms depth=%d threads=%d hash=%dMB lang=%s debug=%d\n",
                            g_opt.timeMs, g_opt.depth, g_opt.threads, g_opt.hashMB,
                            g_opt.english ? "en" : "zh", g_opt.debug ? 1 : 0);
                std::fflush(stdout);
            }
            const SearchConfig& cur = g_ai->config();
            char json[320];
            std::snprintf(json, sizeof(json),
                          "{\"time\":%lld,\"depth\":%d,\"threads\":%d,\"hash\":%d,"
                          "\"english\":%s,\"debug\":%s,\"memory\":%s,"
                          "\"cores\":%u,\"eval\":\"%s\"}",
                          (long long)cur.timeLimitMs, cur.maxDepth, cur.threads, cur.hashMB,
                          g_opt.english ? "true" : "false", g_opt.debug ? "true" : "false",
                          g_opt.memory ? "true" : "false",
                          std::thread::hardware_concurrency(), eval_backend_name());
            respond(c, json, "application/json");
        } else if (req.path == "/clear_memory") {
            // Wipes both copies: the live table, and the file it would otherwise
            // be written back over on exit.
            {
                std::lock_guard<std::mutex> lock(g_aiMutex);
                g_ai->tt().clear();
            }
            std::remove(g_opt.memoryFile.c_str());
            std::printf("Memory cleared\n");
            std::fflush(stdout);
            respond(c, "{\"count\": 0}", "application/json");
        } else if (req.path == "/memory_stats") {
            size_t permille = g_ai->tt().filled_permille();
            size_t approx = g_ai->tt().entries() * permille / 1000;
            respond(c, "{\"count\": " + std::to_string(approx) + "}", "application/json");
        } else {
            respond(c, "{\"error\":\"not found\"}", "application/json");
        }
    }
    CLOSESOCK(c);
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void parse_args(int argc, char** argv) {
    auto val = [](const char* a, const char* name) -> const char* {
        size_t n = std::strlen(name);
        if (std::strncmp(a, name, n) == 0 && a[n] == '=') return a + n + 1;
        return nullptr;
    };
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (const char* v = val(a, "--time")) g_opt.timeMs = std::atoi(v);
        else if (const char* v2 = val(a, "--depth")) g_opt.depth = std::atoi(v2);
        else if (const char* v3 = val(a, "--threads")) g_opt.threads = std::atoi(v3);
        else if (const char* v4 = val(a, "--hash")) g_opt.hashMB = std::atoi(v4);
        else if (const char* v5 = val(a, "--port")) g_opt.port = std::atoi(v5);
        else if (const char* v6 = val(a, "--lang")) g_opt.english = (std::strcmp(v6, "en") == 0);
        else if (const char* v7 = val(a, "--mem")) g_opt.saveEntries = (size_t)std::atoll(v7);
        else if (const char* v8 = val(a, "--memory-file")) g_opt.memoryFile = v8;
        else if (const char* v9 = val(a, "--html")) g_opt.htmlFile = v9;
        else if (std::strcmp(a, "--debug") == 0) g_opt.debug = true;
        else if (std::strcmp(a, "--no-browser") == 0) g_opt.openBrowser = false;
        else if (std::strcmp(a, "--no-memory") == 0) g_opt.memory = false;
        else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            std::printf(
                "Fanorona engine server\n\n"
                "  --time=N          thinking time per move in ms (default 1000)\n"
                "  --depth=N         maximum search depth (default unlimited)\n"
                "  --threads=N       search threads (default 1, try your core count)\n"
                "  --hash=N          transposition table size in MB (default 256)\n"
                "  --mem=N           max entries persisted to disk (default 4000000)\n"
                "  --lang=en|zh      trash-talk language (default zh)\n"
                "  --debug           show search stats in the UI instead of trash talk\n"
                "  --port=N          HTTP port (default 8080)\n"
                "  --memory-file=P   where to persist the table\n"
                "  --html=P          path to game.html\n"
                "  --no-browser      do not open a browser on startup\n");
            std::exit(0);
        }
    }
}

std::atomic<bool> g_running{true};
std::mutex g_saveMutex;

void save_memory(const char* why) {
    std::lock_guard<std::mutex> lock(g_saveMutex);
    if (!g_ai || !g_opt.memory) return;
    if (g_ai->tt().save(g_opt.memoryFile, g_opt.saveEntries))
        std::printf("Memory saved (%s)\n", why);
    std::fflush(stdout);
}

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD) {
    save_memory("shutdown");
    std::exit(0);
    return TRUE;
}
#else
void signal_handler(int) {
    save_memory("shutdown");
    std::exit(0);
}
#endif

}  // namespace

int main(int argc, char** argv) {
    parse_args(argc, argv);
    init_tables();
    zobrist::init();
    init_eval();

    g_html = read_file(g_opt.htmlFile);
    if (g_html.empty()) {
        // Also look next to the executable and in the source tree, so running
        // from a build directory works without extra flags.
        for (const char* alt : {"../src/main/resources/game.html",
                                "src/main/resources/game.html",
                                "../../src/main/resources/game.html"}) {
            g_html = read_file(alt);
            if (!g_html.empty()) break;
        }
    }
    if (g_html.empty()) {
        std::fprintf(stderr, "Cannot find %s -- pass --html=<path>\n", g_opt.htmlFile.c_str());
        return 1;
    }

    SearchConfig cfg;
    cfg.timeLimitMs = g_opt.timeMs;
    cfg.maxDepth = g_opt.depth;
    cfg.threads = g_opt.threads;
    cfg.hashMB = g_opt.hashMB;
    cfg.wantPv = g_opt.debug;  // nothing reads it in trash-talk mode
    g_ai.reset(new AIPlayer(cfg));

    std::printf("--- Fanorona Engine (C++) ---\n");
    std::printf("time=%dms depth=%d threads=%d hash=%dMB eval=%s\n",
                g_opt.timeMs, g_opt.depth, g_opt.threads, g_opt.hashMB, eval_backend_name());

    if (g_opt.memory) {
        long long restored = g_ai->tt().load(g_opt.memoryFile);
        if (restored > 0) std::printf("Memory restored: %lld entries\n", restored);
    }

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    socket_t srv = ::socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)g_opt.port);
    if (::bind(srv, (sockaddr*)&addr, sizeof(addr)) != 0 || ::listen(srv, 32) != 0) {
        std::fprintf(stderr, "Port %d occupied. Close the previous process.\n", g_opt.port);
        return 1;
    }

    std::string url = "http://localhost:" + std::to_string(g_opt.port);
    std::printf("Server started at: %s\n", url.c_str());
    std::printf("Press Ctrl+C to quit (the table is saved on exit).\n");
    if (g_opt.openBrowser) {
#ifdef _WIN32
        std::string cmd = "start \"\" \"" + url + "\"";
        std::system(cmd.c_str());
#else
        std::system(("xdg-open " + url + " >/dev/null 2>&1 &").c_str());
#endif
    }

    // Ctrl+C is the documented way to quit, so it has to be the path that
    // saves. A periodic save backs that up: if the process is killed outright
    // -- closing the console window, a crash -- at most a minute of learning
    // is lost instead of the whole session.
#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#endif
    std::thread([] {
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            if (g_running.load()) save_memory("periodic");
        }
    }).detach();

    while (g_running.load()) {
        socket_t c = ::accept(srv, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        // One thread per connection: the browser makes a handful of short
        // requests, and the expensive work is inside the search's own pool.
        std::thread(handle_client, c).detach();
    }
    CLOSESOCK(srv);
    return 0;
}
