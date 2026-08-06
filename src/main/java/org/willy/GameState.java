package org.willy;

class GameState {
    long myPieces, oppPieces;
    int player;
    boolean inCombo;
    int comboPiece = -1, prevPos = -1;
    long visitedMask = 0;
    int lastDir = -1;
    long zobristHash;

    public GameState copy() {
        GameState s = new GameState();
        s.myPieces = myPieces;
        s.oppPieces = oppPieces;
        s.player = player;
        s.inCombo = inCombo;
        s.comboPiece = comboPiece;
        s.prevPos = prevPos;
        s.visitedMask = visitedMask;
        s.lastDir = lastDir;
        s.zobristHash = zobristHash;
        return s;
    }

    public void initHash() {
        this.zobristHash = Zobrist.compute(this);
    }
}
