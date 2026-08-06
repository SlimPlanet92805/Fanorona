package org.willy;

/**
 * Reads and writes {@link GameState} in the two formats that live outside the
 * engine: the JSON the browser sends, and a one-line text form used by the
 * benchmark.
 * <p>
 * This is separate from {@link GameLogic} so the engine itself has no
 * dependency on a wire format — {@code GameLogic} transliterates to C++ as-is,
 * while the JSON half of this file simply doesn't come along.
 * <p>
 * Both formats carry the board from the <em>absolute</em> point of view
 * ({@code +1} / {@code -1}) and are split into the side-to-move-relative
 * {@code myPieces} / {@code oppPieces} pair on the way in.
 */
class GameStateCodec {

    /** Text form: {@code <45 board chars> <player> <inCombo> <comboPiece> <prevPos> <visitedHex>} */
    static final char EMPTY = '.', WHITE = 'w', BLACK = 'b';

    static GameState fromJson(JsonUtil.JsonObject j) {
        int[] arr = j.getIntArray("board");
        int player = j.getInt("player");
        long my = 0, opp = 0;
        for (int i = 0; i < GameLogic.NUM_POS; i++) {
            if (arr[i] == player) my |= (1L << i);
            else if (arr[i] != 0) opp |= (1L << i);
        }
        long visited = 0;
        if (j.has("visited")) for (int v : j.getIntArray("visited")) visited |= (1L << v);

        return build(my, opp, player,
                j.getBoolean("inCombo"),
                j.has("comboPiece") && !j.isNull("comboPiece") ? j.getInt("comboPiece") : -1,
                j.has("prevPos") && !j.isNull("prevPos") ? j.getInt("prevPos") : -1,
                visited);
    }

    static GameState fromText(String line) {
        String[] f = line.trim().split("\\s+");
        if (f.length < 6)
            throw new IllegalArgumentException("expected 6 fields, got " + f.length + ": " + line);
        String board = f[0];
        if (board.length() != GameLogic.NUM_POS)
            throw new IllegalArgumentException("board must be " + GameLogic.NUM_POS + " chars: " + board);

        int player = Integer.parseInt(f[1]);
        long my = 0, opp = 0;
        for (int i = 0; i < GameLogic.NUM_POS; i++) {
            char c = board.charAt(i);
            if (c == EMPTY) continue;
            int owner = (c == WHITE) ? 1 : -1;
            if (owner == player) my |= (1L << i);
            else opp |= (1L << i);
        }
        return build(my, opp, player,
                !"0".equals(f[2]),
                Integer.parseInt(f[3]),
                Integer.parseInt(f[4]),
                Long.parseUnsignedLong(f[5], 16));
    }

    static String toText(GameState s) {
        StringBuilder board = new StringBuilder(GameLogic.NUM_POS);
        for (int i = 0; i < GameLogic.NUM_POS; i++) {
            long bit = 1L << i;
            int owner = (s.myPieces & bit) != 0 ? s.player
                    : (s.oppPieces & bit) != 0 ? -s.player : 0;
            board.append(owner == 0 ? EMPTY : owner == 1 ? WHITE : BLACK);
        }
        return String.format("%s %d %d %d %d %x",
                board, s.player, s.inCombo ? 1 : 0, s.comboPiece, s.prevPos, s.visitedMask);
    }

    /** Shared tail of both parsers, including the derived {@code lastDir}. */
    private static GameState build(long my, long opp, int player, boolean inCombo,
                                   int comboPiece, int prevPos, long visitedMask) {
        GameState s = new GameState();
        s.myPieces = my;
        s.oppPieces = opp;
        s.player = player;
        s.inCombo = inCombo;
        s.comboPiece = comboPiece;
        s.prevPos = prevPos;
        s.visitedMask = visitedMask;
        if (inCombo && prevPos != -1 && comboPiece != -1)
            s.lastDir = GameLogic.deriveLastDir(prevPos, comboPiece);
        s.initHash();
        return s;
    }
}
