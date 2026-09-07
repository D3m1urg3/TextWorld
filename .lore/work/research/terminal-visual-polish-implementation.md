---
title: "Implementing terminal visual polish: line editing, bars, streaming, and ASCII art"
date: 2026-09-07
status: active
tags: [ui, terminal, ansi-color, linenoise, line-editing, streaming, sse, libcurl, ascii-art, figlet, typography]
modules: [term, band, loop, main, aihttp, prose]
related: [.lore/work/brainstorm/terminal-visual-polish.md, .lore/work/research/terminal-ui-status-band.md, .lore/work/brainstorm/ui-improvements.md]
---

# Implementing terminal visual polish: line editing, bars, streaming, and ASCII art

Research for the items agreed in
[terminal-visual-polish](.lore/work/brainstorm/terminal-visual-polish.md).
Gathered 2026-09-07.

## Key Findings

1. **Upstream `antirez/linenoise` now has UTF-8 and an async API.** The reason
   the forks exist (`linenoise-ng`, `replxx`) was that the original had neither.
   That is no longer true. Upstream is BSD-2, one `.c` file, ~1600 lines, last
   pushed 2026-05-02, used in Redis and MongoDB. **Take upstream, not a fork.**
2. **Its multiplexing API is exactly what the spinner needs.**
   `linenoiseHide()` / `linenoiseShow()` exist to print asynchronous output
   without wrecking the line being edited. Line editing and the spinner are one
   job, not two.
3. **Background-color bars are width-safe, and I verified it locally.** A run of
   spaces wrapped in `\x1b[41m ... \x1b[0m` measures 6 code points after
   `stripSgr`. `layoutBand` already measures `text` and styles at emission, so a
   bar drops into a `BandSpan` with no arithmetic changes at all.
4. **The typography number is 66, not 72.** Bringhurst's 45-75 and the
   Tinker-Paterson eye-movement studies both land there. Above 80, readers lose
   the start of the next line.
5. **libcurl's write callback delivers arbitrary chunks, not lines.** SSE
   parsing needs its own buffer that splits on blank lines. This is the whole
   difficulty of streaming narration; the Anthropic event format itself is
   simple.
6. **FIGlet output should be pasted in as a string literal, not rendered at
   runtime.** No library, no font files, no dependency.
7. **Image-to-ASCII conversion sidesteps "models can't draw ASCII."** A
   luminance-ramp converter turns a picture into character art deterministically.
   That means art can come from pictures without a picture ever shipping.
8. **Tree-drawing characters `├── └── │` are forbidden here.** They are East
   Asian Ambiguous width, the exact class `band.hpp` already rules out. A map
   uses `+--`, `` `-- ``, and `|`, which is what `tree --charset=ascii` emits.

## 1. Line editing

Input today is `std::getline(std::cin, line)` at `main.cpp:154`. No history, no
arrow keys, no ctrl-a.

### Which library

| | Upstream linenoise | linenoise-ng | replxx | GNU readline |
|---|---|---|---|---|
| License | BSD-2 | BSD | BSD | **GPL** |
| Size | ~1600 lines, one `.c` | larger | larger | ~30k lines |
| UTF-8 | yes | yes | yes | yes |
| Async output API | **yes** | no | no | yes, awkward |
| Maintained | yes (2026-05) | forks, quiet | yes | yes |
| Syntax highlight / hints | hints only | no | yes | no |

GPL rules out readline. The forks existed to add UTF-8 and Windows to a version
that lacked both; upstream has UTF-8 now, and this project is POSIX-only. replxx
adds syntax highlighting, which this game has no use for.

**Upstream linenoise.** Vendor `linenoise.c` and `linenoise.h` next to
`sqlite3.c`, which is already vendored the same way.

### The API that matters

```c
char *linenoise(const char *prompt);              /* blocking, replaces getline */
void  linenoiseFree(void *ptr);                   /* it returns malloc'd memory */
int   linenoiseHistoryAdd(const char *line);
int   linenoiseHistorySetMaxLen(int len);
int   linenoiseHistorySave(const char *filename); /* -> next to world.db */
int   linenoiseHistoryLoad(const char *filename);

/* multiplexing — for the spinner */
int   linenoiseEditStart(struct linenoiseState *l, int in, int out,
                         char *buf, size_t buflen, const char *prompt);
