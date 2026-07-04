# Hanzo SQL (pg-wire) — multi-tenant Postgres-wire over pglite

A single `postgres://` endpoint that fronts MANY tenant databases, each a
single embedded-Postgres data directory on a PVC. A service keeps
`DATABASE_URL=postgres://…` and flips off the shared Postgres with **no app
rewrite** — the master-unlock for migrating our Prisma/ORM forks.

Exactly analogous to **docdb** (FerretDB → Mongo-wire per tenant): docdb is the
document product, this is the relational one. The tenant boundary is a
**directory** (`/data/<dbname>`), provisioned on first connection.

## Why pglite (not literal SQLite)

pglite (`@electric-sql/pglite`) is real Postgres compiled to WASM — full pg
dialect. An **unmodified** Prisma `provider = "postgresql"` schema (enums,
`text[]`, `timestamptz`, `gen_random_uuid`, `bytea`, `jsonb`) runs with zero
rewrite, because it IS Postgres. Literal-SQLite (postlite) rejects those
schemas at the dialect layer (scalar arrays + native types) before any wire is
involved — a dead end for our fork schemas.

## How it works (`server.mjs`, ~180 LOC)

One TCP listener on `:5432`. Per connection:

1. **Peek** the pg startup packet. Answer any leading `SSLRequest` /
   `GSSENCRequest` with `N` (in-cluster traffic is on the pod network), then
   read the `StartupMessage` and extract the `database` field.
2. **Validate** the dbname against `^[A-Za-z0-9_][A-Za-z0-9_-]{0,62}$` (defeats
   path traversal; mirrors the 63-byte pg identifier limit). Bad name → a FATAL
   `ErrorResponse` and close.
3. **Resolve** (or lazily create — provision-on-demand) that tenant's pglite
   instance at `/data/<dbname>`, fronted by a per-tenant `PGLiteSocketServer`
   bound to a private Unix socket (`$HANZO_SQL_SOCKDIR/<dbname>.sock`). A
   Promise-cache guarantees one instance per dir (never two writers).
4. **Splice** the client TCP socket to the tenant's Unix socket, forwarding the
   buffered `StartupMessage` first.

The router is only a startup-peeking dispatcher. The library
(`@electric-sql/pglite-socket`) owns the wire protocol, per-db query
serialization (`QueryQueueManager`), and multi-connection handling — we reuse
its tested logic instead of reimplementing its (unexported) queue. Concurrent
connections to the same db share that db's serialized queue (correct MVCC via
pglite); two different dbnames get two dirs = fully isolated data.

## Config (env)

| Env | Default | Meaning |
|-----|---------|---------|
| `HANZO_SQL_DATA` | `/data` | base dir; per-tenant data dir is `<DATA>/<dbname>` (PVC-mounted) |
| `HANZO_SQL_SOCKDIR` | `$TMPDIR/hanzo-sql` | per-tenant Unix sockets (ephemeral; keep short — `sun_path` ≤ ~104 bytes) |
| `HANZO_SQL_HOST` | `0.0.0.0` | listen host |
| `HANZO_SQL_PORT` | `5432` | listen port |
| `HANZO_SQL_MAX_CONN` | `1000` | max concurrent connections per tenant db |
| `HANZO_SQL_DEBUG` | — | `1` for verbose logs |

## Proof (see `evidence/`)

- **`dataroom-prisma-migrate-deploy.log`** — the real **dataroom** (Papermark
  fork) schema, `provider = "postgresql"`, **107 migrations**, run via
  `prisma migrate deploy` against `postgres://…@…:5432/dataroomdb` pointed at
  this router. Result: `All migrations have been successfully applied.`
  (63 public tables, 18 pg ENUM types recorded).
- **`roundtrip-and-isolation.log`** (`roundtrip.mjs`) — full pg-dialect CRUD
  over the wire (`text[]`, enum, `bytea`, `jsonb`, `gen_random_uuid`,
  `timestamptz`, `array_append`) + multi-tenant **isolation**: `tenant_alpha`
  and `tenant_beta` get separate dirs/data; `tenant_beta` cannot see dataroom's
  tables. Verified surviving a full server restart (persistence).
- **`pglite-feature-support.log`** (`features.mjs`) — honest feature matrix.

### What does NOT work in pglite (honest)

`CREATE EXTENSION` for `pgcrypto` / `uuid-ossp` / `pg_trgm` / `vector` /
`citext` fails at runtime: **pglite extensions must be pre-registered at
instance creation** (`PGlite.create(dir, { extensions: { … } })`), not loaded
via runtime `CREATE EXTENSION`. The MVP preloads none, so only built-ins are
available. `gen_random_uuid()` is a **built-in** (pg13+) and works, which
covers the common Prisma default. dataroom's 107 migrations use no extension —
so for that real fork schema, **zero** features failed. Everything else works:
generated columns, partitioned tables, `LISTEN`/`NOTIFY`, materialized views,
full-text search, jsonb ops, window functions, arrays, enums, native types.
Follow-up: preload the common extension set (pgvector, pg_trgm, uuid-ossp).

Single-writer note: pglite serializes writes per db (one WASM instance per
dir). Correct, but not a multi-writer cluster — matches the per-tenant model.

## Image + deploy

- Image **`ghcr.io/hanzoai/sql-wire`**, semver tags only (`0.1.0`, …) — never
  `:latest`, never `sha-…`. Built by `.github/workflows/sql-wire.yml` on
  `sql-wire-v*` git tags (distinct from the Postgres fork's `v*` tags), on the
  hanzoai `hanzo-build-linux-amd64` pool. Base `ghcr.io/hanzoai/nodejs:v24.18.0`.
- K8s: an operator `Service` CR (`hanzo.ai/v1`) named `hanzo-sql-wire`, port
  5432, a **plain PVC** volume at `/data` (NOT `spec.persistence` — that injects
  the `hanzoai/replicate` SQLite-WAL sidecar, which is WRONG for a pg data dir),
  `fsGroup: 65532` so the non-root image can write the PVC.
  Manifest: `universe/infra/k8s/operator/crs/hanzo-sql-wire.yaml`.
- A consumer points at it with, e.g.:
  `DATABASE_URL=postgres://hanzo:pw@hanzo-sql-wire.hanzo.svc:5432/<dbname>`
  (pglite uses trust auth — the password is ignored; isolation is the pod
  network + one dir per db).

## Files

    server.mjs            the multi-tenant startup-peeking router
    package.json          pinned deps (pglite 0.5.4, pglite-socket 0.2.7)
    Dockerfile            ghcr.io/hanzoai/sql-wire (amd64)
    evidence/             raw prisma / round-trip / feature output backing every claim
