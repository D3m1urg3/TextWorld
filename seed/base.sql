-- TextWorld base seed: the two-room prototype world.
-- Literal entity ids, per design §8. Id ledger:
--   1: room 'dormitory cell'
--   2: room 'corridor'
--   3: player
--   4: portable 'candle'
--   5: portable 'key'
--   6: portable 'wand'

INSERT INTO entities(id) VALUES (1), (2), (3), (4), (5), (6);

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
