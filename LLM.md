# postgres

PostgreSQL Database Management System
=====================================

This directory contains the source code distribution of the PostgreSQL
database management system.

## hanzo-sql: the image and its reconciler

`ghcr.io/hanzoai/sql` is PostgreSQL 18 (`FROM postgres:18-bookworm`, not this
tree's 19beta1 source) with pgvector, pg_cron, pg_documentdb and `contrib/zap_fdw`,
run as a replicated member by `sqlha` under Patroni.

`bin/reconcile` is the sidecar of a sql pod. Each pass it makes the cluster equal
a declared file, and acts only on the leader (`pg_is_in_recovery()` false), over
the local socket; on a standby it does nothing. It reads the file only when its
bytes equal the SHA-256 it is started with (`--sha256`, an argument in the pod
spec; a pin beside the file would be edited with it). In order it:

- creates each declared role `LOGIN NOSUPERUSER NOREPLICATION NOBYPASSRLS` with no
  password (`CREATEDB`/`CREATEROLE` only where declared) and sets
  `synchronous_commit=on` on it;
- adds every `oauth` role to the group `sql_iam` (`enroll` adds ADMIN OPTION);
- runs `CREATE DATABASE ... OWNER`, revokes CONNECT from PUBLIC on each declared
  database and grants it to the roles whose `connect` lists it;
- adopts an existing database (`adopt: <role>`) by `ALTER ... OWNER TO` on each
  object that role owns there, never `REASSIGN OWNED BY`, then lists what the role
  still owns there and reports each object (a foreign-data wrapper, an event
  trigger and a subscription can only belong to a superuser); an `adopt` role
  that does not exist is reported;
- reports every `sql_iam` member whose `rolpassword` is not NULL (exit status 1);
- PATCHes the declared `dcs:` block through Patroni's local REST API until
  `/config` equals it, and reloads pg_hba when the hash of its lines changes. The
  hash is kept as the comment on the role `sql_iam`.

It drops nothing, sets no password, alters no role the file does not name, and
logs every statement. Identifiers are always quoted. Every connection sets
`lock_timeout` (5 s) and `statement_timeout` (60 s): a session holding a lock
fails one step, which is reported, and the next pass retries; the pass and the
table's other users are never held longer. Declared file keys: `roles` (`oauth`,
`createdb`, `createrole`, `enroll`, `connect`), `databases` (`owner`, `adopt`),
`dcs` (Patroni's dynamic config, whole); the module docstring is the reference.

A declared role is converged to `LOGIN NOSUPERUSER NOREPLICATION NOBYPASSRLS` and
the two flags, so a role that needs more (REPLICATION, BYPASSRLS, a membership such
as `pg_read_all_data`) is not declared: it is left as it is. The file refuses the
superuser (`SQL_USER`), the Patroni replication user (`SQL_REPLICATION_USER`),
`sql_iam` and `pg_*`, so a declaration cannot strip `replicator` of REPLICATION.

pg_hba lives in the DCS and Patroni writes it to every member. A
`postgresql.pg_hba` in a member's own Patroni file shadows it, and the pass then
errors on every run (after waiting 30 s) that pg_hba.conf does not hold the
declared lines: remove the local one.

The sidecar needs the server's socket directory shared with it, `SQL_USER`,
`SQL_REPLICATION_USER`, and `PATRONI_RESTAPI_USERNAME`/`PATRONI_RESTAPI_PASSWORD`.

Tests: `python3 bin/test_reconcile.py` starts a leader and a standby under Patroni
(server binaries via `pg_config` on PATH, else the newest
`/usr/lib/postgresql/*/bin`; `patroni[raft]`, psycopg2 and PyYAML importable). It
is run by hand, against the image's PostgreSQL major (18): CI has no server and the
image has no raft extra. Last run on PostgreSQL 16.15 and 18.6.

## contrib/hanzo_iam

OAuth validator for hanzo-sql (`oauth_validator_libraries = 'hanzo_iam'`), built with PGXS
against PostgreSQL 18 (the server the image ships), not by the top-level build. The pg_hba.conf
line is `oauth issuer="https://hanzo.id" scope="hanzo-sql" validator=hanzo_iam
delegate_ident_mapping=1`.

- **Trust**: the public keys `<kid>.pem` in GUC `hanzo_iam.dir` (SIGHUP), read on every
  connection, so removing a file revokes its key at once. Rotation adds a file, runs both, removes
  the old one. No HTTP to IAM.
- **Token**: compact JWS, RS256 (OpenSSL EVP, RSA key of at least 2048 bits, at most 16 KiB of
  PEM). At most 8192 bytes whole, three parts; each part decodes into a buffer capped at 6 KiB, so
  the size check is not the only bound. `iss` is `https://hanzo.id`, `aud` holds `hanzo-sql`, `typ`
  is `sql`, no `act`, `sub` is `sql:<role>`, `exp` and `iat` required, `exp` at most 60 s past,
  `iat` at most 60 s ahead, `nbf` optional and, when present, at most 60 s ahead, `exp - iat` in
  (0, 600] s. `typ` is a claim: the JOSE header's own `typ` is not read, so a minter puts `typ`
  in the claims (a header of `JWT` is fine). The claims are not parsed until the signature
  verifies. JSON goes through PG's jsonapi, escapes decoded; a member a check names may appear
  once, however it is spelled.
- **Authorization**: `authn_id` is the role in `sub`; the connection is authorized only when it
  equals the role requested. Why a token was refused is one `hanzo_iam: <reason>` log line from a
  fixed list (a role mismatch logs `role mismatch` and neither role), never sent to the client. A
  key file that cannot be opened or is no PEM public key is also named by its path, for the
  operator.
- **Files**: `token.c` is pure (server module and fuzz target both compile it), `hanzo_iam.c` is
  the module. Tests: `make installcheck` runs `t/001_hanzo_iam.pl` against an installed server;
  `make fuzz` builds `fuzz/fuzz` (libFuzzer, ASan, UBSan; seeds in `fuzz/corpus`), run with
  `-max_len=9500` so inputs reach past the 8 KiB bound.
- **CI**: nothing runs the TAP test or the fuzz target. `hanzo.yml` declares no `test:` lane and
  the Dockerfile only compiles the module, so run both by hand (PostgreSQL 18, `make installcheck`;
  `make fuzz`, 10 minutes clean) before changing `token.c` or `hanzo_iam.c`.
