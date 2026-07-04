#!/usr/bin/env bash
set -u
SP=/private/tmp/claude-501/-Users-z-work-hanzo-hanzo-ai/e413594f-4028-4b99-be20-41bf17b66c0a/scratchpad
DB="postgresql://hanzo:pw@127.0.0.1:55432/copyround?sslmode=disable"
q() { psql "$DB" -v ON_ERROR_STOP=1 -qtA -c "$1"; }

echo "=== build source w/ edge cases (NULL, empty array, unicode, tab, apostrophe) ==="
q "DROP TABLE IF EXISTS src, dst, src_empty, dst_empty;"
q "CREATE TABLE src(id int primary key, name text, tags text[], note text, amt numeric, ts timestamptz);"
q "INSERT INTO src VALUES
 (1,'alice','{x,y}','plain',3.14,'2026-07-04T12:00:00Z'),
 (2,'bob',NULL,E'tab\there',NULL,NULL),
 (3,'café','{}',E'line1\nline2',-0.5,'2026-01-02T03:04:05Z'),
 (4,'quote','{a,\"b c\"}','has ''apos''',1000000,'2026-01-02T03:04:05Z');"
q "CREATE TABLE src_empty(id int);"

echo "=== COPY OUT (raw wire): src -> file ==="
psql "$DB" -c "\copy src TO '$SP/wiretest/src.copy'"
psql "$DB" -c "\copy src_empty TO '$SP/wiretest/src_empty.copy'"
wc -c "$SP/wiretest/src.copy" "$SP/wiretest/src_empty.copy"

echo "=== COPY IN (intercepted -> programmatic): file -> dst ==="
q "CREATE TABLE dst(id int primary key, name text, tags text[], note text, amt numeric, ts timestamptz);"
q "CREATE TABLE dst_empty(id int);"
psql "$DB" -c "\copy dst FROM '$SP/wiretest/src.copy'"
psql "$DB" -c "\copy dst_empty FROM '$SP/wiretest/src_empty.copy'"

echo "=== PARITY ==="
echo "src rows   = $(q 'SELECT count(*) FROM src')"
echo "dst rows   = $(q 'SELECT count(*) FROM dst')"
echo "src EXCEPT dst (want 0) = $(q 'SELECT count(*) FROM (SELECT * FROM src EXCEPT SELECT * FROM dst) x')"
echo "dst EXCEPT src (want 0) = $(q 'SELECT count(*) FROM (SELECT * FROM dst EXCEPT SELECT * FROM src) x')"
echo "src_empty=$(q 'SELECT count(*) FROM src_empty') dst_empty=$(q 'SELECT count(*) FROM dst_empty') (both 0)"
echo "--- dst edge rows ---"
psql "$DB" -v ON_ERROR_STOP=1 -qA -c "SELECT id,name,tags,note,amt FROM dst ORDER BY id"
