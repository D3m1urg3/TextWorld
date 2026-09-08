#!/usr/bin/env python3
"""Step 18: re-run each offline validation script and assert the THREE properties
REQ-POLISH-22 makes checkable — no escape bytes, the same sequence of turn
outcomes, and the same set of events rows. Layout is deliberately changed by this
spec, so the old capture is NOT diffed."""
import os, re, subprocess, sqlite3, sys

ROOT = "/Users/demiurge/Projects/TextWorld"
os.chdir(ROOT)

def run(binary, script, env_extra=None):
    if os.path.exists("world.db"):
        os.remove("world.db")
    env = dict(os.environ, TEXTWORLD_AI="0", COLUMNS="80")
    env.update(env_extra or {})
    with open(script, "rb") as f:
        p = subprocess.run([binary], input=f.read(), capture_output=True, env=env)
    rows = []
    if os.path.exists("world.db"):
        con = sqlite3.connect("world.db")
        rows = con.execute(
            "SELECT turn, verb, subject, object, detail FROM events ORDER BY id"
        ).fetchall()
        con.close()
    return p.stdout.decode("utf-8", "replace"), rows

def outcomes(text, script):
    """One outcome per scripted line, read off the output between prompts."""
    lines = [l for l in open(script).read().split("\n") if l.strip()]
    chunks = text.split("> ")
    out = []
    for chunk in chunks[1:]:
        body = chunk.strip()
        if not body:
            out.append("empty")
        elif "I don't understand that." in body:
            out.append("notick")
        else:
            out.append("ticked")
    return out[:len(lines)]

# (script, extra env, capture filename)
CASES = [
    (".lore/work/validation/background-room-pregeneration/ai-off-script.txt", {},
     ".lore/work/validation/background-room-pregeneration/polish-ai-off.txt"),
    (".lore/work/validation/background-room-pregeneration/ai-off-script.txt",
     {"TEXTWORLD_PREGEN": "0"},
     ".lore/work/validation/background-room-pregeneration/polish-ai-off-pregen0.txt"),
    # The script shared by turn-latency-polish and session-logging.
    (".lore/work/validation/turn-latency-polish/session.txt", {},
     ".lore/work/validation/turn-latency-polish/polish-session.txt"),
    (".lore/work/validation/turn-latency-polish/session.txt",
     {"TEXTWORLD_LOG_LEVEL": "debug", "TEXTWORLD_PROFILE": "1"},
     ".lore/work/validation/turn-latency-polish/polish-session-debug.txt"),
    (".lore/work/validation/turn-latency-polish/session-generate.txt", {},
     ".lore/work/validation/turn-latency-polish/polish-session-generate.txt"),
]

BASELINE = sys.argv[1] if len(sys.argv) > 1 else None
fail = 0
for script, extra, capture in CASES:
    label = os.path.basename(capture)
    new_out, new_rows = run("./build/textworld", script, extra)
    esc = new_out.count("\x1b")
    print(f"--- {label} ---")
    print(f"  escape bytes: {esc}")
    if esc:
        fail += 1
    if BASELINE:
        old_out, old_rows = run(BASELINE, script, extra)
        o_new, o_old = outcomes(new_out, script), outcomes(old_out, script)
        print(f"  outcome sequence: {'MATCH' if o_new == o_old else 'DIFFER'}  {o_new}")
        print(f"  events rows:      {'MATCH' if new_rows == old_rows else 'DIFFER'}"
              f"  ({len(new_rows)} rows)")
        if o_new != o_old or new_rows != old_rows:
            fail += 1
            for a, b in zip(old_rows, new_rows):
                if a != b:
                    print("    old:", a, "\n    new:", b)
    else:
        print(f"  events rows: {len(new_rows)}")
    with open(capture, "w") as f:
        f.write(new_out)
print("FAILURES:", fail)
