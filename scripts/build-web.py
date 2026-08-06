#!/usr/bin/env python3
"""Builds web/index.html — the whole game as one self-contained page.

Compiles the C++ engine to WebAssembly and stitches it into the same
src/main/resources/game.html the desktop build serves. There is deliberately no
second copy of the UI: game.html picks its transport at runtime, so the only
thing this script injects is a `window.FanoronaEngine` that routes the five
calls into the wasm module instead of over HTTP.

The module runs in a Web Worker. A search happily occupies a core for a full
second, and on the main thread that would freeze the page — no spinner, no
clicks, and a browser "page unresponsive" prompt on longer time controls.

No pthreads: GitHub Pages cannot send the COOP/COEP headers SharedArrayBuffer
needs, and the search does not require them. That is also why the wasm can be
inlined (-sSINGLE_FILE) and the result is one file you can open from disk.

Usage:
    # once, to get the toolchain
    git clone https://github.com/emscripten-core/emsdk
    ./emsdk/emsdk install latest && ./emsdk/emsdk activate latest

    python scripts/build-web.py
"""
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "engine" / "src"
OUT_DIR = ROOT / "web"
BUILD = ROOT / "engine" / "build" / "wasm"

SOURCES = ["board.cpp", "zobrist.cpp", "eval.cpp", "search.cpp",
           "codec.cpp", "trashtalk.cpp", "wasm_api.cpp"]

EXPORTED = ["_fan_init", "_fan_config", "_fan_restart", "_fan_get_state",
            "_fan_move", "_fan_ai", "_fan_memory_stats",
            "_fan_export", "_fan_export_ptr", "_fan_import", "_fan_clear",
            "_malloc", "_free"]

# 192 MB of linear memory: enough for a 64 MB table plus headroom, and small
# enough to allocate on a phone. The table is sized in fan_init.
TOTAL_MEMORY = 192 * 1024 * 1024


# em++ before emcc: the sources are C++, and newer Emscripten will not pull in
# libc++ when driven as emcc -- it fails at link with a wall of undefined
# std:: symbols. Older versions were more forgiving, so this only started
# breaking when the CI runner picked up a newer emsdk.
DRIVERS = ("em++.bat", "em++.exe", "em++", "emcc.bat", "emcc.exe", "emcc")


def find_emcc():
    for name in ("em++", "emcc"):
        if shutil.which(name):
            return name
    if os.environ.get("EMSDK"):
        for name in DRIVERS:
            cand = pathlib.Path(os.environ["EMSDK"]) / "upstream" / "emscripten" / name
            if cand.exists():
                return str(cand)
    roots = [ROOT.parent, ROOT.parent.parent, pathlib.Path.home(), pathlib.Path("C:/")]
    for base in roots:
        for name in DRIVERS:
            cand = base / "emsdk" / "upstream" / "emscripten" / name
            if cand.exists():
                return str(cand)
    sys.exit("em++/emcc not found — install emsdk (see the docstring)")


def compile_wasm(emcc):
    BUILD.mkdir(parents=True, exist_ok=True)
    out = BUILD / "fanorona.js"
    cmd = [
        emcc, "-O3", "-std=c++20",
        *[str(SRC / s) for s in SOURCES],
        "-I", str(SRC),
        "-o", str(out),
        "-sMODULARIZE=1",
        "-sEXPORT_NAME=createFanorona",
        "-sENVIRONMENT=worker",
        "-sSINGLE_FILE=1",            # inline the wasm so the page is one file
        "-sINITIAL_MEMORY=%d" % TOTAL_MEMORY,
        "-sALLOW_MEMORY_GROWTH=0",    # fixed heap; the table is the only big user
        "-sEXPORTED_FUNCTIONS=%s" % ",".join(EXPORTED),
        "-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToNewUTF8,HEAPU8",
        "-sFILESYSTEM=0",
        "--closure", "0",
    ]
    # Belt and braces: if only the C driver was found, say so explicitly rather
    # than letting the link fail on missing libc++.
    if pathlib.Path(emcc).name.startswith("emcc"):
        cmd.insert(1, "-sDEFAULT_TO_CXX=1")
    print("compiling wasm...")
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        sys.exit(f"emcc failed:\n{r.stdout}\n{r.stderr}")
    return out.read_text(encoding="utf-8")


