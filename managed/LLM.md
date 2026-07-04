# Hanzo SQL (managed)

Per-tenant, SQLite-backed, provisioned-on-demand SQL — the replacement for the
shared multi-tenant Postgres (`ghcr.io/hanzoai/sql:18`) we are retiring. Exactly
analogous to **docdb** (FerretDB-on-SQLite → Mongo-wire per tenant): docdb is the
document product, Hanzo SQL is the relational one. The tenant boundary is a
**file**, not a schema/row filter.

This directory is the product spec + the pieces that were missing. The core
provisioning machinery **already exists in the operator** — this is not a
greenfield build.

## TL;DR recommendation

There is no single winner; the pg-wire question and the "which backend" question
have different answers, and both are proven below with real `prisma` output.

| Option | What | Verdict |
|--------|------|---------|
| **C — `DATABASE_URL=file:` SQLite** (esign / `Service.persistence`) | native SQLite, no wire hop | **PRIMARY path for our own services.** Requires a 2-line schema adaptation + a one-time data ETL. Already in production for `esign`. |
| **A — pglite pg-wire** (`@electric-sql/pglite`) | real Postgres in WASM, single dir per tenant, speaks `postgres://` | **FOLLOW-UP path** for schemas we cannot rewrite and external `ManagedDatabase` tenants who demand a literal `postgres://`. Runs an **unmodified** Prisma pg schema. |
| **B — postlite / pg-wire-over-SQLite** | pg protocol, SQLite storage | **REJECTED.** The SQLite *dialect* cannot host a real Postgres Prisma schema — fails before any wire is involved. |

## The crux, decided by evidence (not opinion)

Test subject: the real **dataroom** (Papermark fork) Prisma schema —
`/Users/z/work/hanzo/dataroom/prisma/schema/` — 62 tables, 18 pg enums, 11 pg
array columns (`text[]`, `int[]`), `@db.Text` / `@db.Timestamp` native types.

**Option A (pglite): the unmodified pg schema pushes and runs.**
`prisma db push` of dataroom's schema against a pglite pg-wire socket
(`evidence/optionA-pglite-dbpush.log`):

    Datasource "db": PostgreSQL database "postgres", schema "public" at "127.0.0.1:55432"
    🚀  Your database is now in sync with your Prisma schema. Done in 545ms

…then a live enum + `text[]` CRUD roundtrip (`INSERT`/`array_append`/`SELECT`/`DELETE`)
passed, surviving a process restart (persistence confirmed).

**Option B/C (SQLite dialect): the same schema is rejected at validation** —
before any backend is touched (`evidence/optionBC-sqlite-dialect.log`, P1012, 17 errors):

    error: Field "allowList" in model "Link" can't be a list. The current connector does not support lists of primitive types.
    error: Field "pages" in model "DocumentAnnotation" can't be a list. ...
    error: Native type Text is not supported for sqlite connector.
    error: Native type Timestamp is not supported for sqlite connector.

This is a **dialect-layer** failure (11 scalar-list fields + 6 native-type
fields). No pg-wire-over-SQLite proxy (postlite) can fix it: with
`provider = "sqlite"` Prisma refuses the arrays; with `provider = "postgresql"`
Prisma emits `CREATE TYPE … AS ENUM` and `text[]` DDL that SQLite's parser
rejects. Either way it breaks. **Option B is dead.** (Note: pg **enums** alone are
fine on SQLite — Prisma emulates them as `TEXT`; the blockers are **arrays** and
**native types**.)

**Why C is still the primary path despite the above:** for services *we* own, we
rewrite the schema to remove arrays/native-types (arrays → a `String` holding a
JSON array, decoded in the client). `esign` already did exactly this
(`/Users/z/work/hanzo/esign/packages/prisma/schema.prisma`, `provider = "sqlite"`,
0 scalar arrays, 0 `@db.*`) and runs `DATABASE_URL=file:/data/sign.db` in
production. C is simpler and faster (no wire hop, no second process) — it is the
default. A is the escape hatch for schemas we cannot touch (dataroom) or external
customers who require pg-wire.

## The existing operator model (this is "Hanzo SQL")

Provisioning is already declarative and already shipped. Two operator surfaces:

1. **`Service.persistence`** (`operator/src/crd.rs`, `PersistenceSpec` + the
   `Service.spec.persistence` field) — one field on any Service CR auto-wires
   durable SQLite: a shared `app-db` volume, a `replicate-restore` initContainer,
   and a `hanzoai/replicate` sidecar streaming the SQLite WAL to SeaweedFS/S3,
   age-encrypted client-side. `dir_mode` fans one volume into many `.db` files
   (per-tenant / per-org). **This is path C.** See
   `deploy/service-persistence.example.yaml`.

2. **`SQL` / `ManagedDatabase` datastore Kinds** (`operator/src/controllers/sql.rs`,
   `managed_database.rs`) — thin facades over the canonical `DatastoreSpec`
   (`reconcile_datastore_inner`). Each reconciles to a StatefulSet `<name>`, a
   ClusterIP Service `<name>`, a headless Service `<name>-hs`, and a per-replica
   PVC (`volumeClaimTemplate` `data`) — the **same machinery `docdb` uses**. This
   is where **path A** plugs in: keep the Kind, swap the image. See `deploy/sql.yaml`.

### Per-tenant layout

- **One PVC, many files** (path C, `dir_mode`): `/data/<tenant>.db` (or
  `<org>/<tenant>.db`). Cheapest; best for many small tenants. WAL→S3 per file.
- **One pod + PVC per tenant** (path A / paid isolated `ManagedDatabase`): one CR
  → one pglite pod → one Service → one `postgres://` endpoint. Strong isolation,
  no bespoke router. pglite is single-writer per data dir, which matches this.

