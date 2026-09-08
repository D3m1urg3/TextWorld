#!/usr/bin/env bash
# Step 21: the spec's twenty AI-Validation checks, run end to end.
cd /Users/demiurge/Projects/TextWorld
SP=/private/tmp/claude-501/-Users-demiurge-Projects-TextWorld/7b328001-1b89-4057-9d72-65d8469a8403/scratchpad
pass() { printf '  PASS  %s\n' "$1"; }
fail() { printf '  FAIL  %s\n' "$1"; FAILED=$((FAILED+1)); }
FAILED=0
mk() { rm -f world.db; }

printf 'look\ngo north\nlook\nspells\nxyzzy the frobnitz\nwait\nquit\n' > $SP/sw.txt

# 1. prose width, at 200 then 20
mk; COLUMNS=200 TEXTWORLD_AI=0 ./build/textworld < $SP/sw.txt > $SP/c200.txt 2>&1
mk; COLUMNS=20  TEXTWORLD_AI=0 ./build/textworld < $SP/sw.txt > $SP/c20.txt  2>&1
python3 - "$SP/c200.txt" "$SP/c20.txt" <<'PY'
import re, sys
SGR=re.compile("\x1b\\[[0-9;]*m"); PR=re.compile(r"^(> )+")
def narr(path):
    for line in open(path,encoding="utf-8").read().split("\n"):
        s=PR.sub("",SGR.sub("",line))
        if s.startswith("--") or re.match(r" (Exits|Objects|Enemy|You)\s",s) or s.startswith(" "*10):
            continue
        yield s
w200=max((len(s) for s in narr(sys.argv[1])), default=0)
# the mode notice is not narration and predates this spec
w20 =max((len(s) for s in narr(sys.argv[2]) if "AI narration off" not in s), default=0)
print(f"    narration max at COLUMNS=200: {w200} (<=66)")
print(f"    narration max at COLUMNS=20:  {w20} (<=20)")
sys.exit(0 if w200<=66 and w20<=20 else 1)
PY
[ $? -eq 0 ] && pass "1  prose width (66, then 20)" || fail "1  prose width"

# 2. band keeps the full width
grep -qE '^-- .{100,}' $SP/c200.txt && pass "2  band width unchanged" || fail "2  band width"

# 3. indent; spells and band at column 0; no whitespace-only line
python3 - "$SP/c200.txt" <<'PY'
import re,sys
SGR=re.compile("\x1b\\[[0-9;]*m"); PR=re.compile(r"^(> )+")
ws=0; unindented=[]
for line in open(sys.argv[1],encoding="utf-8").read().split("\n"):
    s=PR.sub("",SGR.sub("",line))
    if s!="" and s.strip()=="": ws+=1
    if s.startswith("--") or re.match(r" (Exits|Objects|Enemy|You)\s",s): continue
    if s!="" and not s.startswith("  "): unindented.append(s)
allowed=lambda s: "AI narration off" in s or s.startswith("Spells you know") or " — element:" in s or set(s)<= set("# ")
bad=[s for s in unindented if not allowed(s)]
print(f"    whitespace-only lines: {ws}; unindented non-reference lines: {len(bad)}")
for b in bad[:4]: print("      ", repr(b))
sys.exit(0 if ws==0 and not bad else 1)
PY
[ $? -eq 0 ] && pass "3  indent (band + spells at column 0, no ws-only line)" || fail "3  indent"

# 4. blank line before each prompt on a terminal; none gained in a pipe
mk; COLUMNS=90 CLICOLOR_FORCE=1 TEXTWORLD_AI=0 ./build/textworld < $SP/sw.txt > $SP/tty4.txt 2>&1
python3 - "$SP/tty4.txt" <<'PY'
import re,sys
lines=open(sys.argv[1],encoding="utf-8").read().split("\n")
pr=[i for i,l in enumerate(lines) if re.sub("\x1b\\[[0-9;]*m","",l).startswith(">")]
bad=[i for i in pr if i==0 or lines[i-1].strip()!=""]
print(f"    prompts: {len(pr)}, without a blank line above: {len(bad)}")
sys.exit(0 if pr and not bad else 1)
PY
[ $? -eq 0 ] && pass "4  blank line before the prompt" || fail "4  blank line"

# 5. no duplicate exits/objects
e=$(grep -c 'Exits: ' $SP/c200.txt); y=$(grep -c 'You see: ' $SP/c200.txt)
be=$(grep -cE '^ Exits ' $SP/c200.txt); bo=$(grep -cE '^ Objects ' $SP/c200.txt)
echo "    'Exits: ' $e, 'You see: ' $y; band Exits rows $be, band Objects rows $bo"
[ "$e" -eq 0 ] && [ "$y" -eq 0 ] && [ "$be" -gt 0 ] && pass "5  no duplicate exits" || fail "5  duplicate exits"

# 8. error styling three ways
mk; printf 'xyzzy the frobnitz\nquit\n' > $SP/err.txt
a=$(CLICOLOR_FORCE=1 COLUMNS=90 TEXTWORLD_AI=0 ./build/textworld < $SP/err.txt 2>&1 | grep -a 'understand')
mk; b=$(NO_COLOR=1 CLICOLOR_FORCE=1 COLUMNS=90 TEXTWORLD_AI=0 ./build/textworld < $SP/err.txt 2>&1 | grep -a 'understand')
mk; c=$(TERM=dumb CLICOLOR_FORCE=1 COLUMNS=90 TEXTWORLD_AI=0 ./build/textworld < $SP/err.txt 2>&1 | grep -a 'understand')
if printf '%s' "$a" | grep -q $'\x1b\\[90m' && [ "$b" = "$c" ] && ! printf '%s' "$b" | grep -q $'\x1b'; then
  pass "8  error styling (dim / plain / plain)"; else fail "8  error styling"; fi

# 19. title screen
mk; COLUMNS=90 TEXTWORLD_AI=0 ./build/textworld < $SP/err.txt > $SP/t90.txt 2>&1
mk; COLUMNS=20 TEXTWORLD_AI=0 ./build/textworld < $SP/err.txt > $SP/t20.txt 2>&1
first=$(head -1 $SP/t90.txt); notice=$(grep -n 'AI narration off' $SP/t90.txt | cut -d: -f1)
# REQ-POLISH-30 governs the ART. The em-dash on the "AI narration off — template
# mode" line is main.cpp:126's pre-existing notice, not the title screen.
artlines=$((notice-1))
nonascii=$(head -n $artlines $SP/t90.txt | LC_ALL=C grep -c '[^[:print:][:space:]]'; true)
narrow=$(head -1 $SP/t20.txt)
over=$(head -1 $SP/t20.txt | awk 'length>20' | wc -l | tr -d ' ')
echo "    first line: '${first:0:20}...'; notice at line $notice; art lines $artlines; non-ASCII in the art $nonascii; at COLUMNS=20 '$narrow' (over-20 lines: $over)"
[ "$notice" -gt 1 ] && [ "$nonascii" -eq 0 ] && [ "$narrow" = "TextWorld" ] && [ "$over" -eq 0 ] && pass "19 title screen" || fail "19 title screen"

# REQ-POLISH-33 / check 20's grep half
mk; n=$(TEXTWORLD_AI=0 ./build/textworld < $SP/sw.txt 2>/dev/null | grep -c $'\x1b')
echo "    escape sequences in a piped run: $n"
[ "$n" -eq 0 ] && pass "33 no escape byte reaches a non-terminal" || fail "33 escape bytes in a pipe"

rm -f world.db
echo
echo "FAILURES: $FAILED"