# The worker: owns the module, answers one request at a time. Runs as a Blob so
# the page stays a single file.
WORKER_JS = r"""
// The learned transposition table, kept in IndexedDB across visits. The
// desktop build has always saved its table to disk; there is no reason the
// browser should start from nothing every refresh. Only the deepest entries
// are kept -- those cost the most to compute and are the ones worth carrying.
const DB_NAME = 'fanorona', STORE = 'memory', KEY = 'tt-v1';
const SAVE_ENTRIES = 200000;   // ~3 MB, loads in well under a second

// Whether any of the above happens at all. The user can turn it off, in which
// case the engine forgets everything the moment the tab closes -- which is
// exactly what someone wants who would rather play an opponent that has not
// been studying them.
let memoryEnabled = true;

function openDb() {
  return new Promise((resolve, reject) => {
    const rq = indexedDB.open(DB_NAME, 1);
    rq.onupgradeneeded = () => rq.result.createObjectStore(STORE);
    rq.onsuccess = () => resolve(rq.result);
    rq.onerror = () => reject(rq.error);
  });
}

async function loadMemory() {
  const db = await openDb();
  const blob = await new Promise((resolve, reject) => {
    const rq = db.transaction(STORE, 'readonly').objectStore(STORE).get(KEY);
    rq.onsuccess = () => resolve(rq.result);
    rq.onerror = () => reject(rq.error);
  });
  if (!blob) return 0;
  const bytes = new Uint8Array(blob);
  const M = self.Module;
  const ptr = M._malloc(bytes.length);
  M.HEAPU8.set(bytes, ptr);
  const n = M._fan_import(ptr, bytes.length);
  M._free(ptr);
  return n;
}

async function clearMemory() {
  self.Module._fan_clear();
  const db = await openDb();
  await new Promise((resolve, reject) => {
    const tx = db.transaction(STORE, 'readwrite');
    tx.objectStore(STORE).delete(KEY);
    tx.oncomplete = resolve;
    tx.onerror = () => reject(tx.error);
  });
}

async function saveMemory() {
  const M = self.Module;
  const len = M._fan_export(SAVE_ENTRIES);
  if (len <= 0) return 0;
  // Copy out of the wasm heap before handing it over: the buffer is reused.
  const bytes = M.HEAPU8.slice(M._fan_export_ptr(), M._fan_export_ptr() + len);
  const db = await openDb();
  await new Promise((resolve, reject) => {
    const tx = db.transaction(STORE, 'readwrite');
    tx.objectStore(STORE).put(bytes.buffer, KEY);
    tx.oncomplete = resolve;
    tx.onerror = () => reject(tx.error);
  });
  return len;
}

self.onmessage = async (ev) => {
  const msg = ev.data;
  if (msg.type === 'boot') {
    try {
      // eslint-disable-next-line no-eval
      (0, eval)(msg.runtime);
      self.Module = await createFanorona();
      const M = self.Module;
      self.api = {
        init: M.cwrap('fan_init', null, ['number', 'number', 'number']),
        config: M.cwrap('fan_config', 'string', ['string']),
        restart: M.cwrap('fan_restart', 'string', []),
        get_state: M.cwrap('fan_get_state', 'string', ['string']),
        move: M.cwrap('fan_move', 'string', ['string']),
        ai: M.cwrap('fan_ai', 'string', ['string']),
        memory_stats: M.cwrap('fan_memory_stats', 'string', [])
      };
      self.api.init(msg.timeMs, msg.hashMB, msg.english ? 1 : 0);
      memoryEnabled = msg.memory !== false;
      let restored = 0;
      // Never let a storage problem stop the game from starting.
      if (memoryEnabled) {
        try { restored = await loadMemory(); } catch (e) { restored = -1; }
      }
      self.postMessage({ id: msg.id, ok: true,
                         body: JSON.stringify({status: 'ready', restored: restored}) });
    } catch (e) {
      self.postMessage({ id: msg.id, ok: false, error: String(e) });
    }
    return;
  }
  if (msg.type === 'save') {
    let saved = 0, err = null;
    if (!memoryEnabled) {
      self.postMessage({ id: msg.id, ok: true, body: JSON.stringify({saved: 0}) });
      return;
    }
    try { saved = await saveMemory(); } catch (e) { saved = -1; err = String(e); }
    self.postMessage({ id: msg.id, ok: true,
                       body: JSON.stringify({saved: saved, error: err}) });
    return;
  }
  try {
    const a = self.api;
    let body;
    switch (msg.path) {
      case '/restart':      body = a.restart(); break;
      case '/get_state':    body = a.get_state(msg.body || '{}'); break;
      case '/move':         body = a.move(msg.body || '{}'); break;
      case '/ai':           body = a.ai(msg.body || '{}'); break;
      case '/memory_stats': body = a.memory_stats(); break;
      case '/clear_memory':
        // Both copies, or the next load would just put it all back.
        await clearMemory();
        body = '{"count": 0}';
        break;
      case '/config':
        // `memory` is the worker's setting, not the engine's -- the engine has
        // no idea it is being persisted -- so it is handled here and spliced
        // into the reply, which the UI reads back to render the checkbox.
        if (msg.body) {
          const m = /"memory"\s*:\s*(true|false|1|0)/.exec(msg.body);
          if (m) memoryEnabled = (m[1] === 'true' || m[1] === '1');
        }
        body = a.config(msg.body || '');
        body = body.replace(/}$/, ',"memory":' + (memoryEnabled ? 'true' : 'false') + '}');
        break;
      default:              body = '{"error":"not found"}';
    }
    self.postMessage({ id: msg.id, ok: true, body: body });
  } catch (e) {
    self.postMessage({ id: msg.id, ok: false, error: String(e) });
  }
};
"""

