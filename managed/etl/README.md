# Hanzo SQL — Postgres → SQLite ETL

The canonical, one-way ETL lives with the consuming app, not here (the app owns
its schema and its array-column list). The reference implementation is:

    /Users/z/work/hanzo/esign/scripts/backfill-pg-to-sqlite.ts

It is idempotent, resumable (keyset-paginated, `INSERT OR IGNORE`), loads under
`foreign_keys=OFF`, and applies the exact Prisma-on-SQLite storage codec:

| Postgres source          | SQLite target (Prisma 6 codec) |
|--------------------------|--------------------------------|
| `boolean`                | `INTEGER` 0 / 1                |
| `timestamp`/`timestamptz`| `INTEGER` epoch-**milliseconds** (`Date.getTime()`) |
| `json` / `jsonb`         | `TEXT` (verbatim JSON)         |
| `text[]` / enum `_e[]`   | `TEXT` (JSON array)            |
| `bytea`                  | `BLOB`                         |
| `numeric` (Decimal)      | `REAL` (via NUMERIC affinity)  |
| `bigint`                 | `INTEGER` (int64, lossless)    |
| enum label               | `TEXT` (verbatim)              |

## enum-array-parser.patch (REQUIRED)

`node-pg` parses built-in `text[]` into a JS array, but a column typed as an
**array of a custom enum** (`Role[]`, `WebhookTriggerEvents[]`) has a dynamic OID
node-pg has no parser for — it returns the raw literal `{ADMIN,USER}` as a string,
which trips the script's fail-closed array guard and **aborts the whole load**.
`User.roles` is populated for every user, so the unpatched script cannot migrate
real `sign` data. The patch registers node-pg's own text-array parser for every
enum-array OID (5 lines).

    cd /Users/z/work/hanzo/esign
    git apply /Users/z/work/hanzo/sql/managed/etl/enum-array-parser.patch

Validated end-to-end on synthetic data (real Postgres 18 source, esign schema,
4554 rows across all pitfall types → SQLite → esign Prisma client read-back with
full row parity and byte-exact type fidelity). See
`../evidence/etl-pg-to-sqlite-validation.log`.

## Run

    PG_URL=postgres://user:pass@host:5432/sign \
    SQLITE_PATH=/data/sign.db \
    MIGRATION_SQL=packages/prisma/migrations/0_init/migration.sql \
    node scripts/backfill-pg-to-sqlite.ts --fresh --batch=1000

Constraint: table + column names must match between the Postgres source and the
SQLite target (only type-attributes may differ). This holds for a fork that
converted its own schema pg→sqlite (esign: zero tables/columns/enums
added/removed/renamed). It does **not** hold against a structurally-diverged
upstream — confirm the source is the fork's own pre-conversion Postgres.
