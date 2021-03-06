/* Devices */

INSERT INTO devices (                  /*  1 */
  value, product, created  
) VALUES (
  'aabbccddeeff11223344556677889900', 40, 1372330615
);

/* Groups Members */

INSERT INTO groups_members (
  group_id, device_id
) VALUES (
  10, 1
);

/* Identities */

INSERT INTO identities (
  type, value
) VALUES ( /* dave@strongswan.org */
  5, X'64617665'
);

/* Sessions */

INSERT INTO sessions (
  time, connection, identity, device, product, rec
) VALUES (
  NOW, 1, 1, 1, 40, 0
);

/* Results */

INSERT INTO results (
  session, policy, rec, result
) VALUES (
  1, 1, 0, 'processed 355 packages: 0 not updated, 0 blacklisted, 4 ok, 351 not found'
);

/* Enforcements */

INSERT INTO enforcements (
  policy, group_id, max_age, rec_fail, rec_noresult
) VALUES (
  3, 10, 0, 2, 2
);

INSERT INTO enforcements (
  policy, group_id, max_age
) VALUES (
  17, 2, 86400
);

INSERT INTO enforcements (
  policy, group_id, max_age
) VALUES (
  18, 10, 86400
);

DELETE FROM enforcements WHERE id = 1;
