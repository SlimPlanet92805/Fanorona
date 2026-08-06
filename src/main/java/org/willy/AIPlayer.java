package org.willy;

import java.io.*;
import java.util.*;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

class AIPlayer {
    static final int INF = 100000000;
    static final int MATE_SCORE = 90000000;
    static final int MATE_THRESHOLD = 80000000;

    private final SearchConfig config;

    private int lastScore = 0;
    private int pendingAIMove = -1;

    AIPlayer(SearchConfig config) {
        this.config = config;
    }

    void setupPersistence(String memoryFile) {
        Executors.newSingleThreadScheduledExecutor()
                .scheduleAtFixedRate(() -> saveMemory(memoryFile), 1, 1, TimeUnit.MINUTES);
    }

    static class TTEntry {
        long key;
        int depth, score, flag, bestMove;

        public TTEntry(long k, int d, int s, int f, int m) {
            key = k;
            depth = d;
            score = s;
            flag = f;
            bestMove = m;
        }
    }

    private final Map<Long, TTEntry> tt = new ConcurrentHashMap<>(300000);
    private final int[][] history = new int[46][46];
    private final List<Long> globalHistory = Collections.synchronizedList(new ArrayList<>());
    private int lastPredictedOpponentMove = -1;
    private String lastPredFeedback = "Initial";

    public void recordState(long h) {
        globalHistory.add(h);
        if (globalHistory.size() > 500) globalHistory.removeFirst();
    }

    public void resetGame() {
        globalHistory.clear();
        lastPredFeedback = "Reset";
        lastPredictedOpponentMove = -1;
    }

    /**
     * Raw search output. Deliberately carries no rendered commentary: the
     * caller decides whether to show {@link #statsLog} or feed the rest to
     * {@link TrashTalk}, so the engine stays presentation-free for the port.
     */
    static class AIResult {
        int bestMove, score;
        /** Score after the AI's previous move, for spotting a human blunder. */
        int prevScore;
        boolean isMate;
        String statsLog, strategy, predFeedback;
        /** Search effort, for the benchmark's parity comparison. */
        long nodes, ttHits;
        /** Deepest iteration actually completed. */
        int depth;

        AIResult(int bestMove, int score, int prevScore, boolean isMate,
                 String statsLog, String strategy, String predFeedback) {
            this.bestMove = bestMove;
            this.score = score;
            this.prevScore = prevScore;
            this.isMate = isMate;
            this.statsLog = statsLog;
            this.strategy = strategy;
            this.predFeedback = predFeedback;
        }
    }

    static class SearchContext {
        long start, end;
        boolean stop;
        long nodes, ttHits, rootNodesBest;

        SearchContext(long t) {
            start = System.currentTimeMillis();
            end = start + t;
        }

        void check() {
            if ((nodes++ & 4095) == 0 && System.currentTimeMillis() > end) stop = true;
        }
    }

