package org.willy;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/**
 * Fixed-depth, deterministic search over a fixed set of positions.
 * <p>
 * This exists to make the planned C++ port verifiable. Node counts are the
 * strongest available equivalence check between two implementations of the same
 * search: if the C++ engine visits exactly the same number of nodes and returns
 * the same move and score, it is almost certainly doing the same thing. Speed is
 * then a clean comparison, because both sides did identical work.
 * <p>
 * Depth is fixed rather than time, because the time-based abort in
 * {@link AIPlayer.SearchContext#check()} makes node counts depend on how fast
 * the machine happens to be, which is useless for comparison. For the same
 * reason each position gets a fresh {@link AIPlayer}: no carried-over
 * transposition table, no accumulated history heuristic, no saved memory file.
 */
class Bench {

    /**
     * Deep enough that the run does real work (~1.7M nodes) and the JVM is
     * warm by the end, shallow enough to stay under a second so it can sit in
     * CI as a regression gate.
     */
    private static final int DEFAULT_DEPTH = 8;
    private static final String DEFAULT_POSITIONS = "bench/positions.txt";

    static boolean isBenchRun(String[] args) {
        for (String a : args) if (a.startsWith("--bench")) return true;
        return false;
    }

    static void run(String[] args) {
        GameLogic.initTables();
        Zobrist.init();

        if (arg(args, "--bench-gen") != null) {
            generate(arg(args, "--positions", DEFAULT_POSITIONS));
            return;
        }
        String dump = arg(args, "--bench-dump");
        if (dump != null && !dump.isEmpty()) {
            dump(dump, arg(args, "--positions", DEFAULT_POSITIONS));
            return;
        }
        String perftDepth = arg(args, "--bench-perft");
        if (perftDepth != null) {
            perft(arg(args, "--positions", DEFAULT_POSITIONS),
                  Integer.parseInt(perftDepth.isEmpty() ? "4" : perftDepth));
            return;
        }

        int depth = Integer.parseInt(arg(args, "--depth", String.valueOf(DEFAULT_DEPTH)));
        String path = arg(args, "--positions", DEFAULT_POSITIONS);
        // Fixed time instead of fixed depth: this is the comparison that maps
        // onto play strength, since what matters in a real game is how deep an
        // engine gets within the move budget it is given.
        int movetime = Integer.parseInt(arg(args, "--movetime", "0"));

        List<String> lines;
        try {
            lines = readPositions(path);
        } catch (IOException e) {
            System.err.println("Cannot read " + path + ": " + e.getMessage());
            System.err.println("Generate it first with: --bench-gen");
            return;
        }

        if (movetime > 0) System.out.println("# fanorona bench movetime=" + movetime + "ms positions=" + lines.size());
        else System.out.println("# fanorona bench depth=" + depth + " positions=" + lines.size());
        long totalNodes = 0, totalHits = 0, totalMs = 0, totalDepth = 0;

        for (int i = 0; i < lines.size(); i++) {
            GameState s = GameStateCodec.fromText(lines.get(i));

            SearchConfig cfg = new SearchConfig();
            cfg.maxDepth = movetime > 0 ? 1000 : depth;
            cfg.timeLimitMs = movetime > 0 ? movetime : Integer.MAX_VALUE;
            cfg.logger = null;
            AIPlayer ai = new AIPlayer(cfg);

            long t0 = System.nanoTime();
            AIPlayer.AIResult r = ai.think(s);
            long ms = (System.nanoTime() - t0) / 1_000_000;

            if (movetime > 0)
                System.out.printf("%d depth=%d nodes=%d score=%d bestMove=%d%n",
                        i, r.depth, r.nodes, r.score, r.bestMove);
            else
                System.out.printf("%d nodes=%d ttHits=%d score=%d bestMove=%d%n",
                        i, r.nodes, r.ttHits, r.score, r.bestMove);
            totalNodes += r.nodes;
            totalHits += r.ttHits;
            totalDepth += r.depth;
            totalMs += ms;
        }
        if (movetime > 0) System.out.printf("# total depth=%d nodes=%d%n", totalDepth, totalNodes);
        else

        System.out.printf("# total nodes=%d ttHits=%d%n", totalNodes, totalHits);

        // Timing goes to stderr so stdout stays byte-stable and can be diffed
        // straight against the golden file:
        //     --bench > bench/golden.txt
        System.err.printf("# wall %d ms, %.0f knps%n",
                totalMs, totalMs > 0 ? totalNodes / (double) totalMs : 0.0);
    }

