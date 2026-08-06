#!/usr/bin/env python3
"""Builds docs/demo.gif — the animated board shown at the top of the README.

Not part of the app and not needed to build or run it; kept here so the GIF
can be regenerated after UI changes instead of being an undocumented binary
nobody wants to touch.

Unlike a scripted/faked recording, this drives a *real* running
FanoronaServer through an actual browser (Playwright + Chromium): every move
shown, every AI log line, and every trash-talk line in the GIF is the real
engine thinking against itself, not seeded text.

The human side is played by a second, deliberately weakened instance of the
same engine rather than by a greedy heuristic. A greedy capture-maximising
player walks into every trap and gets mated in three or four moves, which
makes for an accurate but unwatchable demo -- the board never develops. A weak
engine loses properly instead: slowly, over a real game.

Usage:
    # the opponent shown in the GIF
    engine/build/fanorona --time=600 --threads=8 --lang=en --no-browser \
        --html=src/main/resources/game.html &
    # the sparring partner playing the human side
    engine/build/fanorona --port=8081 --time=40 --depth=3 --threads=1 \
        --hash=16 --no-browser --html=src/main/resources/game.html &
    python -m pip install playwright && python -m playwright install chromium
    python scripts/make-demo-gif.py

Works against the Java server too -- same endpoints -- but the C++ engine is
what ships, so that is what the demo should show.

Records a full game (until someone wins or MAX_TURNS is hit as a safety
cap), so the GIF isn't just an opening fragment.
"""
import json
import pathlib
import sys
import urllib.request

from PIL import Image
from playwright.sync_api import sync_playwright

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "demo.gif"
URL = "http://localhost:8080/"
# Set to "" to play the human side with the greedy fallback instead.
SPARRING_URL = "http://localhost:8081"

WIDTH, HEIGHT = 1300, 1150
GIF_WIDTH = 1100
MAX_TURNS = 150  # safety cap; a real game usually ends well before this
MAX_COMBO_STEPS = 12  # safety cap per turn
STOP_ACTION = 720  # voluntarily ending a capture chain


def wait_for_status(page, contains, timeout=15000):
    page.wait_for_function(
        "t => document.getElementById('status-text').innerText.includes(t)",
        arg=contains, timeout=timeout,
    )


def greedy_move(page):
    """Fallback: the capture with the most victims, else any non-stop move."""
    return page.evaluate("""() => {
        if (typeof gameMoves === 'undefined' || !gameMoves.length) return null;
        const captures = gameMoves.filter(m => m.type !== 'stop' && m.victims && m.victims.length > 0);
        if (captures.length) {
            return captures.reduce((a, b) => (b.victims.length > a.victims.length ? b : a));
        }
        return gameMoves.find(m => m.type !== 'stop') || gameMoves[0];
    }""")


def sparring_move(page):
    """Asks the weak engine on SPARRING_URL what the human side should play."""
    state = page.evaluate("""() => ({
        board: boardState, player: currentPlayer,
        inCombo: comboInfo.inCombo, comboPiece: comboInfo.comboPiece,
        prevPos: comboInfo.prevPos, visited: comboInfo.visited
    })""")
    req = urllib.request.Request(
        SPARRING_URL + "/ai",
        data=json.dumps(state).encode(),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=20) as r:
        action = json.loads(r.read().decode())["action_id"]
    return page.evaluate("a => gameMoves.find(m => m.action_id === a) || null", action)


def pick_move(page):
    if SPARRING_URL:
        try:
            m = sparring_move(page)
            if m:
                return m
        except Exception as e:
            print(f"  (sparring engine unavailable, falling back to greedy: {e})")
    return greedy_move(page)


def click_move(page, move):
    # Ending a capture chain voluntarily is a real move, but it has no board
    # square: its `from` is -1 and it is played through the Stop Combo button.
    if move["type"] == "stop" or move["action_id"] == STOP_ACTION:
        page.click("#stop-btn")
        return
    # Mid-combo the UI has already selected the moving piece for us; clicking it
    # again is unnecessary and the element may not carry a click handler.
    if page.evaluate("() => selectedPieceIndex") != move["from"]:
        page.click(f"#piece-{move['from']}")
    dest = page.evaluate("i => getCoord(i)", move["to"])
    box = page.query_selector("#board-wrap").bounding_box()
    page.mouse.click(box["x"] + dest["x"], box["y"] + dest["y"])


