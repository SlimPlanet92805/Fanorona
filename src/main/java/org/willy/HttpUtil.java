package org.willy;

import com.sun.net.httpserver.HttpExchange;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

public class HttpUtil {
    static void serveFile(HttpExchange t, String resourcePath, String contentType) throws IOException {
        try (InputStream is = FanoronaServer.class.getClassLoader().getResourceAsStream(resourcePath)) {
            if (is == null) {
                t.sendResponseHeaders(404, 0);
                t.close();
                return;
            }
            byte[] bytes = is.readAllBytes();
            t.getResponseHeaders().set("Content-Type", contentType);
            t.sendResponseHeaders(200, bytes.length);
            try (OutputStream os = t.getResponseBody()) {
                os.write(bytes);
            }
        }
    }

    static void sendJson(HttpExchange t, String j) throws IOException {
        byte[] b = j.getBytes(StandardCharsets.UTF_8);
        t.getResponseHeaders().set("Content-Type", "application/json");
        t.sendResponseHeaders(200, b.length);
        try (OutputStream os = t.getResponseBody()) {
            os.write(b);
        }
    }
}
