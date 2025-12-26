package org.willy;

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpHandler;

import java.io.IOException;
import java.util.HashSet;
import java.util.Set;

class MoveHandler implements HttpHandler {
    public void handle(HttpExchange ex) throws IOException {
        String b = JsonUtil.getBody(ex);
        GameState s = GameLogic.fromJson(JsonUtil.JsonParser.parse(b));
        int aid = JsonUtil.JsonParser.parse(b).getInt("action_id");
        FanoronaServer.aiPlayer.analyzeHumanMove(aid);
        FanoronaServer.aiPlayer.recordState(s.zobristHash);
        GameLogic.StepResult res = GameLogic.step(s, aid);
        FanoronaServer.aiPlayer.recordState(res.state.zobristHash);
        int[] out = new int[GameLogic.NUM_POS];
        long my = res.state.myPieces, op = res.state.oppPieces;
        for (int i = 0; i < GameLogic.NUM_POS; i++) {
            if ((my & (1L << i)) != 0) out[i] = res.state.player;
            else if ((op & (1L << i)) != 0) out[i] = -res.state.player;
        }
        Set<Integer> v = new HashSet<>();
        long vm = res.state.visitedMask;
        while (vm != 0) {
            v.add(Long.numberOfTrailingZeros(vm));
            vm &= (vm - 1);
        }
        HttpUtil.sendJson(ex, String.format("{\"board\":%s,\"player\":%d,\"inCombo\":%b,\"comboPiece\":%d,\"prevPos\":%d,\"win\":%b,\"visited\":%s}", JsonUtil.arrayToJson(out), res.state.player, res.state.inCombo, res.state.comboPiece, res.state.prevPos, res.win, JsonUtil.listToJson(v)));
    }
}
