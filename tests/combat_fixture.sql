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
--   9: room 'armory'
--  10: hostile 'ironhide brute' (health 14/14, chip 1, barriered), in the armory
--  11: room 'library'
--  12,13,14: hostile 'book swarm' bodies (health 10/10, chip 1), in the library
--  15: room 'outer hall' — a safe edge, graph distance 3 from the seed (room 1),
--      beyond the front radius: its eligible-enemy menu is empty (REQ-COMBAT-34)

INSERT INTO entities(id) VALUES
  (1), (2), (3), (4), (5), (6), (7), (8), (9), (10), (11), (12), (13), (14), (15);

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

-- 9: room 'armory' (holds the ironhide defense lock), off the corridor via up
INSERT INTO room(entity) VALUES (9);
INSERT INTO name(entity, value) VALUES (9, 'armory');
INSERT INTO description(entity, prose) VALUES (9, 'A cramped armory of dull iron.');

-- 11: room 'library' (holds the book-swarm multiplicity lock), off the cell via down
INSERT INTO room(entity) VALUES (11);
INSERT INTO name(entity, value) VALUES (11, 'library');
INSERT INTO description(entity, prose) VALUES (11, 'A vaulted library, shelves stirring.');

-- 15: room 'outer hall', off the frost study via east — distance 3 from the seed
-- (1 → 2 → 6 → 15), a safe edge beyond the front radius (REQ-COMBAT-34).
INSERT INTO room(entity) VALUES (15);
INSERT INTO name(entity, value) VALUES (15, 'outer hall');
INSERT INTO description(entity, prose) VALUES (15, 'A still hall at the quiet edge.');

-- exits: north 1->2, south 2->1 (realized pair; flee tests use the return leg);
-- east 2->6, west 6->2 (frost study); up 2->9, down 9->2 (armory);
-- down 1->11, up 11->1 (library, off the cell so the swarm is reachable alone).
INSERT INTO exits(room, direction, dest) VALUES
  (1, 'north', 2),
  (2, 'south', 1),
  (2, 'east', 6),
  (6, 'west', 2),
  (2, 'up', 9),
  (9, 'down', 2),
  (1, 'down', 11),
  (11, 'up', 1),
  (6, 'east', 15),
  (15, 'west', 6);
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

-- The bestiary catalog: one frozen record per archetype — the mold every
-- instance below is CAST FROM (REQ-COMBAT-28, -29, -30). Numbers match base.sql's
-- catalog; every instance in this fixture copies its stats from here, so a
-- fixture enemy can never drift from its archetype.
INSERT INTO bestiary(archetype, name, blurb, health, chip, telegraph_period, tier, barrier) VALUES
  ('goblin_grunt', 'goblin grunt',
   'a scrawny goblin raider up from the breached lower halls, quick and lightly armored', 8, 1, 2, 1, 0),
  ('rime_touched', 'rime-touched goblin',
   'a goblin sheathed in creeping frost, cold to approach and grudging to burn', 10, 1, 3, 1, 0),
  ('ironhide', 'ironhide brute',
   'a brute plated in warded iron that turns aside every blow until its ward is broken', 14, 1, 4, 2, 1),
  ('book_swarm', 'snapping folio',
   'a loose flock of snapping folios, flimsy alone but dangerous in a swarm', 10, 1, 0, 1, 0);

INSERT INTO drop_table(archetype, spell) VALUES
  ('goblin_grunt', 'fire'),
  ('rime_touched', 'frost'),
  ('ironhide', 'dispel'),
  ('book_swarm', 'blast');

-- 7: hostile 'goblin grunt', in the corridor (telegraphs every 2 combat turns).
-- Stats cast from the bestiary; only id, prose, and location are placed by hand.
INSERT INTO hostile(entity, archetype, chip, telegraph_period)
  SELECT 7, archetype, chip, telegraph_period FROM bestiary WHERE archetype = 'goblin_grunt';
INSERT INTO health(entity, current, max)
  SELECT 7, health, health FROM bestiary WHERE archetype = 'goblin_grunt';
INSERT INTO name(entity, value)
  SELECT 7, name FROM bestiary WHERE archetype = 'goblin_grunt';
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
  ('ember', 'fire', 3, 1, 'dot'),
  ('blast', NULL, 3, 2, 'aoe');

-- Resistance: rime-touched is weak to Fire (2x), resists Frost (1/2x).
INSERT INTO resistance(archetype, element, multiplier_num, multiplier_den) VALUES
  ('rime_touched', 'fire', 2, 1),
  ('rime_touched', 'frost', 1, 2);

-- 8: hostile 'rime-touched goblin', in the frost study (element lock, weak Fire).
-- Stats cast from the bestiary.
INSERT INTO hostile(entity, archetype, chip, telegraph_period)
  SELECT 8, archetype, chip, telegraph_period FROM bestiary WHERE archetype = 'rime_touched';
INSERT INTO health(entity, current, max)
  SELECT 8, health, health FROM bestiary WHERE archetype = 'rime_touched';
INSERT INTO name(entity, value)
  SELECT 8, name FROM bestiary WHERE archetype = 'rime_touched';
INSERT INTO description(entity, prose) VALUES (8, 'A goblin sheathed in creeping frost.');
INSERT INTO location(entity, container) VALUES (8, 6);

-- 10: hostile 'ironhide brute', in the armory (defense lock: barriered until
-- Dispel). Stats cast from the bestiary; its barrier row is likewise catalog-
-- driven — planted only because the archetype's `barrier` flag is 1.
INSERT INTO hostile(entity, archetype, chip, telegraph_period)
  SELECT 10, archetype, chip, telegraph_period FROM bestiary WHERE archetype = 'ironhide';
INSERT INTO health(entity, current, max)
  SELECT 10, health, health FROM bestiary WHERE archetype = 'ironhide';
INSERT INTO name(entity, value)
  SELECT 10, name FROM bestiary WHERE archetype = 'ironhide';
INSERT INTO description(entity, prose) VALUES (10, 'A brute plated in warded iron.');
INSERT INTO location(entity, container) VALUES (10, 9);
INSERT INTO barrier(entity)
  SELECT 10 FROM bestiary WHERE archetype = 'ironhide' AND barrier = 1;

-- 12,13,14: book-swarm bodies co-located in the library. Multiplicity lock:
-- multiple low-HP bodies, chip only, NO telegraph (period 0) — overlapping
-- telegraphs are out of scope; the answer is AoE/DoT reaching all bodies. All
-- three bodies are cast from the ONE book_swarm catalog row (cross join over the
-- id list), so the swarm shares its archetype's stats exactly.
INSERT INTO hostile(entity, archetype, chip, telegraph_period)
  SELECT v.id, b.archetype, b.chip, b.telegraph_period
  FROM bestiary b, (SELECT 12 AS id UNION ALL SELECT 13 UNION ALL SELECT 14) v
  WHERE b.archetype = 'book_swarm';
INSERT INTO health(entity, current, max)
  SELECT v.id, b.health, b.health
  FROM bestiary b, (SELECT 12 AS id UNION ALL SELECT 13 UNION ALL SELECT 14) v
  WHERE b.archetype = 'book_swarm';
INSERT INTO name(entity, value)
  SELECT v.id, b.name
  FROM bestiary b, (SELECT 12 AS id UNION ALL SELECT 13 UNION ALL SELECT 14) v
  WHERE b.archetype = 'book_swarm';
INSERT INTO description(entity, prose) VALUES
  (12, 'A book that bites.'), (13, 'A book that bites.'), (14, 'A book that bites.');
INSERT INTO location(entity, container) VALUES (12, 11), (13, 11), (14, 11);