    /** Skips blank lines and {@code #} comments so the file can document itself. */
    static List<String> readPositions(String path) throws IOException {
        List<String> out = new ArrayList<>();
        for (String line : Files.readAllLines(Path.of(path), StandardCharsets.UTF_8)) {
            String t = line.trim();
            if (!t.isEmpty() && !t.startsWith("#")) out.add(t);
        }
        return out;
    }

    // ---------------------------------------------------------------- perft

    /**
     * Counts leaf nodes of the pure move-generation tree.
     * <p>
     * Unlike the search benchmark, this touches no transposition table, no
     * evaluation and no move ordering — only the rules. That makes it the one
     * correctness check that stays valid through every search optimisation:
     * a faster transposition table or a parallel search is *supposed* to change
     * search node counts, but it must never change perft.
     */
    static long perft(GameState s, int depth) {
        List<Move> moves = GameLogic.getDetailedMoves(s);
        if (depth <= 1) return moves.size();
        long n = 0;
        for (Move m : moves) n += perft(GameLogic.step(s, m.actionId).state, depth - 1);
        return n;
    }

    private static void perft(String path, int maxDepth) {
        List<String> lines = positions(path);
        System.out.println("# perft maxDepth=" + maxDepth + " positions=" + lines.size());
        for (int i = 0; i < lines.size(); i++) {
            GameState s = GameStateCodec.fromText(lines.get(i));
            StringBuilder sb = new StringBuilder();
            sb.append(i);
            for (int d = 1; d <= maxDepth; d++) sb.append(' ').append(perft(s, d));
            System.out.println(sb);
        }
    }

    // ---------------------------------------------------------------- sub-harnesses

    /**
     * Dumps one layer of the engine at a time.
     * <p>
     * A whole-search node-count divergence is close to impossible to localise,
     * so the C++ port is checked bottom-up: get {@code zobrist} matching before
     * looking at {@code movegen}, {@code movegen} before {@code step}, and so on.
     * Each mode prints a stable, diffable text stream and nothing else.
     */
    private static void dump(String what, String path) {
        switch (what) {
            case "zobrist" -> {
                // Depends on Java's exact LCG; the first thing to break in a port.
                for (int i = 0; i < GameLogic.NUM_POS; i++)
                    System.out.printf("P %d 0 %016x%nP %d 1 %016x%n",
                            i, Zobrist.P[i][0], i, Zobrist.P[i][1]);
                System.out.printf("T %016x%n", Zobrist.T);
            }
            case "codec" -> {
                int bad = 0;
                for (String line : positions(path)) {
                    GameState s = GameStateCodec.fromText(line);
                    String back = GameStateCodec.toText(s);
                    if (!back.equals(line)) {
                        bad++;
                        System.out.println("MISMATCH\n  in : " + line + "\n  out: " + back);
                    }
                }
                System.out.println(bad == 0 ? "# codec round-trip OK" : "# " + bad + " MISMATCHES");
            }
            case "movegen" -> {
                for (String line : positions(path)) {
                    GameState s = GameStateCodec.fromText(line);
                    System.out.printf("pos %016x%n", s.zobristHash);
                    for (Move m : GameLogic.getDetailedMoves(s))
                        System.out.printf("  move id=%d from=%d to=%d type=%s victims=%d%n",
                                m.actionId, m.from, m.to, m.type, m.victims.size());
                }
            }
            case "step" -> {
                for (String line : positions(path)) {
                    GameState s = GameStateCodec.fromText(line);
                    System.out.printf("pos %016x%n", s.zobristHash);
                    for (Move m : GameLogic.getDetailedMoves(s)) {
                        GameLogic.StepResult r = GameLogic.step(s, m.actionId);
                        System.out.printf("  %d -> %s win=%b hash=%016x lastDir=%d%n",
                                m.actionId, GameStateCodec.toText(r.state), r.win,
                                r.state.zobristHash, r.state.lastDir);
                    }
                }
            }
            case "eval" -> {
                for (String line : positions(path)) {
                    GameState s = GameStateCodec.fromText(line);
                    System.out.printf("%016x %d%n", s.zobristHash, AIPlayer.evaluate(s));
                    // Also evaluate every child, for far more coverage than the
                    // six root positions alone would give.
                    for (Move m : GameLogic.getDetailedMoves(s)) {
                        GameState c = GameLogic.step(s, m.actionId).state;
                        System.out.printf("  %d %016x %d%n", m.actionId, c.zobristHash, AIPlayer.evaluate(c));
                    }
                }
            }
            default -> System.err.println(
                    "unknown dump '" + what + "'; expected zobrist|codec|movegen|step|eval");
        }
    }

