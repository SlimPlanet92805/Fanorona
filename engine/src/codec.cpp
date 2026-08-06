// Port of the text half of GameStateCodec.java. The JSON half stays in Java;
// only the benchmark format has to exist on both sides.
#include "fanorona.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace fanorona {

namespace {
constexpr char EMPTY = '.', WHITE = 'w', BLACK = 'b';
}

GameState from_text(const std::string& line) {
    std::istringstream in(line);
    std::string board;
    int player, inCombo, comboPiece, prevPos;
    std::string visitedHex;
    if (!(in >> board >> player >> inCombo >> comboPiece >> prevPos >> visitedHex))
        throw std::runtime_error("malformed position line: " + line);
    if (board.size() != static_cast<size_t>(NUM_POS))
        throw std::runtime_error("board must be 45 chars: " + board);

    GameState s;
    s.player = player;
    for (int i = 0; i < NUM_POS; i++) {
        char c = board[i];
        if (c == EMPTY) continue;
        int owner = (c == WHITE) ? 1 : -1;
        // Bitboards are side-to-move relative; the text form is absolute.
        if (owner == player) s.myPieces |= (1ULL << i);
        else s.oppPieces |= (1ULL << i);
    }
    s.inCombo = (inCombo != 0);
    s.comboPiece = comboPiece;
    s.prevPos = prevPos;
    s.visitedMask = std::strtoull(visitedHex.c_str(), nullptr, 16);
    // lastDir is derived, never transmitted -- both sides must derive it the
    // same way or the no-same-direction chain rule diverges.
    if (s.inCombo && s.prevPos != -1 && s.comboPiece != -1)
        s.lastDir = derive_last_dir(s.prevPos, s.comboPiece);
    s.init_hash();
    return s;
}

std::string to_text(const GameState& s) {
    std::string board(NUM_POS, EMPTY);
    for (int i = 0; i < NUM_POS; i++) {
        u64 bit = 1ULL << i;
        int owner = (s.myPieces & bit) != 0 ? s.player : ((s.oppPieces & bit) != 0 ? -s.player : 0);
        board[i] = owner == 0 ? EMPTY : (owner == 1 ? WHITE : BLACK);
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), " %d %d %d %d %llx", s.player, s.inCombo ? 1 : 0,
                  s.comboPiece, s.prevPos, static_cast<unsigned long long>(s.visitedMask));
    return board + buf;
}

}  // namespace fanorona
