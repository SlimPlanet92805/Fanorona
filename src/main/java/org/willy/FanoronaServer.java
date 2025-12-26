package org.willy;

import com.sun.net.httpserver.HttpServer;

import java.awt.*;
import java.net.BindException;
import java.net.InetSocketAddress;
import java.net.URI;
import java.util.concurrent.Executors;

public class FanoronaServer {

    private static final int PORT = 8080;
    static final String MEMORY_FILE = "fanorona_memory.dat";
    private static final String HTML_FILE = "game.html";

    static boolean HIDE_DETAILED_LOG = true;

    // --- Memory Constraints ---
    static int MAX_MEMORY_ENTRIES = 1000000;

    static final int INF = 100000000;
    static final int MATE_SCORE = 90000000;
    static final int MATE_THRESHOLD = 80000000;
    static int MAX_DEPTH = 1000;
    static int Time_LIMIT = 1000;

    static final AIPlayer aiPlayer = new AIPlayer();

    public static void main(String[] args) {
        for (String arg : args) {
            if (arg.startsWith("--depth=")) MAX_DEPTH = Integer.parseInt(arg.split("=")[1]);
            if (arg.startsWith("--time=")) Time_LIMIT = Integer.parseInt(arg.split("=")[1]);
            if (arg.startsWith("--mem=")) MAX_MEMORY_ENTRIES = Integer.parseInt(arg.split("=")[1]);
            if (arg.startsWith("--debug")) HIDE_DETAILED_LOG = false;
        }

        try {
            System.out.println("--- Fanorona Server ---");
            GameLogic.initTables();
            Zobrist.init();

            aiPlayer.loadMemory(MEMORY_FILE);

            Runtime.getRuntime().addShutdownHook(new Thread(() -> {
                System.out.println("\n🛑 System shutting down. Pruning and saving memory...");
                aiPlayer.saveMemory(MEMORY_FILE);
            }));

            AIPlayer.setupPersistence();

            HttpServer server = HttpServer.create(new InetSocketAddress(PORT), 0);
            server.createContext("/", ex -> HttpUtil.serveFile(ex, HTML_FILE, "text/html"));
            server.createContext("/restart", ex -> {
                aiPlayer.resetGame();
                HttpUtil.sendJson(ex, "{\"status\": \"ok\"}");
            });
            server.createContext("/get_state", ex -> {
                String b = JsonUtil.getBody(ex);
                GameState state = GameLogic.fromJson(JsonUtil.JsonParser.parse(b));
                HttpUtil.sendJson(ex, "{\"moves\": " + JsonUtil.movesToJson(GameLogic.getDetailedMoves(state)) + "}");
            });
            server.createContext("/move", new MoveHandler());
            server.createContext("/ai", ex -> {
                try {
                    String b = JsonUtil.getBody(ex);
                    GameState state = GameLogic.fromJson(JsonUtil.JsonParser.parse(b));
                    AIPlayer.AIResult res = aiPlayer.think(state);

                    String displayPv = res.pv;
                    String safePv = displayPv.replace("\"", "'").replace("\n", " ");

                    String json = String.format(
                            "{\"action_id\": %d, \"score\": %d, \"strategy\": \"%s\", \"pv\": \"%s\"}",
                            res.bestMove, res.score, res.strategy, safePv);

                    HttpUtil.sendJson(ex, json);
                } catch (Exception e) {
                    e.printStackTrace();
                    HttpUtil.sendJson(ex, "{\"error\":\"AI Logic Error\"}");
                }
            });
            server.createContext("/memory_stats", ex -> {
                try {
                    String json = String.format("{\"count\": %d}", aiPlayer.getMemorySize());
                    HttpUtil.sendJson(ex, json);
                } catch (Exception e) {
                    HttpUtil.sendJson(ex, "{\"count\": 0}");
                }
            });

            server.setExecutor(Executors.newCachedThreadPool());
            server.start();
            System.out.println("🚀 Server started at: http://localhost:" + PORT);
            if (Desktop.isDesktopSupported() && Desktop.getDesktop().isSupported(Desktop.Action.BROWSE)) {
                System.out.println("🌍 Opening browser...");
                Desktop.getDesktop().browse(new URI("http://localhost:8080"));
            }
        } catch (BindException e) {
            System.err.println("❌ Port occupied. Please close the previous process.");
        } catch (Exception e) {
            e.printStackTrace();
        }
    }
}