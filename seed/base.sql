-- TextWorld base seed: the two-room prototype world.
-- Literal entity ids, per design §8. Id ledger:
--   1: room 'dormitory cell'
--   2: room 'corridor'
--   3: player
--   4: portable 'candle'
--   5: portable 'key'
--   6: portable 'wand'
--   7: hostile 'goblin grunt' (in the corridor)

INSERT INTO entities(id) VALUES (1), (2), (3), (4), (5), (6), (7);

-- 1: room 'dormitory cell'
INSERT INTO room(entity) VALUES (1);
INSERT INTO name(entity, value) VALUES (1, 'dormitory cell');
INSERT INTO description(entity, prose) VALUES (1,
  'A narrow student''s cell under a sloped ceiling: a bed with unfamiliar sheets, a desk, a trunk you have not finished unpacking. Moonlight through the single lancet window finds the door to the north, standing just ajar.');

-- 2: room 'corridor'
INSERT INTO room(entity) VALUES (2);
INSERT INTO name(entity, value) VALUES (2, 'corridor');
INSERT INTO description(entity, prose) VALUES (2,
  'A long panelled corridor, doors shut on either side and the ceiling lost in the dark. Somewhere far off a stair creaks to itself. A lamp in a wall bracket kindles quietly as you approach, and the way south leads back to your cell.');

-- exits: north 1→2, south 2→1 (realized cell↔corridor pair, unchanged)
INSERT INTO exits(room, direction, dest) VALUES
  (1, 'north', 2),
  (2, 'south', 1);

-- Latent frontier (REQ-EXITS-9): NULL dest = an open direction whose room is
-- not generated yet; walking it triggers generation. Two invertible directions
-- off the corridor, coherent with Thornmere Hall: on north to an unseen stair
-- or landing, and up to the sleeping floor above.
INSERT INTO exits(room, direction, dest) VALUES
  (2, 'north', NULL),
  (2, 'up', NULL);

-- 3: player, out of bed in the dormitory cell (no description: the player is not canon prose)
INSERT INTO player(entity) VALUES (3);
INSERT INTO name(entity, value) VALUES (3, 'player');
INSERT INTO location(entity, container) VALUES (3, 1);
-- The player carries health like every combatant (REQ-COMBAT-4). Seed canon.
INSERT INTO health(entity, current, max) VALUES (3, 12, 12);
-- A student starts knowing the two basic counters, so the telegraph/counter
-- puzzle is exercisable from a fresh world (REQ-COMBAT-7). Learned = canon.
INSERT INTO known_spells(entity, spell) VALUES (3, 'ward'), (3, 'stun');

-- 4: portable 'candle', in the dormitory cell
INSERT INTO portable(entity) VALUES (4);
INSERT INTO name(entity, value) VALUES (4, 'candle');
INSERT INTO description(entity, prose) VALUES (4,
  'A stub of white candle in a pewter holder, burning with a small, patient flame that never seems to shorten it.');
INSERT INTO location(entity, container) VALUES (4, 1);

-- 5: portable 'key', in the corridor
INSERT INTO portable(entity) VALUES (5);
INSERT INTO name(entity, value) VALUES (5, 'key');
INSERT INTO description(entity, prose) VALUES (5,
  'A cold iron key on a loop of faded ribbon, heavy as a promise, its teeth cut for a lock you have not found.');
INSERT INTO location(entity, container) VALUES (5, 2);

-- 6: portable 'wand', on the desk in the dormitory cell
INSERT INTO portable(entity) VALUES (6);
INSERT INTO name(entity, value) VALUES (6, 'wand');
INSERT INTO description(entity, prose) VALUES (6,
  'A wand of pale ashwood, smooth where other hands once held it, left on the desk as though someone knew you were coming.');
INSERT INTO location(entity, container) VALUES (6, 1);

