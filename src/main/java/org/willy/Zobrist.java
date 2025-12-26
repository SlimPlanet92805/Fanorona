package org.willy;

import java.util.Random;

class Zobrist {
    static long[][] P = new long[GameLogic.NUM_POS][2];
    static long T;

    static void init() {
        Random r = new Random(12345);
        for (int i = 0; i < GameLogic.NUM_POS; i++) {
            P[i][0] = r.nextLong();
            P[i][1] = r.nextLong();
        }
        T = r.nextLong();
    }

    static long compute(GameState s) {
        long h = 0, m = s.myPieces, o = s.oppPieces;
        while (m != 0) {
            h ^= P[Long.numberOfTrailingZeros(m)][0];
            m &= (m - 1);
        }
        while (o != 0) {
            h ^= P[Long.numberOfTrailingZeros(o)][1];
            o &= (o - 1);
        }
        if (s.player == -1) h ^= T;
        return h;
    }
}
