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

## How it works (`server.mjs`, ~230 LOC)

One TCP listener on `:5432`. The router **terminates the pg wire protocol** and
drives pglite's `execProtocolRaw` directly. It does **not** use
`@electric-sql/pglite-socket` — that library's per-message forwarding emits a
premature `ReadyForQuery` and hangs strict clients (asyncpg), and it crashes
pglite outright on `COPY … FROM stdin`.

Per connection:

1. **Peek** the startup packet: answer any `SSLRequest`/`GSSENCRequest` with `N`
   (in-cluster traffic is on the pod network), read the `StartupMessage`, extract
   the `database` field, validate it (`^[A-Za-z0-9_][A-Za-z0-9_-]{0,62}$`, defeats
   path traversal). Bad name → FATAL `ErrorResponse`.
2. **Resolve** (or lazily create — provision-on-demand) that tenant's pglite
   instance at `/data/<dbname>` (Promise-cache = one instance per dir).
3. **Drive** the session: the startup message and every subsequent frontend
   message **batch** go to `db.execProtocolRaw`. Messages are batched to each
   `Sync`/`Flush`/simple-`Query` boundary so a pipelined extended-protocol
   prepare (`Parse`+`Describe`+`Sync`) is processed atomically — the fix that
   makes strict clients (asyncpg) work.

Three protocol details the router owns (pglite/one-session-per-dir quirks):

- **`COPY … FROM stdin` is intercepted.** Sent over the raw wire it *crashes*
  pglite; instead the router captures `CopyData`/`CopyDone`, rewrites the SQL to
  `COPY … FROM '/dev/blob'`, runs pglite's programmatic COPY, and synthesizes
  `CopyInResponse`/`CommandComplete`/`ReadyForQuery`. `COPY … TO stdout` needs no
  special handling (works over the raw wire). This is what makes a
  `pg_dump | psql` data load work.
- **Named statements/portals are namespaced per connection.** pglite is ONE
  backend session per data dir, but prepared statements are session-scoped in
  Postgres. Without isolation, connection B reusing a name connection A prepared
  (e.g. asyncpg's `__asyncpg_stmt_1__` across a pool) collides
  ("prepared statement already exists"). The router prefixes every non-empty
  name in `Parse`/`Bind`/`Describe`/`Close`/`Execute` with a per-connection tag.
- **`Terminate` closes only that socket** — never forwarded to the shared session
  (which would tear it down for every other connection on the tenant).

Per-db serialization with transaction affinity keeps the single shared session
consistent: while a transaction is open, only its owning connection's messages
run; a connection that dies mid-transaction is rolled back.

## Config (env)

| Env | Default | Meaning |
|-----|---------|---------|
| `HANZO_SQL_DATA` | `/data` | base dir; per-tenant data dir is `<DATA>/<dbname>` (PVC-mounted) |
| `HANZO_SQL_LISTEN_PORT` | `5432` | listen port. **Not `HANZO_SQL_PORT`** — a k8s service-link injects `<SVC>_PORT=tcp://…` for the `hanzo-sql` alias, which would shadow it (`Number("tcp://…")=NaN` → crash). The code reads the legacy `HANZO_SQL_PORT` too, but ignores it unless it parses as an integer. |
| `HANZO_SQL_HOST` | `0.0.0.0` | listen host |
| `HANZO_SQL_DEBUG` | — | `1` for verbose logs |

## Verified clients (see `evidence/`)

| Client | Status | Evidence |
|--------|--------|----------|
| **node-postgres / Prisma** | ✓ full | `dataroom-prisma-migrate-deploy.log` (real Papermark schema, `provider=postgresql`, **107 migrations**, `prisma migrate deploy` → all applied; 63 tables, 18 enums), `roundtrip-and-isolation.log` |
| **psql** | ✓ incl. COPY | `copy-roundtrip-parity.log` — `\copy … TO` (raw) + `\copy … FROM` (intercepted) round-trip with **byte-exact row parity** (`src EXCEPT dst = 0`), across NULL, empty array, unicode, embedded tab/newline, apostrophe, quoted array element, and a **0-row table** (pg_dump emits COPY even for empty tables) |
| **asyncpg** (Python) | ✓ incl. default statement cache, pools, reconnect | `asyncpg-roundtrip.log` — single conn + a 5-connection pool + sequential reconnect to the same tenant (the named-statement collision case) |

### What does NOT work in pglite (honest)

`CREATE EXTENSION` for `pgcrypto` / `uuid-ossp` / `pg_trgm` / `vector` /
`citext` fails at runtime — pglite extensions must be **pre-registered at
instance creation** (`PGlite.create(dir, { extensions })`), not loaded via SQL.
The MVP preloads none. `gen_random_uuid()` is a **built-in** (pg13+) and works
(the common Prisma default). dataroom's 107 migrations use no extension — for
that real fork schema, **zero** features fail. `pglite-feature-support.log` has
the matrix (generated columns, partitioning, LISTEN/NOTIFY, matviews, FTS,
jsonb, window fns, arrays, enums, native types all OK). Follow-up: preload the
common extension set (pgvector, pg_trgm, uuid-ossp).

Single-session note: pglite is one backend session per data dir, so writes are
serialized and concurrent transactions from different connections cannot truly
overlap (they queue). Correct for the per-tenant / migration-target model; not a
multi-writer cluster.

## Image + deploy

- Image **`ghcr.io/hanzoai/sql-wire`**, semver tags only (`0.1.1`, …) — never
  `:latest`, never `sha-…`. Built by `.github/workflows/sql-wire.yml` on
  `sql-wire-v*` git tags (distinct from the Postgres fork's `v*` tags), on the
  `hanzo-build-linux-amd64` pool. Base `ghcr.io/hanzoai/nodejs:v24.18.0`.
- K8s: an operator `Service` CR (`hanzo.ai/v1`) named `hanzo-sql-wire`, port
  5432, a **plain PVC** at `/data` (NOT `spec.persistence` — that injects the
  `hanzoai/replicate` SQLite-WAL sidecar, WRONG for a pg data dir),
  `fsGroup: 65532`. Manifest:
  `universe/infra/k8s/operator/crs/hanzo-sql-wire.yaml`.
- A consumer points at it with:
  `DATABASE_URL=postgres://hanzo:pw@hanzo-sql-wire.hanzo.svc:5432/<dbname>`
  (pglite uses trust auth — password ignored; isolation is the pod network +
  one dir per db).

## Files

    server.mjs            the multi-tenant wire router (drives pglite execProtocolRaw)
    package.json          pinned dep (pglite 0.5.4)
    Dockerfile            ghcr.io/hanzoai/sql-wire (amd64)
    evidence/             raw prisma / COPY-parity / asyncpg / feature output backing every claim
