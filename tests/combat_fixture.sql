-- Combat test fixture: a controlled world the deterministic testCombat* tests
-- drive, isolated from base.sql (which grows more enemies across Brick 3) and
-- from fixture.sql (whose exact entity/room counts other tests pin). Mirrors
-- the seed's combat shape: two rooms, a player carrying health, and one goblin
-- grunt hand-placed in the corridor. Later bricks extend THIS file with
-- dedicated rooms/enemies per lock category so each test targets a known foe.
--
-- Id ledger:
--   1: room 'cell'
--   2: room 'corridor'
--   3: player (health 12/12), in the cell
--   4: portable 'wand', in the cell (for downed drop-all coverage)
--   5: portable 'key', in the corridor
--   6: room 'frost study'
--   7: hostile 'goblin grunt' (health 8/8, chip 1), in the corridor
--   8: hostile 'rime-touched goblin' (health 10/10, chip 1), in the frost study

INSERT INTO entities(id) VALUES (1), (2), (3), (4), (5), (6), (7), (8);

-- 1: room 'cell'
INSERT INTO room(entity) VALUES (1);
INSERT INTO name(entity, value) VALUES (1, 'cell');
INSERT INTO description(entity, prose) VALUES (1, 'A bare stone cell.');

-- 2: room 'corridor'
INSERT INTO room(entity) VALUES (2);
INSERT INTO name(entity, value) VALUES (2, 'corridor');
INSERT INTO description(entity, prose) VALUES (2, 'A long dim corridor.');

-- 6: room 'frost study' (holds the rime-touched element lock)
INSERT INTO room(entity) VALUES (6);
INSERT INTO name(entity, value) VALUES (6, 'frost study');
INSERT INTO description(entity, prose) VALUES (6, 'A frost-rimed study.');

-- exits: north 1->2, south 2->1 (realized pair; flee tests use the return leg);
-- east 2->6, west 6->2 (realized pair to the frost study).
INSERT INTO exits(room, direction, dest) VALUES
  (1, 'north', 2),
  (2, 'south', 1),
  (2, 'east', 6),
  (6, 'west', 2);
-- a latent frontier stub off the corridor (flee-into-the-unknown is refused)
INSERT INTO exits(room, direction, dest) VALUES
  (2, 'north', NULL);

-- 3: player, in the cell, carrying health like every combatant
INSERT INTO player(entity) VALUES (3);
INSERT INTO name(entity, value) VALUES (3, 'player');
INSERT INTO location(entity, container) VALUES (3, 1);
INSERT INTO health(entity, current, max) VALUES (3, 12, 12);
INSERT INTO known_spells(entity, spell) VALUES (3, 'ward'), (3, 'stun');

-- 4: portable 'wand', in the cell
INSERT INTO portable(entity) VALUES (4);
INSERT INTO name(entity, value) VALUES (4, 'wand');
INSERT INTO description(entity, prose) VALUES (4, 'A plain ashwood wand.');
INSERT INTO location(entity, container) VALUES (4, 1);

-- 5: portable 'key', in the corridor
INSERT INTO portable(entity) VALUES (5);
INSERT INTO name(entity, value) VALUES (5, 'key');
INSERT INTO description(entity, prose) VALUES (5, 'A cold iron key.');
INSERT INTO location(entity, container) VALUES (5, 2);

-- 7: hostile 'goblin grunt', in the corridor (telegraphs every 2 combat turns)
INSERT INTO hostile(entity, archetype, chip, telegraph_period) VALUES (7, 'goblin_grunt', 1, 2);
INSERT INTO health(entity, current, max) VALUES (7, 8, 8);
INSERT INTO name(entity, value) VALUES (7, 'goblin grunt');
INSERT INTO description(entity, prose) VALUES (7, 'A scrawny goblin in stolen leathers.');
INSERT INTO location(entity, container) VALUES (7, 2);

-- Spell catalog: engine-owned constants. The player knows ward + stun (above);
-- fire/frost/dispel are learned from grimoires (Step 18).
INSERT INTO spell_catalog(spell, element, cooldown, tier, effect) VALUES
  ('ward', NULL, 2, 1, 'ward'),
  ('stun', NULL, 3, 1, 'stun'),
  ('fire', 'fire', 2, 1, 'damage'),
  ('frost', 'frost', 2, 1, 'frost'),
  ('dispel', NULL, 3, 2, 'dispel'),
  ('ember', 'fire', 3, 1, 'dot');

-- Resistance: rime-touched is weak to Fire (2x), resists Frost (1/2x).
INSERT INTO resistance(archetype, element, multiplier_num, multiplier_den) VALUES
  ('rime_touched', 'fire', 2, 1),
  ('rime_touched', 'frost', 1, 2);

-- 8: hostile 'rime-touched goblin', in the frost study (element lock, weak Fire)
INSERT INTO hostile(entity, archetype, chip, telegraph_period) VALUES (8, 'rime_touched', 1, 3);
INSERT INTO health(entity, current, max) VALUES (8, 10, 10);
INSERT INTO name(entity, value) VALUES (8, 'rime-touched goblin');
INSERT INTO description(entity, prose) VALUES (8, 'A goblin sheathed in creeping frost.');
INSERT INTO location(entity, container) VALUES (8, 6);
