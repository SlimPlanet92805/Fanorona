package org.willy;

import java.util.function.Consumer;

/**
 * Everything the search engine needs to know that isn't the position itself.
 * <p>
 * This exists so {@link AIPlayer} never reaches into {@link FanoronaServer}'s
 * mutable statics. That matters for two reasons beyond tidiness: benchmark runs
 * need a silent, fixed-depth, deterministic configuration that differs from the
 * server's, and the planned WebAssembly build has no stdout to log to at all.
 * <p>
 * Ports to a plain struct in C++.
 */
class SearchConfig {
    /** Wall-clock budget per move. Bench mode sets this effectively unlimited. */
    int timeLimitMs = 1000;

    /**
     * Hard cap on iterative-deepening depth. The default is high enough that
     * the time limit is what actually stops the search; bench mode lowers it to
     * get runs that are reproducible instead of machine-speed-dependent.
     */
    int maxDepth = 1000;

    /** Transposition-table entries kept across the periodic prune-and-save. */
    int maxMemoryEntries = 1_000_000;

    /** Where diagnostics go. Bench mode silences this. */
    Consumer<String> logger = System.out::println;

    void log(String msg) {
        if (logger != null) logger.accept(msg);
    }
}