    public AIResult think(GameState root) {
        int myC = Long.bitCount(root.myPieces), oppC = Long.bitCount(root.oppPieces);
        SearchContext ctx = new SearchContext(config.timeLimitMs);

        List<Move> moves = GameLogic.getDetailedMoves(root);
        if (moves.isEmpty())
            return new AIResult(720, -MATE_SCORE, lastScore, true, "Surrender", "Resign", lastPredFeedback);

        int bestMove = moves.get(0).actionId;
        int score = 0;
        int reachedDepth = 0;
        String stopReason = "MaxDepth";
        sortByCaptures(moves);

        boolean amWinning = myC > oppC + 1;

        long iterationStartNodes = 0;
        for (int d = 1; d <= config.maxDepth; d++) {
            ctx.rootNodesBest = 0;
            iterationStartNodes = ctx.nodes;
            if (d > 1) sortByCaptures(moves, bestMove); // previous iteration's best first

            int alpha = -INF, beta = INF;
            int bestS = -INF, bestM = moves.get(0).actionId;

            for (int i = 0; i < moves.size(); i++) {
                Move m = moves.get(i);
                GameLogic.StepResult res = GameLogic.step(root, m.actionId);

                boolean isLoop = globalHistory.contains(res.state.zobristHash);
                int cur;
                if (isLoop) {
                    cur = amWinning ? -25000 : 0;
                } else {
                    if (res.state.player == root.player) cur = negascout(ctx, res.state, d, alpha, beta);
                    else cur = -negascout(ctx, res.state, d - 1, -beta, -alpha);
                }

                if (ctx.stop) break;
                if (i == 0) ctx.rootNodesBest += (ctx.nodes - iterationStartNodes);
                if (cur > bestS) {
                    bestS = cur;
                    bestM = m.actionId;
                }
                alpha = Math.max(alpha, cur);
                if (alpha >= beta) break;
            }

            if (ctx.stop) {
                stopReason = "Time";
                break;
            }
            score = bestS;
            bestMove = bestM;
            reachedDepth = d;
            tt.put(root.zobristHash, new TTEntry(root.zobristHash, d, score, 0, bestMove));
            if (Math.abs(score) > MATE_THRESHOLD) {
                stopReason = "Mate";
                break;
            }
        }

        double hits = (ctx.ttHits * 100.0) / Math.max(1, ctx.nodes);
        long nodesInThisIteration = ctx.nodes - iterationStartNodes;
        double rootPct = (ctx.rootNodesBest * 100.0) / Math.max(1, nodesInThisIteration);
        double nps = ctx.nodes / (Math.max(1, System.currentTimeMillis() - ctx.start) / 1000.0);

        GameLogic.StepResult resFinal = GameLogic.step(root, bestMove);
        TTEntry nextE = tt.get(resFinal.state.zobristHash);
        lastPredictedOpponentMove = (nextE != null) ? nextE.bestMove : -1;
        String statsLog = String.format("[%s] D:%d N:%dk NPS:%.0fk | Hits:%.0f%% | Root:%.0f%% | Pred:%s", stopReason, getDepth(root), ctx.nodes / 1000, nps / 1000, hits, rootPct, lastPredFeedback) + " | " + getNarrativePV(root, bestMove, config.maxDepth);
        config.log("> Score(" + score + "): " + statsLog);

        // Snapshot the previous score before overwriting it: the blunder-detection
        // branch in TrashTalk compares the two, and reading lastScore after the
        // assignment (as this used to) made the difference unconditionally zero.
        int prevScore = lastScore;
        lastScore = score;
        pendingAIMove = bestMove;

        AIResult result = new AIResult(bestMove, score, prevScore, Math.abs(score) > MATE_THRESHOLD,
                statsLog, getStrategy(score), lastPredFeedback);
        result.nodes = ctx.nodes;
        result.ttHits = ctx.ttHits;
        result.depth = reachedDepth;
        return result;
    }

    /** Most captures first. Stable, so equal-capture moves keep generation order. */
    private static void sortByCaptures(List<Move> moves) {
        moves.sort(Comparator.comparingInt((Move m) -> m.victims.size()).reversed());
    }

    /** As {@link #sortByCaptures(List)}, but pins {@code first} to the front. */
    private static void sortByCaptures(List<Move> moves, int first) {
        moves.sort(Comparator.comparingLong((Move m) -> m.actionId == first
                ? Long.MAX_VALUE
                : m.victims.size()).reversed());
    }

    private static String getStrategy(int score) {
        String strategy;
        if (score > MATE_THRESHOLD) {
            strategy = "Checkmate";
        } else if (score < -MATE_THRESHOLD) {
            strategy = "Defeat";
        } else if (score >= 2000) {
            strategy = "Crushing";
        } else if (score >= 500) {
            strategy = "Advantage";
        } else if (score <= -2000) {
            strategy = "Critical";
        } else if (score <= -500) {
            strategy = "Pressure";
        } else {
            strategy = "Balanced";
        }
        return strategy;
    }

    public void analyzeHumanMove(int actualMove) {
        if (actualMove == pendingAIMove) {
            pendingAIMove = -1;
            lastPredFeedback = "Wait...";
            return;
        }
        if (lastPredictedOpponentMove != -1) {
            lastPredFeedback = (actualMove == lastPredictedOpponentMove) ? "Hit🎯" : "Miss🧐";
        }
    }

    private int getDepth(GameState r) {
        TTEntry e = tt.get(r.zobristHash);
        return e != null ? e.depth : 0;
    }

