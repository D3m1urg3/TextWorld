-- TextWorld base seed: the two-room prototype world.
-- Literal entity ids, per design §8. Id ledger:
--   1: room 'stone hall'
--   2: room 'garden'
--   3: player
--   4: portable 'lantern'
--   5: portable 'key'

INSERT INTO entities(id) VALUES (1), (2), (3), (4), (5);

-- 1: room 'stone hall'
INSERT INTO room(entity) VALUES (1);
INSERT INTO name(entity, value) VALUES (1, 'stone hall');
INSERT INTO description(entity, prose) VALUES (1,
  'A vaulted hall of grey stone, its flagstones worn smooth by feet long gone. Cold air pools in the corners, and a doorway to the north lets in a thin blade of green light.');

-- 2: room 'garden'
INSERT INTO room(entity) VALUES (2);
INSERT INTO name(entity, value) VALUES (2, 'garden');
INSERT INTO description(entity, prose) VALUES (2,
  'An overgrown walled garden, all bramble and drowsy bees. Ivy has claimed the sundial, and the path south back into the hall is nearly lost under fallen petals.');

-- exits: north 1→2, south 2→1
INSERT INTO exits(room, direction, dest) VALUES
  (1, 'north', 2),
  (2, 'south', 1);

-- 3: player, standing in the stone hall (no description: the player is not canon prose)
INSERT INTO player(entity) VALUES (3);
INSERT INTO name(entity, value) VALUES (3, 'player');
INSERT INTO location(entity, container) VALUES (3, 1);

-- 4: portable 'lantern', in the stone hall
INSERT INTO portable(entity) VALUES (4);
INSERT INTO name(entity, value) VALUES (4, 'lantern');
INSERT INTO description(entity, prose) VALUES (4,
  'A brass lantern, dented and smoke-dulled, its little flame steady behind sooty glass.');
INSERT INTO location(entity, container) VALUES (4, 1);

-- 5: portable 'key', in the garden
INSERT INTO portable(entity) VALUES (5);
INSERT INTO name(entity, value) VALUES (5, 'key');
INSERT INTO description(entity, prose) VALUES (5,
  'An iron key gone orange with rust, heavy as a promise, its teeth cut for a lock you have not found.');
INSERT INTO location(entity, container) VALUES (5, 2);
