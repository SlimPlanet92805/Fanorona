package org.willy;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

class GameLogic {
    static final int NUM_POS = 45;
    private static final int[][] ADJ_INDEX = new int[NUM_POS][8];
    private static final int[] OPPOSITE_DIR = {4, 5, 6, 7, 0, 1, 2, 3};
    private static final int[] DR = {-1, -1, 0, 1, 1, 1, 0, -1};
    private static final int[] DC = {0, 1, 1, 1, 0, -1, -1, -1};
    static final String[] POS_NAMES = new String[45];
    static final int[] POS_R = new int[45];
    static final int[] POS_C = new int[45];
    static final int[] POS_VAL = {
            1, 2, 1, 2, 1, 2, 1, 2, 1,
            2, 4, 3, 4, 3, 4, 3, 4, 2,
            1, 3, 8, 6, 9, 6, 8, 3, 1,
            2, 4, 3, 4, 3, 4, 3, 4, 2,
            1, 2, 1, 2, 1, 2, 1, 2, 1
    };

    public static List<Move> getDetailedMoves(GameState s) {
        List<Move> moves = new ArrayList<>(32);
        if (s.inCombo) {
            generateMovesForPiece(s, s.comboPiece, moves, true);
            moves.add(new Move(720, -1, -1, "stop", Collections.emptyList()));
            return moves;
        }
        long temp = s.myPieces;
        boolean canC = false;
        while (temp != 0) {
            int p = Long.numberOfTrailingZeros(temp);
            if (generateMovesForPiece(s, p, moves, true)) canC = true;
            temp &= (temp - 1);
        }
        if (canC) return moves;
        temp = s.myPieces;
        while (temp != 0) {
            int p = Long.numberOfTrailingZeros(temp);
            generateMovesForPiece(s, p, moves, false);
            temp &= (temp - 1);
        }
        return moves;
    }

    public static boolean hasCaptureMoves(GameState s, int p) {
        long occ = s.myPieces | s.oppPieces;
        for (int d = 0; d < 8; d++) {
            if (s.inCombo && d == s.lastDir) continue;
            int t = ADJ_INDEX[p][d];
            if (t == -1 || (occ & (1L << t)) != 0 || (s.inCombo && (s.visitedMask & (1L << t)) != 0)) continue;
            int av = ADJ_INDEX[t][d];
            if (av != -1 && (s.oppPieces & (1L << av)) != 0) return true;
            int wv = ADJ_INDEX[p][OPPOSITE_DIR[d]];
            if (wv != -1 && (s.oppPieces & (1L << wv)) != 0) return true;
        }
        return false;
    }

    private static boolean generateMovesForPiece(GameState s, int p, List<Move> list, boolean onlyC) {
        boolean f = false;
        long occ = s.myPieces | s.oppPieces;
        for (int d = 0; d < 8; d++) {
            if (s.inCombo && d == s.lastDir) continue;
            int t = ADJ_INDEX[p][d];
            if (t == -1 || (occ & (1L << t)) != 0 || (s.inCombo && (s.visitedMask & (1L << t)) != 0)) continue;
            int av = ADJ_INDEX[t][d];
            boolean isA = (av != -1 && (s.oppPieces & (1L << av)) != 0);
            int wv = ADJ_INDEX[p][OPPOSITE_DIR[d]];
            boolean isW = (wv != -1 && (s.oppPieces & (1L << wv)) != 0);
            if (isA) {
                list.add(new Move(p * 8 + d, p, t, "approach", trace(s.oppPieces, t, d)));
                f = true;
            }
            if (isW) {
                list.add(new Move(p * 8 + d + 360, p, t, "withdrawal", trace(s.oppPieces, p, OPPOSITE_DIR[d])));
                f = true;
            }
            if (!onlyC && !isA && !isW) {
                list.add(new Move(p * 8 + d, p, t, "move", Collections.emptyList()));
                f = true;
            }
        }
        return f;
    }