def play_human_turn(page, frames, durations):
    """Plays out one full human turn — including any capture combo chain — greedily
    preferring the biggest capture at each step, and snapshots after every ply."""
    played = False
    for _ in range(MAX_COMBO_STEPS):
        try:
            # Wait for our turn specifically, not merely for a populated move
            # list: the list is also set while the AI is thinking, and the
            # engine now answers fast enough that clicking then hits a piece
            # the UI has already disabled.
            #
            # The board is ready when a selectable source piece exists -- or
            # when we are mid-combo, where the UI has already selected the
            # capturing piece and renders destination dots instead, so no
            # `.source` element is present.
            page.wait_for_function(
                "() => typeof gameMoves !== 'undefined' && gameMoves.length > 0"
                " && currentPlayer === mySide && !isGameOver"
                " && (document.querySelector('.piece.source') !== null"
                "     || comboInfo.inCombo)",
                timeout=15000)
        except Exception:
            break
        move = pick_move(page)
        if not move:
            break
        stopping = move["type"] == "stop" or move["action_id"] == STOP_ACTION
        click_move(page, move)
        played = True
        # Long enough for the slide (340ms) plus the capture fade (260ms), so
        # the snapshot lands after the taken pieces have gone rather than
        # freezing mid-dissolve.
        page.wait_for_timeout(680)
        # Ending the chain has to be *played*, not just skipped: bailing out of
        # the loop here would leave the game stuck mid-combo, waiting forever
        # for a turn that never ends.
        if stopping:
            break
        shot(page, frames, durations, 750)
        if not page.evaluate("() => comboInfo.inCombo && currentPlayer === mySide"):
            break
    return played


def main():
    if not OUT.parent.exists():
        OUT.parent.mkdir(parents=True)
    frames = []
    durations = []
    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(viewport={"width": WIDTH, "height": HEIGHT})
        try:
            page.goto(URL, wait_until="networkidle")
        except Exception as e:
            sys.exit(f"could not reach {URL} — is FanoronaServer running? ({e})")

        wait_for_status(page, "Turn:")
        shot(page, frames, durations, 1200)

        for turn in range(MAX_TURNS):
            if page.evaluate("() => isGameOver"):
                break
            if not play_human_turn(page, frames, durations):
                break
            page.wait_for_timeout(500)  # let status flip to "AI is thinking..."
            if page.evaluate("() => isGameOver"):
                shot(page, frames, durations, 2200)
                break
            try:
                page.wait_for_function(
                    "() => isGameOver || document.getElementById('status-text').innerText.includes('Turn:')",
                    timeout=20000)
            except Exception:
                break
            if page.evaluate("() => isGameOver"):
                page.wait_for_timeout(400)
                shot(page, frames, durations, 2200)
                break
            page.wait_for_timeout(350)
            shot(page, frames, durations, 900)
            print(f"  turn {turn + 1} recorded ({len(frames)} frames so far)")

        page.wait_for_timeout(400)
        shot(page, frames, durations, 2500)  # hold on the final game-over frame
        browser.close()

    if not frames:
        sys.exit("captured no frames")
    frames[0].save(OUT, save_all=True, append_images=frames[1:],
                    duration=durations, loop=0, optimize=True, disposal=2)
    print(f"wrote {OUT} ({OUT.stat().st_size / 1024:.0f} KB, {len(frames)} frames)")


def shot(page, frames, durations, hold_ms):
    png = page.screenshot()
    import io
    img = Image.open(io.BytesIO(png)).convert("RGB")
    img = img.resize((GIF_WIDTH, round(img.height * GIF_WIDTH / img.width)), Image.LANCZOS)
    frames.append(img.convert("P", palette=Image.ADAPTIVE, colors=160))
    durations.append(hold_ms)


if __name__ == "__main__":
    main()
