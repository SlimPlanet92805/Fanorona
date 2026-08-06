package org.willy;

import com.sun.net.httpserver.HttpExchange;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.*;

public class JsonUtil {
    static String movesToJson(List<Move> l) {
        StringBuilder b = new StringBuilder("[");
        for (int i = 0; i < l.size(); i++) {
            Move m = l.get(i);
            b.append(String.format("{\"action_id\":%d,\"from\":%d,\"to\":%d,\"type\":\"%s\",\"victims\":%s}", m.actionId, m.from, m.to, m.type, listToJson(m.victims)));
            if (i < l.size() - 1) b.append(",");
        }
        return b.append("]").toString();
    }

    static String arrayToJson(int[] a) {
        StringBuilder b = new StringBuilder("[");
        for (int i = 0; i < a.length; i++) {
            b.append(a[i]);
            if (i < a.length - 1) b.append(",");
        }
        return b.append("]").toString();
    }

    static String listToJson(Collection<Integer> c) {
        StringBuilder b = new StringBuilder("[");
        int i = 0;
        for (Integer v : c) {
            b.append(v);
            if (i++ < c.size() - 1) b.append(",");
        }
        return b.append("]").toString();
    }

    static String getBody(HttpExchange t) throws IOException {
        try (BufferedReader r = new BufferedReader(new InputStreamReader(t.getRequestBody(), StandardCharsets.UTF_8))) {
            StringBuilder b = new StringBuilder();
            String l;
            while ((l = r.readLine()) != null) b.append(l);
            return b.toString();
        }
    }

    static class JsonObject {
        Map<String, Object> m = new HashMap<>();

        int getInt(String k) {
            return ((Number) m.get(k)).intValue();
        }

        boolean getBoolean(String k) {
            return (Boolean) m.get(k);
        }

        int[] getIntArray(String k) {
            List<?> l = (List<?>) m.get(k);
            if (l == null) return new int[0];
            int[] a = new int[l.size()];
            for (int i = 0; i < l.size(); i++) {
                a[i] = ((Number) l.get(i)).intValue();
            }
            return a;
        }

        boolean has(String k) {
            return m.containsKey(k);
        }

        boolean isNull(String k) {
            return m.get(k) == null;
        }
    }

    static class JsonParser {
        static JsonObject parse(String j) {
            JsonObject o = new JsonObject();
            j = j.trim();
            if (j.startsWith("{")) j = j.substring(1, j.length() - 1);
            int i = 0;
            while (i < j.length()) {
                int col = j.indexOf(':', i);
                if (col == -1) break;
                String k = j.substring(i, col).trim().replace("\"", "");
                i = col + 1;
                while (i < j.length() && Character.isWhitespace(j.charAt(i))) i++;
                char c = j.charAt(i);
                if (c == '[') {
                    int end = match(j, i, '[', ']');
                    String as = j.substring(i + 1, end);
                    List<Object> l = new ArrayList<>();
                    if (!as.trim().isEmpty())
                        for (String p : as.split(",")) if (!p.trim().isEmpty()) l.add(Double.parseDouble(p.trim()));
                    o.m.put(k, l);
                    i = end + 1;
                } else if (c == 't') {
                    o.m.put(k, true);
                    i += 4;
                } else if (c == 'f') {
                    o.m.put(k, false);
                    i += 5;
                } else if (c == 'n') {
                    o.m.put(k, null);
                    i += 4;
                } else {
                    int com = j.indexOf(',', i);
                    if (com == -1) com = j.length();
                    String v = j.substring(i, com).trim();
                    if (v.startsWith("\"")) o.m.put(k, v.replace("\"", ""));
                    else o.m.put(k, Double.parseDouble(v));
                    i = com;
                }
                while (i < j.length() && (j.charAt(i) == ',' || Character.isWhitespace(j.charAt(i)))) i++;
            }
            return o;
        }

        static int match(String s, int st, char op, char cl) {
            int d = 0;
            for (int i = st; i < s.length(); i++) {
                if (s.charAt(i) == op) d++;
                if (s.charAt(i) == cl && --d == 0) return i;
            }
            return -1;
        }
    }
}