### How a tenant is provisioned on-demand

Declaratively, via a CR (the operator reconciles it) — never `kubectl set image`,
never hand-rolled. Path C: a `Service` CR with `persistence` (+ `dir_mode` for
many tenants). Path A: one `SQL`/`ManagedDatabase` CR per isolated tenant. The
imperative dev/CI counterpart for path A is `pgwire/provision.mjs` (inits a tenant
data dir, prints the DATABASE_URL).

### How it is exposed + how DATABASE_URL reaches a consumer

- **Service DNS**: `<name>.<ns>.svc.cluster.local` (path A pg-wire on `:5432`;
  serviceAliases mint extra Services, e.g. `postgres`). Path C has no network
  endpoint — the file is local to the pod.
- **DATABASE_URL handoff** mirrors docdb's `MONGO_URI` exactly: the operator does
  **not** inject it. The connection string is synced from KMS via a `KMSSecret` CR
  → k8s Secret → consumer env `DATABASE_URL` `valueFrom.secretKeyRef`. Path C's
  value is a literal `file:/data/<app>.db` (usually just set inline in the CR
  `env`); path A's value is `postgresql://postgres@<name>.<ns>.svc:5432/postgres?schema=public`.

## Image + tags

- Path A pg-wire endpoint: **`ghcr.io/hanzoai/sql-managed`**, semver tags only
  (`v0.1.0`, …) — **never `:latest`, never `sha-…`**. Built by CI on
  `hanzo-build-linux-amd64` via `hanzoai/.github` `docker-build.yml` (linux/amd64).
  Base `ghcr.io/hanzoai/nodejs:v24.18.0`. See `pgwire/Dockerfile`.
- Path C reuses each consuming app's own image + the `hanzoai/replicate` sidecar.
- The legacy `ghcr.io/hanzoai/sql:18` (Postgres source fork) is the thing being
  retired; do not build new tags of it.

## Data migration (the real blocker) — Postgres → SQLite ETL

Killing the shared Postgres means moving existing rows into the per-tenant SQLite
files. The canonical ETL is `esign/scripts/backfill-pg-to-sqlite.ts` (idempotent,
resumable keyset copy, exact Prisma-on-SQLite codec). It had **one real bug**:
node-pg returns **enum arrays** (`Role[]`) as the raw string `{ADMIN,USER}`, which
aborts the load — and `User.roles` is populated for every user. Fixed in
`etl/enum-array-parser.patch` (register node-pg's text-array parser for every
enum-array OID). Full recipe + type-coercion table: `etl/README.md`.

Validated end-to-end on **synthetic** data (no prod data): real Postgres 18
source running esign's pre-conversion schema, 4554 seeded rows exercising every
pitfall (enum-arrays, `text[]`, `bytea`, `bigint` > 2^53, `timestamptz`+null,
`jsonb`, boolean, scalar enums) → SQLite via the patched script → **row-count
parity** + **byte-exact type fidelity** + read-back through esign's own Prisma
client (`emailVerified` as `Date`, `credentialId` as `Uint8Array`, `counter` as
lossless `bigint`, `data` as parsed JSON). Evidence:
`evidence/etl-pg-to-sqlite-validation.log`.

Schema-drift verdict: against esign's own pre-conversion Postgres schema the copy
is structurally 1:1 (conversion commit `e055e76df` added/removed/renamed **zero**
tables/columns/enums — only datasource + type-attributes changed). The ETL
assumes matching table+column names on both sides; confirm the source is the
fork's own Postgres, not a structurally-diverged upstream.

## Open items / blockers

1. **`fsGroup` gap for path A.** `DatastoreSpec` has no `fsGroup` (only the
   `Service` Kind does). The nonroot `sql-managed` image (uid 65532) can't write a
   root-owned DO-block PVC. Fix = add `fsGroup` to `DatastoreSpec`
   (`operator/src/crd.rs`) and set `65532` in `deploy/sql.yaml`. Until then, host
   the pglite endpoint on the `Service` Kind (has `fsGroup` + `persistence`) or
   run the image as root. Path C is unaffected (uses `Service.fsGroup`).
2. **Multi-tenant pg-wire router (path A optimization).** `pgwire/server.mjs`
   serves one tenant per process (proven: enum + `text[]` + `gen_random_uuid`
   roundtrip, `evidence/pgwire-scaffold-smoke.log`). Fronting many tenant files
   from one process (route on the pg-wire startup `database` param → per-tenant
   `PGLiteSocketHandler`) + real auth (SCRAM verifier, not trust) are the
   remaining work before A serves untrusted external tenants. Not built (per
   scope); pod-per-tenant is the baseline and needs no router.
3. **`credentialsSecret` is inert** in `DatastoreSpec` (operator never reads it) —
   use `env` / `envFrom`. Documented so nobody wires a password expecting it to work.

## Files here

    LLM.md                                  this spec
    deploy/sql.yaml                         path A — SQL datastore CR (pglite endpoint)
    deploy/service-persistence.example.yaml path C — Service CR with persistence (esign pattern)
    etl/README.md                           pg→SQLite migration recipe + coercion table
    etl/enum-array-parser.patch             the required fix to esign's backfill script
    pgwire/server.mjs                        path A prototype — pg-wire over pglite (tested)
    pgwire/provision.mjs                     path A tenant provisioning helper (DATABASE_URL emitter)
    pgwire/{Dockerfile,package.json}         ghcr.io/hanzoai/sql-managed:v0.1.0 (amd64)
    evidence/                                raw prisma / ETL / smoke output backing every claim above