-- The bestiary catalog: one frozen record per archetype — the mold every
-- instance is cast from (REQ-COMBAT-28, -29, -30). The engine owns every number;
-- the model (via the architect's eligible menu, Brick 4) sees only `blurb` and
-- selects an archetype by name, never a stat. This shipped world hand-places just
-- the goblin (REQ-COMBAT-39); the other archetypes are the mold the architect
-- casts from as the invasion grows. Numbers here are the single source of truth —
-- the goblin instance below COPIES them rather than restating literals.
INSERT INTO bestiary(archetype, name, blurb, health, chip, telegraph_period, tier, barrier) VALUES
  ('goblin_grunt', 'goblin grunt',
   'a scrawny goblin raider up from the breached lower halls, quick and lightly armored', 8, 1, 2, 1, 0),
  ('rime_touched', 'rime-touched goblin',
   'a goblin sheathed in creeping frost, cold to approach and grudging to burn', 10, 1, 3, 1, 0),
  ('ironhide', 'ironhide brute',
   'a brute plated in warded iron that turns aside every blow until its ward is broken', 14, 1, 4, 2, 1),
  ('book_swarm', 'snapping folio',
   'a loose flock of snapping folios, flimsy alone but dangerous in a swarm', 10, 1, 0, 1, 0);

-- The fixed archetype → grimoire-spell drop map (REQ-COMBAT-20): defeating an
-- archetype drops a grimoire teaching this spell. Read by the eligibility menu
-- (Brick 4) — an archetype can be offered when it drops a key the player lacks.
INSERT INTO drop_table(archetype, spell) VALUES
  ('goblin_grunt', 'fire'),
  ('rime_touched', 'frost'),
  ('ironhide', 'dispel'),
  ('book_swarm', 'blast');

-- 7: hostile 'goblin grunt', hand-placed in the corridor (REQ-COMBAT-39). The
-- tutorial lock: low health, basic-attack-soluble, a small untelegraphed chip
-- clock. Its stats are CAST FROM the bestiary (micro-decision 4) — the name and
-- numbers are copied from the catalog row, so a seed instance can never drift
-- from its archetype. Only the entity id, canon prose, and location are placed
-- by hand here.
INSERT INTO hostile(entity, archetype, chip, telegraph_period)
  SELECT 7, archetype, chip, telegraph_period FROM bestiary WHERE archetype = 'goblin_grunt';
INSERT INTO health(entity, current, max)
  SELECT 7, health, health FROM bestiary WHERE archetype = 'goblin_grunt';
INSERT INTO name(entity, value)
  SELECT 7, name FROM bestiary WHERE archetype = 'goblin_grunt';
INSERT INTO description(entity, prose) VALUES (7,
  'A scrawny goblin in stolen leathers, one of the invaders come up from the breached lower halls. It bares its teeth and shifts its weight, watching for an opening.');
INSERT INTO location(entity, container) VALUES (7, 2);

-- Spell catalog: engine-owned constants (REQ-COMBAT-13, -14). Keys are the
-- lowercase words the parser/resolver lower to. Ward blocks a telegraphed
-- strike; Stun interrupts it. Elemental keys (Fire/Frost/Dispel/AoE) arrive in
-- Brick 3. cooldown measured on the global tick clock; never reduced.
INSERT INTO spell_catalog(spell, element, cooldown, tier, effect) VALUES
  ('ward', NULL, 2, 1, 'ward'),
  ('stun', NULL, 3, 1, 'stun'),
  ('fire', 'fire', 2, 1, 'damage'),      -- elemental damage (element lock key)
  ('frost', 'frost', 2, 1, 'frost'),     -- elemental damage + a slow (CC)
  ('dispel', NULL, 3, 2, 'dispel'),      -- strips a barrier (defense lock key)
  ('ember', 'fire', 3, 1, 'dot'),        -- damage-over-time (a multiplicity key)
  ('blast', NULL, 3, 2, 'aoe');          -- area damage + DoT to all (multiplicity key)

-- Resistance table: integer ratios (num/den), no floats, no RNG. A missing
-- (archetype, element) row means neutral (1/1). The rime-touched archetype is
-- the element lock: weak to Fire (2x), shrugs off Frost (1/2x). Its INSTANCE is
-- placed in Step 14; the archetype-level ratios live here.
INSERT INTO resistance(archetype, element, multiplier_num, multiplier_den) VALUES
  ('rime_touched', 'fire', 2, 1),
  ('rime_touched', 'frost', 1, 2);

-- The closed motive vocabulary (REQ-BARD-STORE-5), engine-owned constants like
-- spell_catalog and bestiary: seeded here, never written at runtime. The model
-- sees `blurb`, never the key, and writeCatalogEntry throws on a motive absent
-- from this table — so the bard cannot invent a ninth.
--
-- AUTHORED CONTENT, PROVISIONAL pending author approval. Changing the vocabulary
-- is this one edit plus a world-file delete; nothing in code names these keys.
INSERT INTO motive_catalog(motive, blurb) VALUES
  ('curiosity',    'wants to know something they have not been told'),
  ('secrecy',      'has something to keep hidden, and is arranging for it to stay that way'),
  ('rivalry',      'wants to be first, or to be seen to be first'),
  ('obligation',   'is bound by a duty they did not choose'),
  ('grief',        'is holding on to someone or something already gone'),
  ('appetite',     'wants to take and carry off'),
  ('pride',        'would rather be wrong than corrected'),
  ('homesickness', 'does not belong here yet, and feels it');

-- The closed condition vocabulary (REQ-ARC-STORE-5), engine-owned constants
-- like spell_catalog, bestiary and motive_catalog: seeded here, never written at
-- runtime. writeStoryStep throws on a kind absent from this table, so a story
-- step cannot promise a condition the engine has no way to check. The model
-- sees `blurb`, never the key; `arg_kind` says what condition_arg may hold.
--
-- AUTHORED CONTENT, PROVISIONAL pending author approval. Changing the
-- vocabulary is this one edit plus a world-file delete.
INSERT INTO condition_catalog(kind, blurb, arg_kind) VALUES
  ('enemies_defeated', 'when this many enemies have been put down', 'int'),
  ('rooms_built',      'when this many new rooms have been discovered', 'int'),
  ('spell_learned',    'when the player has learned this spell', 'spell'),
  ('reached_depth',    'when the player has gone this many rooms deep from where they started', 'int');

-- The story arc (REQ-ARC-STORE-2, -6): three meta ROWS, not a table — zero DDL,
-- the meta.setting precedent. Safe here because initialize() runs this seed
-- BEFORE it inserts schema_version and turn. They are three rows rather than one
-- blob so a later consumer can send the architect the premise without the ending
-- leaking into every room build.
--
-- AUTHORED CONTENT, PROVISIONAL pending author approval. Coherent with
-- seed/setting.txt; the overture replaces it at world creation in a later brick.
INSERT INTO meta(key, value) VALUES
  ('arc_premise',
   'A goblin warband has come up through a breach in Thornmere''s foundations and is hunting the old grimoires shelved in the deep stacks. The school sleeps through it, and a first-night student is out of bed.'),
  ('arc_goal',
   'The warband means to strip the deep stacks and carry the grimoires down through the breach before Thornmere wakes.'),
  ('arc_ending',
   'It would be settled if the breach were sealed with the deep stacks still on their shelves, or if the school woke in time to seal it itself.');

-- The story arc's ordered list of steps (REQ-ARC-STORE-7, -8), hand-written and
-- seeded in the SHIPPED world only — tests/combat_fixture.sql deliberately gets
-- none, so every fixture-based test runs against the empty-list world of
-- REQ-ARC-STORE-19a. Plain INSERTs, not helper calls, exactly as motive_catalog
-- and bestiary are: the seed is SQL.
--
-- Hand authorship is the point of this brick. It makes the whole advance rule
-- testable with SQL and no API key; the overture replaces this content in a
-- later brick without touching the schema.
--
-- Step 1's condition is enemies_defeated and step 2's is spell_learned because
-- those are the only two reachable with AI disabled (a latent exit is a wall,
-- so no room is ever generated and neither rooms_built nor reached_depth can
-- become true) — which is what lets the golden-session gate reach a real
-- advance offline.
--
-- No reached_turn values: a fresh world is at step zero (REQ-ARC-STORE-8).
--
-- AUTHORED CONTENT, PROVISIONAL pending author approval.
INSERT INTO story_step(n, condition_kind, condition_arg, prose) VALUES
  (1, 'enemies_defeated', '1',
   'Word of the fight runs ahead of you. Below the stair, the warband knows the school is awake.'),
  (2, 'spell_learned', 'fire',
   'They have set watchfires in the lower halls, and the deep stacks smell of smoke.'),
  (3, 'rooms_built', '4',
   'The Vigil Lamps no longer kindle in the inner corridors. Something has been at them.'),
  (4, 'reached_depth', '3',
   'They have found the index, and are reading it. The old grimoires are being counted.'),
  (5, 'enemies_defeated', '5',
   'The breach stands open to the lower halls, and the warband is carrying the deep stacks out through it.');

-- The title screen (REQ-POLISH-29, -30). Static text, rendered once while
-- authoring and pasted in — no FIGlet renderer, no font files, no runtime
-- dependency, and the art is reviewable in a diff.
--
-- A ROW, not a shape: zero DDL, no SCHEMA_VERSION bump (REQ-POLISH-32), the
-- same move meta.setting and meta.start_room make.
--
-- Plain ASCII only (REQ-UI-29, REQ-POLISH-30): '#' and spaces and nothing else.
-- No box drawing and no ambiguous-width character, so it cannot misalign on any
-- terminal. Its natural width is 53 columns; below that main.cpp prints the
-- game's name as plain text instead (REQ-POLISH-31).
INSERT INTO meta(key, value) VALUES ('title_art',
'##### ##### #   # ##### #   #  ###  ####  #     ####
  #   #      # #    #   #   # #   # #   # #     #   #
  #   ####    #     #   # # # #   # ####  #     #   #
  #   #      # #    #   ## ## #   # #  #  #     #   #
  #   ##### #   #   #   #   #  ###  #   # ##### ####');
