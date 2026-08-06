// Adjacency tables, move generation, and step().
#include "fanorona.h"

#include <bit>

namespace fanorona {

int ADJ_INDEX[NUM_POS][8];
int POS_R[NUM_POS];
int POS_C[NUM_POS];
char POS_NAMES[NUM_POS][4];

const int OPPOSITE_DIR[8] = {4, 5, 6, 7, 0, 1, 2, 3};
static const int DR[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
static const int DC[8] = {0, 1, 1, 1, 0, -1, -1, -1};

const int POS_VAL[NUM_POS] = {
    1, 2, 1, 2, 1, 2, 1, 2, 1,
    2, 4, 3, 4, 3, 4, 3, 4, 2,
    1, 3, 8, 6, 9, 6, 8, 3, 1,
    2, 4, 3, 4, 3, 4, 3, 4, 2,
    1, 2, 1, 2, 1, 2, 1, 2, 1};

void init_tables() {
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int curr = r * COLS + c;
            POS_R[curr] = r;
            POS_C[curr] = c;
            POS_NAMES[curr][0] = static_cast<char>('a' + c);
            POS_NAMES[curr][1] = static_cast<char>('0' + (ROWS - r));
            POS_NAMES[curr][2] = '\0';
            // "Strong" intersections carry all eight lines; weak ones only the
            // four orthogonals. That is what makes this a Fanorona board rather
            // than a plain grid.
            bool isStrong = (r + c) % 2 == 0;
            for (int d = 0; d < 8; d++) {
                ADJ_INDEX[curr][d] = -1;
                if (!isStrong && (d % 2 != 0)) continue;
                int nr = r + DR[d], nc = c + DC[d];
                if (nr >= 0 && nr < ROWS && nc >= 0 && nc < COLS) ADJ_INDEX[curr][d] = nr * COLS + nc;
            }
        }
    }
}

int derive_last_dir(int prevPos, int comboPiece) {
    for (int d = 0; d < 8; d++)
        if (ADJ_INDEX[prevPos][d] == comboPiece) return d;
    return -1;
}

void GameState::init_hash() {
    hA = 0;
    hB = 0;
    u64 m = myPieces, o = oppPieces;
    while (m != 0) {
        int i = std::countr_zero(m);
        hA ^= zobrist::P[i][0];
        hB ^= zobrist::P[i][1];
        m &= (m - 1);
    }
    while (o != 0) {
        int i = std::countr_zero(o);
        hA ^= zobrist::P[i][1];
        hB ^= zobrist::P[i][0];
        o &= (o - 1);
    }
    zobristHash = hA ^ (player == -1 ? zobrist::T : 0);
}

const char* to_string(MoveType t) {
    switch (t) {
        case MoveType::Approach: return "approach";
        case MoveType::Withdrawal: return "withdrawal";
        case MoveType::Move: return "move";
        case MoveType::Stop: return "stop";
    }
    return "?";
}

// The unbroken run of enemy pieces along one line from `start` in direction `d`.
static inline u64 trace(u64 enemy, int start, int d) {
    u64 mask = 0;
    int c = ADJ_INDEX[start][d];
    while (c != -1 && (enemy & (1ULL << c)) != 0) {
        mask |= (1ULL << c);
        c = ADJ_INDEX[c][d];
    }
    return mask;
}

bool has_capture_moves(const GameState& s, int p) {
    u64 occ = s.myPieces | s.oppPieces;
    for (int d = 0; d < 8; d++) {
        if (s.inCombo && d == s.lastDir) continue;
        int t = ADJ_INDEX[p][d];
        if (t == -1 || (occ & (1ULL << t)) != 0 || (s.inCombo && (s.visitedMask & (1ULL << t)) != 0)) continue;
        int av = ADJ_INDEX[t][d];
        if (av != -1 && (s.oppPieces & (1ULL << av)) != 0) return true;
        int wv = ADJ_INDEX[p][OPPOSITE_DIR[d]];
        if (wv != -1 && (s.oppPieces & (1ULL << wv)) != 0) return true;
    }
    return false;
}

static inline bool gen_for_piece(const GameState& s, int p, Move* out, int& n, bool onlyC) {
    bool f = false;
    u64 occ = s.myPieces | s.oppPieces;
    for (int d = 0; d < 8; d++) {
        if (s.inCombo && d == s.lastDir) continue;
        int t = ADJ_INDEX[p][d];
        if (t == -1 || (occ & (1ULL << t)) != 0 || (s.inCombo && (s.visitedMask & (1ULL << t)) != 0)) continue;
        int av = ADJ_INDEX[t][d];
        bool isA = (av != -1 && (s.oppPieces & (1ULL << av)) != 0);
        int wv = ADJ_INDEX[p][OPPOSITE_DIR[d]];
        bool isW = (wv != -1 && (s.oppPieces & (1ULL << wv)) != 0);
        if (isA) {
            Move& m = out[n++];
            m.actionId = static_cast<int16_t>(p * 8 + d);
            m.from = static_cast<int8_t>(p);
            m.to = static_cast<int8_t>(t);
            m.type = MoveType::Approach;
            m.victimMask = trace(s.oppPieces, t, d);
            f = true;
        }
        if (isW) {
            Move& m = out[n++];
            m.actionId = static_cast<int16_t>(p * 8 + d + 360);
            m.from = static_cast<int8_t>(p);
            m.to = static_cast<int8_t>(t);
            m.type = MoveType::Withdrawal;
            m.victimMask = trace(s.oppPieces, p, OPPOSITE_DIR[d]);
            f = true;
        }
        if (!onlyC && !isA && !isW) {
            Move& m = out[n++];
            m.actionId = static_cast<int16_t>(p * 8 + d);
            m.from = static_cast<int8_t>(p);
            m.to = static_cast<int8_t>(t);
            m.type = MoveType::Move;
            m.victimMask = 0;
            f = true;
        }
    }
    return f;
}