    private int negascout(SearchContext ctx, GameState s, int d, int alpha, int beta) {
        ctx.check();
        if (ctx.stop) return alpha;
        TTEntry e = tt.get(s.zobristHash);
        if (e != null && e.depth >= d) {
            ctx.ttHits++;
            if (e.flag == 0) return e.score;
            if (e.flag == 1) alpha = Math.max(alpha, e.score);
            if (e.flag == 2) beta = Math.min(beta, e.score);
            if (alpha >= beta) return e.score;
        }
        if (s.oppPieces == 0) return MATE_SCORE - (50 - d);
        if (d <= 0) return s.inCombo ? negascout(ctx, s, 1, alpha, beta) : evaluate(s);
        List<Move> moves = GameLogic.getDetailedMoves(s);
        if (moves.isEmpty()) return -MATE_SCORE + (50 - d);
        int ttM = (e != null) ? e.bestMove : -1;
        // Key-extraction sort rather than a subtracting comparator: history values
        // grow without bound (see the d*d accumulation below), so a subtraction
        // could overflow int and break the strict weak ordering that sort requires.
        moves.sort(Comparator.comparingLong((Move m) -> m.actionId == ttM
                ? Long.MAX_VALUE
                : (long) m.victims.size() * 1000L + history[m.from + 1][m.to + 1]).reversed());
        int bestV = -INF, bestA = -1, alphaO = alpha;
        for (int i = 0; i < moves.size(); i++) {
            Move m = moves.get(i);
            GameLogic.StepResult res = GameLogic.step(s, m.actionId);
            int val;
            if (res.state.player == s.player) val = negascout(ctx, res.state, d, alpha, beta);
            else {
                if (i == 0) val = -negascout(ctx, res.state, d - 1, -beta, -alpha);
                else {
                    val = -negascout(ctx, res.state, d - 1, -alpha - 1, -alpha);
                    if (val > alpha && val < beta) val = -negascout(ctx, res.state, d - 1, -beta, -alpha);
                }
            }
            if (ctx.stop) return alpha;
            if (val > bestV) {
                bestV = val;
                bestA = m.actionId;
            }
            if (val > alpha) {
                alpha = val;
                if (m.from >= 0) bumpHistory(m.from + 1, m.to + 1, d * d);
            }
            if (alpha >= beta) break;
        }
        if (!ctx.stop)
            tt.put(s.zobristHash, new TTEntry(s.zobristHash, d, bestV, (bestV <= alphaO ? 2 : (bestV >= beta ? 1 : 0)), bestA));
        return bestV;
    }

    /**
     * Ceiling on a history score. Once any entry would pass it, the whole table
     * is halved, which preserves the relative ordering the sort cares about while
     * keeping the values far away from int overflow. Without this the table grows
     * without bound across a long session and eventually wraps negative.
     */
    private static final int HISTORY_MAX = 1 << 24;

    private void bumpHistory(int from, int to, int bonus) {
        int v = history[from][to] + bonus;
        if (v >= HISTORY_MAX) {
            for (int r = 0; r < 46; r++)
                for (int c = 0; c < 46; c++) history[r][c] >>= 1;
            v >>= 1;
        }
        history[from][to] = v;
    }

    /** Ceiling on the "close the distance" term; mirrors {@code eval_hunt_cap} in eval.cpp. */
    static final int HUNT_CAP = 50;

    /** Package-private so {@link Bench} can diff it against the C++ port directly. */
    static int evaluate(GameState s) {
        int myC = Long.bitCount(s.myPieces), oppC = Long.bitCount(s.oppPieces);
        int sc = (myC - oppC) * 100;
        long t = s.myPieces;
        while (t != 0) {
            sc += GameLogic.POS_VAL[Long.numberOfTrailingZeros(t)];
            t &= (t - 1);
        }
        t = s.oppPieces;
        while (t != 0) {
            sc -= GameLogic.POS_VAL[Long.numberOfTrailingZeros(t)];
            t &= (t - 1);
        }
        if (myC > oppC && oppC > 0) {
            int enemyP = Long.numberOfTrailingZeros(s.oppPieces);
            int er = GameLogic.POS_R[enemyP], ec = GameLogic.POS_C[enemyP];
            long my = s.myPieces;
            int dSum = 0;
            while (my != 0) {
                int p = Long.numberOfTrailingZeros(my);
                dSum += Math.abs(GameLogic.POS_R[p] - er) + Math.abs(GameLogic.POS_C[p] - ec);
                my &= (my - 1);
            }
            // Mean distance, not the sum: as a sum this reached -528, outweighing
            // five pieces of material. Kept identical to eval.cpp's eval_hunt_cap
            // path so the two engines stay bit-for-bit comparable.
            sc -= Math.min(dSum * 2 / myC, HUNT_CAP);
        }
        return sc;
    }

