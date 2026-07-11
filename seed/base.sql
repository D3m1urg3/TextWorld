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

-- 7: hostile 'goblin grunt', hand-placed in the corridor (REQ-COMBAT-39). The
-- tutorial lock: low health, basic-attack-soluble, a small untelegraphed chip
-- clock. Numbers are per-instance literals here (micro-decision 4); Brick 4's
-- bestiary catalog becomes the mold these are cast from. archetype tag =
-- 'goblin_grunt'.
INSERT INTO hostile(entity, archetype, chip, telegraph_period) VALUES (7, 'goblin_grunt', 1, 2);
INSERT INTO health(entity, current, max) VALUES (7, 8, 8);
INSERT INTO name(entity, value) VALUES (7, 'goblin grunt');
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