char *linenoiseEditFeed(struct linenoiseState *l);
void  linenoiseEditStop(struct linenoiseState *l);
void  linenoiseHide(struct linenoiseState *l);
void  linenoiseShow(struct linenoiseState *l);
```

Notes for this codebase:

- `linenoise()` returns `malloc`'d memory; `linenoiseFree` it. Wrap in a
  `unique_ptr` with a custom deleter rather than sprinkling frees.
- It returns `NULL` on EOF, which maps onto the existing `if (eof) break`.
- Non-TTY input has no length limit and no editing, so piping a script into the
  game — which is how the validation captures in `.lore/work/validation/` were
  made — keeps working unchanged.
- History file: a `.textworld_history` beside `world.db`, loaded at startup and
  saved on quit. The log directory already establishes the "next to world.db"
  convention.
- Only a subset of VT100 escapes is used, ANSI.SYS compatible. It behaves under
  `TERM=dumb` (tested upstream against Emacs comint).

### Tab completion — still say no

`linenoiseSetCompletionCallback` is there and easy. It is still the wrong
feature: completing room nouns tells you what is present before you have looked.

## 2. Bars in the band

### It is width-safe, verified

```
printf 'HP \033[41m      \033[42m    \033[0m 10/12'
```

After `stripSgr`, that is `HP       ` plus `10/12` — the escape bytes vanish and
the spaces count as spaces. `BandSpan{text, color, bold, pad}` already keeps
plain text separate from styling, and `layoutBand` measures `text` only. So a
bar is a span whose `text` is N spaces. **No change to the layout arithmetic.**

The catch: `Color` in `term.hpp` is foreground-only, and `colorize()` emits
`3x`/`9x` codes. Bars need background codes (`4x`/`10x`). That is one more
function beside `colorize`/`bolden`/`boldColor`, not a redesign.

### Three ways to draw it

| Technique | Escape | Theme-safe | Can color-code | Verdict |
|---|---|---|---|---|
| Background color on spaces | `\x1b[41m` + spaces | yes, resolves through the user's 16-color theme | yes — red when low | **use this** |
| Reverse video | `\x1b[7m` + spaces, `\x1b[27m` | most theme-safe of all | no, it only inverts | fallback under `NO_COLOR` |
| ASCII fill characters | `[####....]` | always | yes | the `TERM=dumb` path |

That gives the same three-step degrade the telegraph already uses: color, then
bold or reverse, then plain characters that still read correctly.

Do not use `█` (U+2588) or `░` (U+2591). They are the obvious choice and they
are Ambiguous width — the same trap the earlier research caught with `─` and `·`.

## 3. Prose width

`loop.cpp:194` passes `detectWidth()` — the full terminal — into `wrapProse`.

The research is old and consistent: Bringhurst gives 45-75 characters for a
single column; Tinker and Paterson's 1929 and 1940 eye-movement studies found
45-75 minimizes regressions; 66 is the most-cited single number. Past about 80,
the eye misses the start of the next line and rereads or skips one.

So `std::min(w, 66)`, not 72. Keep the band at full width — it is a table, not
prose, and tables read fine wide.

## 4. Spinner during the blocking call

A turn is ~3.4 s of frozen terminal (~7.3 s when a room is generated).

Two ways:

**With linenoise's async API.** `linenoiseHide()`, print the spinner frame,
`linenoiseShow()`. Built for exactly this and it composes with the editing work.

**Without it.** A thread that writes `\r`, a frame character, then `\x1b[2K\r`
to erase. Simpler, but it fights whatever else writes to stdout.

Either way, gate on `currentStyle().attrs` — the flag that is already false for
a pipe and for `TERM=dumb`. That keeps the escape bytes out of the golden test
comparisons in `tests/tests.cpp`, the same way the color gating already does.

## 5. Streaming narration

### Anthropic's SSE format

Set `"stream": true`. Response is `content-type: text/event-stream`. Each event
is an `event:` line and a `data:` line; events are separated by a blank line.

Order for a plain text reply:

```
event: message_start
data: {"type":"message_start","message":{...,"content":[],...}}

event: content_block_start
data: {"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}}

event: ping
data: {"type":"ping"}

event: content_block_delta
data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hello"}}

event: content_block_stop
data: {"type":"content_block_stop","index":0}

event: message_delta
data: {"type":"message_delta","delta":{"stop_reason":"end_turn",...},"usage":{"output_tokens":15}}

event: message_stop
data: {"type":"message_stop"}
```

The only thing the narrator cares about is
`delta.type == "text_delta"` → append `delta.text`. Everything else is skipped.
`ping` events arrive at any point and mean nothing. An `error` event can arrive
mid-stream, after a 200 — so a stream can fail *after* text has already printed.

### The hard part is libcurl, not the format

`CURLOPT_WRITEFUNCTION` is called whenever bytes arrive — "one line, many lines,
or only a part of a single line." It has no concept of line or event boundaries.
So the callback must own a persistent buffer, append, scan for `\n\n`, and
process whole events only, leaving the remainder for next time. libcurl also
buffers up to 16K by default; `CURLOPT_BUFFERSIZE` makes the callback fire more
often but never sooner than the bytes arrive.

### What it breaks here

Streaming conflicts with three things this codebase currently relies on:

- **Wrapping.** `wrapProse` takes the whole string. Streaming means wrapping
  incrementally, holding the tail of a partial word until the next delta lands.
- **The fallback rule.** Today any failure silently falls back to templates. Once
  half a paragraph is on screen, there is nothing to fall back to. A mid-stream
  error needs a different answer, and there isn't one yet.
- **The profiler.** `twprof` records one duration per call. A streamed call has
  two useful numbers — time to first byte, and total.

Time-to-first-token is the whole point: it turns 3.4 s of nothing into ~0.5 s of
nothing followed by text arriving. But it is the largest item on the list and it
touches the fallback rule, which is a design decision, not a code change.

## 6. ASCII art

### The title screen

FIGlet (1991) renders text as block letters from `.flf` font files; 300+ fonts
exist. **Do not embed a renderer.** Run it once while authoring, paste the
output as a raw string literal in the seed data. Zero runtime dependency, zero
font files, and the art is reviewable in a diff.

Neither `figlet` nor `toilet` is installed on this machine — `brew install
figlet`, or any of the online generators, for the one-time render.

### Pictures into character art

`jp2a` and similar converters map pixel luminance
(`0.299R + 0.587G + 0.114B`) onto a character ramp — the classic one being
`" ...',;:clodxkO0KXNWM"`, darkest to lightest.

This matters more than it looks. The brainstorm rejected model-drawn ASCII art
because models cannot draw in ASCII, which is true. But a model *can* make a
picture, and a converter turns a picture into character art deterministically.
Two uses:

- **Now:** author the ten room-shape sketches from reference pictures instead of
  drawing them character by character. Much faster to produce, and consistent.
- **Later:** if image generation is ever turned on, the same pipeline produces
  terminal art with no image protocol and no per-terminal support question.

Quality note: converters do not interpolate when resizing, so scale the source
to the exact output size first. Monospace cells are about twice as tall as they
are wide, and the converter must compensate or everything comes out squashed.

### The map

`tree(1)`-style output is the right shape, because the world is a tree today. But
the standard glyphs `├── └── │` are Ambiguous width and are exactly what
`band.hpp` forbids. Use the ASCII set — `+--`, `` `-- ``, `|` — which is what
`tree --charset=ascii` produces. The algorithm is trivial: `+--` for a child,
`` `-- `` for the last child, `|` carried down while an ancestor still has
siblings.

## Sources

- [antirez/linenoise](https://github.com/antirez/linenoise) — README, `linenoise.h`, repo metadata
- [replxx](https://github.com/AmokHuginnsson/replxx), [linenoise-ng](https://github.com/arangodb/linenoise-ng)
- [Streaming messages — Claude Platform Docs](https://platform.claude.com/docs/en/build-with-claude/streaming)
- [CURLOPT_WRITEFUNCTION](https://curl.se/libcurl/c/CURLOPT_WRITEFUNCTION.html)
- [How streaming LLM APIs work — Simon Willison](https://til.simonwillison.net/llms/streaming-llm-apis)
- [Readability: The Optimal Line Length — Baymard](https://baymard.com/blog/line-length-readability)
- [Optimal Line Length for Readability — UXPin](https://www.uxpin.com/studio/blog/optimal-line-length-for-readability/)
- [jp2a man page](https://manpages.ubuntu.com/manpages/jammy/man1/jp2a.1.html), [Converting images to ASCII art](https://bitesofcode.wordpress.com/2017/01/19/converting-images-to-ascii-art-part-1/)
- [What is FIGlet — EZASCII](https://ezascii.com/blog/what-is-figlet-and-what-can-you-do-with-it)
- [Creating an ASCII-art tree in C#](https://andrewlock.net/creating-an-ascii-art-tree-in-csharp/)
- [Minimal bash progress bars with ANSI escapes](https://eskerda.com/minimal-bash-progress-bar-ansi-escape-sequences/)