    private static List<String> positions(String path) {
        try {
            return readPositions(path);
        } catch (IOException e) {
            throw new RuntimeException("Cannot read " + path + " (generate it with --bench-gen)", e);
        }
    }

    // ---------------------------------------------------------------- generation

    private static final String START_BOARD =
            "wwwwwwwwwwwwwwwwww" + "wbwb.wbwb" + "bbbbbbbbbbbbbbbbbb";

    /**
     * Walks one deterministic game and saves the positions worth benchmarking.
     * <p>
     * Move choice is greedy on captures with an actionId tie-break, so the walk
     * is reproducible without a random seed, and it naturally passes through
     * capture chains — which is what exercises the combo branch in
     * {@code GameLogic.step} and the {@code d <= 0 && inCombo} extension in the
     * search, the two places a port is most likely to diverge.
     */
    private static void generate(String path) {
        // Play the whole game first, then sample it proportionally. Sampling at
        // fixed ply numbers would miss: greedy capture play finishes in far fewer
        // moves than a real game, so most fixed offsets fall past the end.
        List<GameState> game = new ArrayList<>();
        GameState s = GameStateCodec.fromText(START_BOARD + " 1 0 -1 -1 0");
        game.add(s);

        boolean ended = false;
        for (int ply = 1; ply <= 400; ply++) {
            List<Move> moves = GameLogic.getDetailedMoves(s);
            if (moves.isEmpty()) {
                ended = true;
                break;
            }
            Move best = moves.stream()
                    .filter(m -> m.actionId != 720)
                    .max(Comparator.<Move>comparingInt(m -> m.victims.size())
                            .thenComparing(m -> -m.actionId))
                    .orElse(moves.get(0));

            GameLogic.StepResult res = GameLogic.step(s, best.actionId);
            s = res.state;
            game.add(s);
            if (res.win) {
                ended = true;
                break;
            }
        }

        List<String> picked = new ArrayList<>();
        List<String> notes = new ArrayList<>();
        int n = game.size();

        add(picked, notes, game.get(0), "opening (initial position)");
        add(picked, notes, game.get(Math.max(1, n / 6)), "early game");
        add(picked, notes, game.get(n / 2), "midgame");
        add(picked, notes, game.get(Math.min(n - 1, n * 4 / 5)), "late game");

        // The combo branch in step() and the `d <= 0 && inCombo` extension in the
        // search are where a port is most likely to diverge, so make sure at
        // least one mid-chain position is covered.
        for (GameState g : game) {
            if (g.inCombo) {
                add(picked, notes, g, "mid-capture chain (inCombo)");
                break;
            }
        }
        if (ended) add(picked, notes, game.get(n - 1),
                "terminal (game over: no moves, or opponent wiped out)");

        StringBuilder sb = new StringBuilder();
        sb.append("# Benchmark positions for the Java/C++ parity check.\n");
        sb.append("# Format: <45 board chars .wb> <player> <inCombo> <comboPiece> <prevPos> <visitedHex>\n");
        sb.append("# Regenerate with: --bench-gen\n");
        for (int i = 0; i < picked.size(); i++) {
            sb.append("# ").append(i).append(": ").append(notes.get(i)).append('\n');
            sb.append(picked.get(i)).append('\n');
        }
        try {
            Path p = Path.of(path);
            if (p.getParent() != null) Files.createDirectories(p.getParent());
            Files.writeString(p, sb.toString(), StandardCharsets.UTF_8);
            System.out.println("wrote " + path + " (" + picked.size() + " positions)");
        } catch (IOException e) {
            System.err.println("Cannot write " + path + ": " + e.getMessage());
        }
    }

    private static void add(List<String> picked, List<String> notes, GameState s, String note) {
        picked.add(GameStateCodec.toText(s));
        notes.add(note);
    }

    // ---------------------------------------------------------------- arg helpers

    private static String arg(String[] args, String name) {
        for (String a : args) {
            if (a.equals(name)) return "";
            if (a.startsWith(name + "=")) return a.substring(name.length() + 1);
        }
        return null;
    }

    private static String arg(String[] args, String name, String fallback) {
        String v = arg(args, name);
        return (v == null || v.isEmpty()) ? fallback : v;
    }
}
