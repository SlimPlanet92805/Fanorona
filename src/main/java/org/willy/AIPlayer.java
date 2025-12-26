package org.willy;

import java.io.*;
import java.util.*;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

import static org.willy.FanoronaServer.*;

class AIPlayer {
    private int lastScore = 0;
    private int pendingAIMove = -1;

    static void setupPersistence() {
        Executors.newSingleThreadScheduledExecutor().scheduleAtFixedRate(() -> FanoronaServer.aiPlayer.saveMemory(FanoronaServer.MEMORY_FILE), 1, 1, TimeUnit.MINUTES);
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

    static class AIResult {
        int bestMove, score;
        String pv, strategy;

        public AIResult(int m, int s, String p, String st) {
            bestMove = m;
            score = s;
            pv = p;
            strategy = st;
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
        SearchContext ctx = new SearchContext(Time_LIMIT);

        List<Move> moves = GameLogic.getDetailedMoves(root);
        if (moves.isEmpty()) return new AIResult(720, -FanoronaServer.MATE_SCORE, "Surrender", "Resign");

        int bestMove = moves.get(0).actionId;
        int score = 0;
        String stopReason = "MaxDepth";
        moves.sort((a, b) -> b.victims.size() - a.victims.size());

        boolean amWinning = myC > oppC + 1;

        long iterationStartNodes = 0;
        for (int d = 1; d <= 1000; d++) {
            ctx.rootNodesBest = 0;
            iterationStartNodes = ctx.nodes;
            if (d > 1) {
                final int lastBest = bestMove; // Best action ID
                moves.sort((a, b) -> {
                    if (a.actionId == lastBest) return -1; // Put it at the front
                    if (b.actionId == lastBest) return 1;
                    return b.victims.size() - a.victims.size();
                });
            }

            int alpha = -FanoronaServer.INF, beta = FanoronaServer.INF;
            int bestS = -FanoronaServer.INF, bestM = moves.get(0).actionId;

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
            tt.put(root.zobristHash, new TTEntry(root.zobristHash, d, score, 0, bestMove));
            if (Math.abs(score) > FanoronaServer.MATE_THRESHOLD) {
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
        String statsLog = String.format("[%s] D:%d N:%dk NPS:%.0fk | Hits:%.0f%% | Root:%.0f%% | Pred:%s", stopReason, getDepth(root), ctx.nodes / 1000, nps / 1000, hits, rootPct, lastPredFeedback) + " | " + getNarrativePV(root, bestMove, MAX_DEPTH);
        System.out.println("> " + statsLog);

        String strategy = getStrategy(score);
        lastScore = score;
        pendingAIMove = bestMove;
        if (HIDE_DETAILED_LOG) {
            boolean isMate = Math.abs(score) > FanoronaServer.MATE_THRESHOLD;
            String aiMessage = getTrashTalk(score, lastScore, isMate, lastPredFeedback);
            return new AIResult(bestMove, score, aiMessage, strategy);
        } else {
            return new AIResult(bestMove, score, statsLog, strategy);
        }
    }

    private static String getStrategy(int score) {
        String strategy;
        if (score > FanoronaServer.MATE_THRESHOLD) {
            strategy = "Checkmate";
        } else if (score < -FanoronaServer.MATE_THRESHOLD) {
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
        if (s.oppPieces == 0) return FanoronaServer.MATE_SCORE - (50 - d);
        if (d <= 0) return s.inCombo ? negascout(ctx, s, 1, alpha, beta) : evaluate(s);
        List<Move> moves = GameLogic.getDetailedMoves(s);
        if (moves.isEmpty()) return -FanoronaServer.MATE_SCORE + (50 - d);
        int ttM = (e != null) ? e.bestMove : -1;
        moves.sort((a, b) -> {
            if (a.actionId == ttM) return -1;
            if (b.actionId == ttM) return 1;
            return (b.victims.size() * 1000 + history[b.from + 1][b.to + 1]) - (a.victims.size() * 1000 + history[a.from + 1][a.to + 1]);
        });
        int bestV = -FanoronaServer.INF, bestA = -1, alphaO = alpha;
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
                if (m.from >= 0) history[m.from + 1][m.to + 1] += d * d;
            }
            if (alpha >= beta) break;
        }
        if (!ctx.stop)
            tt.put(s.zobristHash, new TTEntry(s.zobristHash, d, bestV, (bestV <= alphaO ? 2 : (bestV >= beta ? 1 : 0)), bestA));
        return bestV;
    }

    private int evaluate(GameState s) {
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
            sc -= (dSum * 2);
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
            System.out.println("🧠 Loading memory...");
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
            System.out.println("✅ Entries restored: " + tt.size());
        } catch (Exception e) {
            System.err.println("Memory corrupted and reset");
        }
    }

    public synchronized void saveMemory(String p) {
        // Pruning Logic
        if (tt.size() > FanoronaServer.MAX_MEMORY_ENTRIES) {
            System.out.println("✂️ Memory limit reached (" + tt.size() + "). Pruning...");
            int oldSize = tt.size();
            tt.entrySet().removeIf(e -> e.getValue().depth <= 2);

            if (tt.size() > FanoronaServer.MAX_MEMORY_ENTRIES) {
                tt.entrySet().removeIf(e -> e.getValue().depth <= 4);
            }

            if (tt.size() > FanoronaServer.MAX_MEMORY_ENTRIES) {
                tt.entrySet().removeIf(e -> e.getValue().depth <= 8);
            }

            if (tt.size() > FanoronaServer.MAX_MEMORY_ENTRIES) {
                System.out.println("⚠️ Still over limit, forced truncation by depth...");
                List<Map.Entry<Long, TTEntry>> list = new ArrayList<>(tt.entrySet());

                list.sort((a, b) -> b.getValue().depth - a.getValue().depth);

                tt.clear();
                for (int i = 0; i < FanoronaServer.MAX_MEMORY_ENTRIES; i++) {
                    Map.Entry<Long, TTEntry> entry = list.get(i);
                    tt.put(entry.getKey(), entry.getValue());
                }
            }
            System.out.println("✂️ Pruning complete. Size: " + oldSize + " -> " + tt.size());
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
            System.out.println("💾 Memory saved (" + tt.size() + " entries)");
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    private String getTrashTalk(int score, int prevScore, boolean isMate, String feedback) {
        int diff = score - prevScore;
        boolean amWinning = score > 300;
        Random rnd = new Random();

        // --- Scenario A: Checkmate ---
        if (isMate) {
            if (score > 0) {
                String[] msgs = {
                        "蚌埠住了，这也能输？😂",
                        "赢麻了家人们 🥳",
                        "杀杀杀杀杀！ ⚔️",
                        "纯纯的送 💀",
                        "纯纯的乱杀 🎯",
                        "这就是你的全部实力吗？😏",
                        "下得不错，下次别下了。🤡",
                        "就这？就这？ 🤷",
                        "你是来搞笑的吧？ 🤡",
                        "建议多练练再来 ♿",
                        "结束了，下把加油吧。☕"
                };
                return msgs[rnd.nextInt(msgs.length)];
            } else {
                String[] msgs = {
                        "不是哥们，开了？这都能输？举报了！🚨",
                        "卧槽？外挂！举报了 🚨"
                };
                return msgs[rnd.nextInt(msgs.length)]; // AI losing (rare)
            }
        }

        // --- Scenario B: Player Blunder (AI score spiked up) ---
        // If score jumps up by > 400, player likely made a mistake
        if (diff > 400 && amWinning) {
            String[] msgs = {
                    "谢谢你，这步我记一辈子。🙏",
                    "兄弟你这是在下棋，还是在送？🎁",
                    "我刚还在想怎么赢，你就帮我想好了。🤝",
                    "你这一手我直接笑死 😂",
                    "这就是你的实力？ 🐔",
                    "典中典之送子大师 📦",
                    "别送了别送了，我吃不下了 🍽️",
                    "这步一出，胜率直接起飞。📈",
                    "你刚刚那下，是手滑还是心软？🤨"
            };
            return msgs[rnd.nextInt(msgs.length)];
        }

        // --- Scenario C: Prediction Hit (AI read the player) ---
        if (feedback.contains("Hit") && amWinning) {
            String[] msgs = {
                    "想什么呢，我就知道你会走这步。📖",
                    "我都不用算，你肯定这么下。😑",
                    "果然，老套路了。🥱",
                    "你的想法被我看穿了 👁️",
                    "早就猜到你要这么走 🎯",
                    "太好猜了，没挑战性 🥱",
                    "你这思路，我小学就会了。📘",
                    "别急，我知道你下一步想干嘛。🔮",
                    "你是不是在按我剧本走？🎬"
            };
            return msgs[rnd.nextInt(msgs.length)];
        }

        // --- Scenario D: AI is Crushing (> 2000 score) ---
        if (score > 2000) {
            String[] msgs = {
                    "这盘已经不是技术问题了。😶",
                    "建议直接下一把，真的。🏳️",
                    "投了吧，别挣扎了 🏳️",
                    "我这边显示的是教学模式。📚",
                    "别下了，越下越难看。😬"
            };
            return msgs[rnd.nextInt(msgs.length)];
        }

        // --- Scenario E: AI Disadvantage (Rare) ---
        if (score < -500) {
            String[] msgs = {
                    "别高兴太早，我还没认真呢。😏",
                    "让你几步，测试一下你成色。🧐",
                    "现在是让分局，懂？📉",
                    "只是让你几步罢了 🤨"
            };
            return msgs[rnd.nextInt(msgs.length)];
        }

        // --- Scenario F: Neutral / Thinking ---
        String[] fillers = {
                "我算算……你先别急。🤔",
                "让我康康... 🧐",
                "嗯……这盘有点意思。😐",
                "让我想想怎么让你输得体面点... 💭",
                "正在计算你的108种死法... 🌌",
                "别急，等我给你安排得明明白白。🤖",
                "你猜我下一步走哪？ 🎲",
                "别催别催，马上 ⏰",
                "嗯... 你这是要干嘛？ 😕",
                "继续，下给我看看。👀"
        };
        return fillers[rnd.nextInt(fillers.length)];
    }
}
