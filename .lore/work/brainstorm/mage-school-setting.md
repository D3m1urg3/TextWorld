---
title: Mage-school setting and initial rooms
date: 2026-07-09
status: open
tags: [setting, seed, worldbuilding, mage-school, initial-rooms]
modules: [seed, architect]
related: [.lore/work/brainstorm/ai-integration-points.md]
---

# Mage-school setting and initial rooms

Replace the Elwyn Priory default world with a Hogwarts-inspired (but original)
mage school. Scope of this brainstorm: **setting and initial rooms only** —
`seed/setting.txt` and `seed/base.sql`. Gameplay (spells, learning, NPCs) is
deliberately deferred to a future session.

## Why a mage school fits this engine

- **Spells are words.** The AI resolver already interprets arbitrary
  utterances as actions, so incantations need no new verb machinery. (Deferred,
  but it's the long-term payoff that motivates the reskin.)
- **A magical school is the perfect alibi for AI-generated geography.** The
  architect grows rooms at unmapped exits, which risks incoherent layouts. In a
  school where staircases move and wings were added by quarreling headmasters,
  incoherence is lore-accurate. The engine's weakness becomes a setting feature.
- **Strong generative grammar.** Classrooms, towers, libraries, dormitories,
  undercrofts, hidden passages — the school concept gives the architect a rich,
  bounded vocabulary to riff on.

## Decisions made

| Question | Decision | Why |
|----------|----------|-----|
| Why is the school empty? | **Night** — everyone is asleep, the player is out of bed after curfew | Keeps the NPC-free constraint natural; adds free low-stakes tension; self-motivates exploration. Ages well: NPCs can later arrive one at a time (caretaker, ghost, another sleepless student), or the sun eventually rises as a content update. |
| Who is the player? | **New student, first night** — arrived today, can't sleep | Explains why the player doesn't know the geography, mirroring the fact that the geography doesn't exist until the architect generates it. Same alibi trick as the moving staircases. |
| Anchoring image | **Manor–castle mix** — a fortified manor: castle bones domesticated by generations of headmasters | Gives the architect both vocabularies (towers, undercrofts, battlements + studies, galleries, window seats, kitchens). The tension *is* the tone: a serious old fortress gone soft and scholarly. Battlements with laundry lines, a gatehouse turned greenhouse, arrow slits reglazed as reading nooks. |

## Setting.txt considerations

- It is the architect's DNA — every generated room inherits from it. Deserves
  as much care as any mechanic.
- Needs a **name and light cosmology** (two sentences suffice) so the architect
  reuses proper nouns and generated rooms feel like one place, not generic
  fantasy.
- Tone flips from the Priory's "no magic beyond ordinary strangeness" to:
  **magic is ambient and domestic** — self-lighting candles, restless books,
  doors with opinions — not spectacular. Wonder over danger. Rooms stay
  human-scale. Constraint list is what keeps generated rooms coherent.
- Must not promise people: prose should make the night-emptiness natural or
  the architect will generate bustling rooms the engine can't deliver.
- IP note: Hogwarts-*inspired* vibe, but original names, original school.

## Seed rooms (base.sql shape)

Keep the current two-room / two-item / one-exit-pair shape:

- **Room 1 (start): the player's dormitory cell / student's chamber.**
  Enclosed, safe, one obvious exit. Personal scale — "you live here (as of
  this morning)."
- **Room 2: a corridor.** Boring on paper, but structurally the best
  architect-bait: neutral, exits in every direction invite generation.
  (Library and common room were considered — too destination-flavored for a
  seed room.)
- **Items:**
  - *Candle* (lantern analog) — light source, domestic magic flavor.
  - *Rusty key* — "cut for a lock you have not found" works even better in a
    school full of locked doors.
  - *Wand* — pure flavor for now, deliberately inert; becomes the spell hook
    in a future gameplay session. Even doing nothing, it sets the wizard
    identity.

## Deferred (future sessions)

- **Spell system**: spells as canon knowledge (`known_spells` component);
  the constraint is what makes a spell different from a wish. Spells map to
  mutation categories. Incantations discoverable in world prose. Misfires as
  controlled chaos.
- **NPCs**: confirmed as a planned future feature. Night setting is designed
  to absorb them gradually.

## Open questions

- Exact school name and two-sentence cosmology (to be drafted with the
  setting.txt rewrite).
- Whether the wand starts in the cell (on the player) or is found in the
  corridor.
- Does the old Priory seed survive anywhere (alternate setting file?) or is
  it simply replaced?