int generate_moves(const GameState& s, Move* out) {
    int n = 0;
    if (s.inCombo) {
        gen_for_piece(s, s.comboPiece, out, n, true);
        Move& stop = out[n++];
        stop.actionId = STOP_ACTION;
        stop.from = -1;
        stop.to = -1;
        stop.type = MoveType::Stop;
        stop.victimMask = 0;
        return n;
    }
    // Captures are compulsory: scan every piece for one first, and only fall
    // back to quiet moves if the whole board has none.
    u64 temp = s.myPieces;
    bool canC = false;
    while (temp != 0) {
        if (gen_for_piece(s, std::countr_zero(temp), out, n, true)) canC = true;
        temp &= (temp - 1);
    }
    if (canC) return n;
    // n is necessarily 0 here: with onlyC set, the pass above appends nothing
    // unless it found a capture. Kept as an append, matching the reference.
    temp = s.myPieces;
    while (temp != 0) {
        gen_for_piece(s, std::countr_zero(temp), out, n, false);
        temp &= (temp - 1);
    }
    return n;
}

std::vector<Move> get_detailed_moves(const GameState& s) {
    Move buf[MAX_MOVES];
    int n = generate_moves(s, buf);
    return std::vector<Move>(buf, buf + n);
}

static inline void end_turn(GameState& s) {
    // Bitboards are side-to-move relative, so ending a turn is a swap -- and
    // because both hashes are maintained, so is the hash.
    u64 temp = s.myPieces;
    s.myPieces = s.oppPieces;
    s.oppPieces = temp;
    temp = s.hA;
    s.hA = s.hB;
    s.hB = temp;
    s.player = -s.player;
    s.inCombo = false;
    s.comboPiece = -1;
    s.prevPos = -1;
    s.lastDir = -1;
    s.visitedMask = 0;
}

StepResult step(const GameState& state, int action) {
    StepResult out;
    GameState& next = out.state;
    next = state;

    if (action == STOP_ACTION) {
        end_turn(next);
        next.zobristHash = next.hA ^ (next.player == -1 ? zobrist::T : 0);
        return out;
    }

    bool isWd = action >= 360;
    int norm = isWd ? action - 360 : action;
    int fIdx = norm / 8, dIdx = norm % 8, tIdx = ADJ_INDEX[fIdx][dIdx];

    next.myPieces &= ~(1ULL << fIdx);
    next.myPieces |= (1ULL << tIdx);
    // Incremental: the moving piece leaves fIdx and arrives at tIdx.
    next.hA ^= zobrist::P[fIdx][0] ^ zobrist::P[tIdx][0];
    next.hB ^= zobrist::P[fIdx][1] ^ zobrist::P[tIdx][1];

    u64 victims;
    if (isWd) {
        victims = trace(state.oppPieces, fIdx, OPPOSITE_DIR[dIdx]);
    } else {
        int av = ADJ_INDEX[tIdx][dIdx];
        victims = (av != -1 && (state.oppPieces & (1ULL << av)) != 0)
                      ? trace(state.oppPieces, tIdx, dIdx)
                      : 0;
    }
    if (victims != 0) {
        next.oppPieces &= ~victims;
        u64 v = victims;
        while (v != 0) {
            int i = std::countr_zero(v);
            next.hA ^= zobrist::P[i][1];
            next.hB ^= zobrist::P[i][0];
            v &= (v - 1);
        }
    }

    if (!next.inCombo) next.visitedMask = (1ULL << fIdx);
    next.visitedMask |= (1ULL << tIdx);

    if (victims != 0) {
        if (next.oppPieces == 0) {
            next.zobristHash = next.hA ^ (next.player == -1 ? zobrist::T : 0);
            out.win = true;
            return out;
        }
        next.prevPos = fIdx;
        next.comboPiece = tIdx;
        next.inCombo = true;
        next.lastDir = dIdx;
        // A chain continues only while the same piece can keep capturing.
        if (!has_capture_moves(next, tIdx)) end_turn(next);
    } else {
        end_turn(next);
    }
    next.zobristHash = next.hA ^ (next.player == -1 ? zobrist::T : 0);
    return out;
}

}  // namespace fanorona