    private static List<Integer> trace(long enemy, int start, int d) {
        List<Integer> v = new ArrayList<>();
        int c = ADJ_INDEX[start][d];
        while (c != -1 && (enemy & (1L << c)) != 0) {
            v.add(c);
            c = ADJ_INDEX[c][d];
        }
        return v;
    }

    public static StepResult step(GameState state, int action) {
        GameState next = state.copy();
        if (action == 720) {
            endTurn(next);
            next.initHash();
            return new StepResult(next, false);
        }
        boolean isWd = action >= 360;
        int norm = isWd ? action - 360 : action;
        int fIdx = norm / 8, dIdx = norm % 8, tIdx = ADJ_INDEX[fIdx][dIdx];
        next.myPieces &= ~(1L << fIdx);
        next.myPieces |= (1L << tIdx);
        List<Integer> victims = isWd ? trace(state.oppPieces, fIdx, OPPOSITE_DIR[dIdx]) :
                ((ADJ_INDEX[tIdx][dIdx] != -1 && (state.oppPieces & (1L << ADJ_INDEX[tIdx][dIdx])) != 0) ? trace(state.oppPieces, tIdx, dIdx) : Collections.emptyList());
        for (int v : victims) next.oppPieces &= ~(1L << v);
        if (!next.inCombo) next.visitedMask = (1L << fIdx);
        next.visitedMask |= (1L << tIdx);
        if (!victims.isEmpty()) {
            if (next.oppPieces == 0) {
                next.initHash();
                return new StepResult(next, true);
            }
            next.prevPos = fIdx;
            next.comboPiece = tIdx;
            next.inCombo = true;
            next.lastDir = dIdx;
            if (!hasCaptureMoves(next, tIdx)) endTurn(next);
        } else endTurn(next);
        next.initHash();
        return new StepResult(next, false);
    }

    private static void endTurn(GameState s) {
        long temp = s.myPieces;
        s.myPieces = s.oppPieces;
        s.oppPieces = temp;
        s.player *= -1;
        s.inCombo = false;
        s.comboPiece = -1;
        s.prevPos = -1;
        s.lastDir = -1;
        s.visitedMask = 0;
    }

    public static GameState fromJson(JsonUtil.JsonObject j) {
        GameState s = new GameState();
        int[] arr = j.getIntArray("board");
        s.player = j.getInt("player");
        for (int i = 0; i < NUM_POS; i++) {
            if (arr[i] == s.player) s.myPieces |= (1L << i);
            else if (arr[i] != 0) s.oppPieces |= (1L << i);
        }
        s.inCombo = j.getBoolean("inCombo");
        s.comboPiece = j.has("comboPiece") && !j.isNull("comboPiece") ? j.getInt("comboPiece") : -1;
        s.prevPos = j.has("prevPos") && !j.isNull("prevPos") ? j.getInt("prevPos") : -1;
        if (j.has("visited")) for (int v : j.getIntArray("visited")) s.visitedMask |= (1L << v);
        if (s.inCombo && s.prevPos != -1 && s.comboPiece != -1) for (int d = 0; d < 8; d++)
            if (ADJ_INDEX[s.prevPos][d] == s.comboPiece) {
                s.lastDir = d;
                break;
            }
        s.initHash();
        return s;
    }

    static void initTables() {
        for (int r = 0; r < 5; r++) {
            for (int c = 0; c < 9; c++) {
                int curr = r * 9 + c;
                POS_NAMES[curr] = "" + (char) ('a' + c) + (5 - r);
                POS_R[curr] = r;
                POS_C[curr] = c;
                boolean isStrong = (r + c) % 2 == 0;
                for (int d = 0; d < 8; d++) {
                    ADJ_INDEX[curr][d] = -1;
                    if (!isStrong && (d % 2 != 0)) continue;
                    int nr = r + DR[d], nc = c + DC[d];
                    if (nr >= 0 && nr < 5 && nc >= 0 && nc < 9) ADJ_INDEX[curr][d] = nr * 9 + nc;
                }
            }
        }
    }

    static class StepResult {
        GameState state;
        boolean win;

        public StepResult(GameState s, boolean w) {
            state = s;
            win = w;
        }
    }
}