    private String getNarrativePV(GameState root, int first, int maxSteps) {
        StringBuilder sb = new StringBuilder();
        GameState curr = root.copy();
        Set<Long> seen = new HashSet<>();
        int mid = first;
        int startP = root.player;
        int lastP = startP;

        sb.append("A:");

        for (int i = 0; i < maxSteps; i++) {
            if (seen.contains(curr.zobristHash)) break;
            seen.add(curr.zobristHash);

            List<Move> ms = GameLogic.getDetailedMoves(curr);
            Move m = null;
            for (Move move : ms)
                if (move.actionId == mid) {
                    m = move;
                    break;
                }
            if (m == null) break;

            if (curr.player != lastP) {
                sb.append(" | ").append(curr.player == startP ? "A:" : "H:");
                lastP = curr.player;
            }

            if (m.actionId == 720) sb.append("Stop ");
            else sb.append(GameLogic.POS_NAMES[m.from]).append("-").append(GameLogic.POS_NAMES[m.to]).append(" ");

            GameLogic.StepResult res = GameLogic.step(curr, mid);
            if (res.win) {
                sb.append("#WIN ");
                break;
            }
            curr = res.state;
            TTEntry e = tt.get(curr.zobristHash);
            if (e == null) break;
            mid = e.bestMove;
        }
        return sb.toString().trim();
    }

    public int getMemorySize() {
        return tt.size();
    }

    public void loadMemory(String p) {
        File f = new File(p);
        if (!f.exists()) return;
        try (DataInputStream dis = new DataInputStream(new BufferedInputStream(new FileInputStream(f)))) {
            config.log("🧠 Loading memory...");
            int size = dis.readInt();
            for (int i = 0; i < size; i++) {
                long k = dis.readLong();
                int d = dis.readInt();
                int s = dis.readInt();
                int fl = dis.readInt();
                int bm = dis.readInt();
                tt.put(k, new TTEntry(k, d, s, fl, bm));
            }
            if (dis.available() > 0) {
                for (int r = 0; r < 46; r++) for (int c = 0; c < 46; c++) history[r][c] = dis.readInt();
            }
            config.log("✅ Entries restored: " + tt.size());
        } catch (Exception e) {
            config.log("Memory corrupted and reset");
        }
    }

    public synchronized void saveMemory(String p) {
        // Pruning Logic
        if (tt.size() > config.maxMemoryEntries) {
            config.log("✂️ Memory limit reached (" + tt.size() + "). Pruning...");
            int oldSize = tt.size();
            tt.entrySet().removeIf(e -> e.getValue().depth <= 2);

            if (tt.size() > config.maxMemoryEntries) {
                tt.entrySet().removeIf(e -> e.getValue().depth <= 4);
            }

            if (tt.size() > config.maxMemoryEntries) {
                tt.entrySet().removeIf(e -> e.getValue().depth <= 8);
            }

            if (tt.size() > config.maxMemoryEntries) {
                config.log("⚠️ Still over limit, forced truncation by depth...");
                List<Map.Entry<Long, TTEntry>> list = new ArrayList<>(tt.entrySet());

                list.sort(Comparator.comparingInt((Map.Entry<Long, TTEntry> a) -> a.getValue().depth).reversed());

                tt.clear();
                for (int i = 0; i < config.maxMemoryEntries; i++) {
                    Map.Entry<Long, TTEntry> entry = list.get(i);
                    tt.put(entry.getKey(), entry.getValue());
                }
            }
            config.log("✂️ Pruning complete. Size: " + oldSize + " -> " + tt.size());
        }

        try (DataOutputStream dos = new DataOutputStream(new BufferedOutputStream(new FileOutputStream(p)))) {
            dos.writeInt(tt.size());
            for (TTEntry e : tt.values()) {
                dos.writeLong(e.key);
                dos.writeInt(e.depth);
                dos.writeInt(e.score);
                dos.writeInt(e.flag);
                dos.writeInt(e.bestMove);
            }
            for (int r = 0; r < 46; r++) for (int c = 0; c < 46; c++) dos.writeInt(history[r][c]);
            dos.flush();
            config.log("💾 Memory saved (" + tt.size() + " entries)");
        } catch (Exception e) {
            config.log("Memory save failed: " + e);
        }
    }
}
