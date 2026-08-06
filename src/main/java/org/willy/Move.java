package org.willy;

import java.util.List;

class Move {
    int actionId, from, to;
    String type;
    List<Integer> victims;

    public Move(int a, int f, int t, String tp, List<Integer> v) {
        actionId = a;
        from = f;
        to = t;
        type = tp;
        victims = v;
    }
}