# The shim game.html talks to. Deliberately mimics fetch's shape so the UI code
# is identical in both builds.
SHIM_JS = r"""
(function () {
  const RUNTIME = window.__FANORONA_WASM_RUNTIME__;
  delete window.__FANORONA_WASM_RUNTIME__;

  const workerBlob = new Blob([window.__FANORONA_WORKER_SRC__], {type: 'text/javascript'});
  delete window.__FANORONA_WORKER_SRC__;
  const worker = new Worker(URL.createObjectURL(workerBlob));

  let nextId = 1;
  const pending = new Map();
  worker.onmessage = (ev) => {
    const {id, ok, body, error} = ev.data;
    const p = pending.get(id);
    if (!p) return;
    pending.delete(id);
    if (ok) p.resolve({ ok: true, status: 200, json: async () => JSON.parse(body) });
    else p.reject(new Error(error));
  };

  function send(payload) {
    return new Promise((resolve, reject) => {
      const id = nextId++;
      pending.set(id, {resolve, reject});
      worker.postMessage(Object.assign({id}, payload));
    });
  }

  // Boot from whatever the user last chose rather than from fixed defaults:
  // the worker starts before any UI code runs, and re-applying settings after
  // the fact would mean allocating the table twice and loading a memory the
  // user may have switched off. game.html writes this key; see saveSettings().
  let stored = {};
  try { stored = JSON.parse(localStorage.getItem('fanorona-settings') || '{}') || {}; }
  catch (e) { stored = {}; }
  const memoryOn = stored.memory !== false;

  const ready = send({
    type: 'boot', runtime: RUNTIME,
    timeMs: stored.time > 0 ? stored.time : 1000,
    hashMB: stored.hash > 0 ? Math.min(stored.hash, 128) : 64,
    english: stored.lang ? stored.lang === 'en'
                         : !/^zh\b/i.test(navigator.language || ''),
    memory: memoryOn
  });

  let saving = false;
  async function persist() {
    if (saving) return null;       // serialising twice at once helps nobody
    saving = true;
    try {
      const r = await send({type: 'save'});
      return await r.json();
    } catch (e) {
      return {error: String(e)};   // quota exceeded, private mode, no storage
    } finally {
      saving = false;
    }
  }

  // Save when the tab goes away rather than on a timer: the work is only worth
  // doing once per visit, and 'hidden' is the last event that reliably fires
  // on mobile, where tabs are killed without warning.
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'hidden') persist();
  });

  window.FanoronaEngine = {
    ready,
    persist,
    async call(path, opts) {
      await ready;
      const res = await send({path: path, body: opts && opts.body ? opts.body : ''});
      // A finished game is the other natural moment: that is when the table
      // holds everything it just learned.
      if (path === '/restart') persist();
      return res;
    }
  };
})();
"""

# data-i18n keys, so the banner follows the language switch like the rest of
# the interface. game.html owns the translations; this only tags the elements.
BANNER = """
<div id="wasm-banner">
  <span data-i18n="bannerText">Running entirely in your browser — the engine is compiled to WebAssembly, no server involved.</span>
  <a href="https://github.com/SlimPlanet92805/Fanorona" data-i18n="bannerLink">Source &amp; native build</a>
</div>
<style>
  #wasm-banner {
    width: 100%; box-sizing: border-box; padding: 8px 20px; text-align: center;
    background: #14301f; color: #8fdcae; font-size: 13px;
    border-bottom: 1px solid #24523a;
  }
  #wasm-banner a { color: #00e676; }
</style>
"""


def main():
    emcc = find_emcc()
    runtime = compile_wasm(emcc)

    html = (ROOT / "src" / "main" / "resources" / "game.html").read_text(encoding="utf-8")

    def js_literal(s):
        import json
        return json.dumps(s)

    injection = (
        "<script>window.__FANORONA_WASM_RUNTIME__ = " + js_literal(runtime) + ";\n"
        "window.__FANORONA_WORKER_SRC__ = " + js_literal(WORKER_JS) + ";</script>\n"
        "<script>" + SHIM_JS + "</script>\n"
    )

    # The shim must install window.FanoronaEngine before game.html's own script
    # runs, so it goes at the top of <body>, and the banner with it.
    marker = "<body>"
    if marker not in html:
        sys.exit("could not find <body> in game.html")
    html = html.replace(marker, marker + "\n" + injection + BANNER, 1)
    html = html.replace("<title>Fanorona</title>",
                        "<title>Fanorona — play the engine in your browser</title>", 1)

    OUT_DIR.mkdir(exist_ok=True)
    out = OUT_DIR / "index.html"
    out.write_text(html, encoding="utf-8")
    print(f"wrote {out} ({out.stat().st_size / 1024:.0f} KB)")


if __name__ == "__main__":
    main()
