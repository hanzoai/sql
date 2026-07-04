// Round-trip + multi-tenant isolation proof, driven entirely over pg-wire (TCP)
// through the Hanzo SQL router. Uses node-postgres (real Postgres client).
import pg from 'pg'
const { Client } = pg

const PORT = 55432
const HOST = '127.0.0.1'
const url = (db) => `postgresql://hanzo:pw@${HOST}:${PORT}/${db}`

async function q(db, sql, params) {
  const c = new Client({ connectionString: url(db) })
  await c.connect()
  try { return await c.query(sql, params) } finally { await c.end() }
}

console.log('=== 1. dataroomdb: migrated schema is present (post migrate deploy) ===')
const applied = await q('dataroomdb', 'SELECT count(*)::int AS n FROM _prisma_migrations WHERE finished_at IS NOT NULL')
console.log('applied migrations recorded:', applied.rows[0].n)
const tbls = await q('dataroomdb', "SELECT count(*)::int AS n FROM information_schema.tables WHERE table_schema='public'")
console.log('public tables in dataroomdb:', tbls.rows[0].n)
const enums = await q('dataroomdb', 'SELECT count(*)::int AS n FROM pg_type WHERE typtype = %L'.replace('%L', "'e'"))
console.log('pg ENUM types in dataroomdb:', enums.rows[0].n)

console.log('\n=== 2. full pg-dialect CRUD round-trip on tenant_alpha (fresh db) ===')
await q('tenant_alpha', `CREATE TYPE mood AS ENUM ('happy','sad')`)
await q('tenant_alpha', `CREATE TABLE t (
  id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  tags text[] NOT NULL,
  m mood NOT NULL,
  blob bytea,
  meta jsonb,
  created timestamptz NOT NULL DEFAULT now()
)`)
await q('tenant_alpha', `INSERT INTO t (tags, m, blob, meta) VALUES ($1, $2, $3, $4)`,
  [['a', 'b', 'c'], 'happy', Buffer.from('hi'), { k: 1 }])
const r = await q('tenant_alpha', `SELECT tags, m, meta, blob, (id IS NOT NULL) AS has_uuid FROM t`)
console.log('row:', JSON.stringify({
  tags: r.rows[0].tags, m: r.rows[0].m, meta: r.rows[0].meta,
  blob: r.rows[0].blob.toString(), has_uuid: r.rows[0].has_uuid,
}))
const app = await q('tenant_alpha', `SELECT array_append(tags,'d') AS appended FROM t`)
console.log('array_append:', JSON.stringify(app.rows[0].appended))

console.log('\n=== 3. multi-tenant ISOLATION: tenant_beta is a separate db/dir ===')
await q('tenant_beta', `CREATE TABLE t (id int PRIMARY KEY, who text)`)
await q('tenant_beta', `INSERT INTO t (id, who) VALUES (1, 'beta-only')`)
const beta = await q('tenant_beta', `SELECT who FROM t`)
console.log('tenant_beta.t.who:', beta.rows[0].who)

// alpha.t has a DIFFERENT shape (uuid/tags/mood) than beta.t (int/who) -> isolated.
const alphaCols = await q('tenant_alpha', "SELECT string_agg(column_name, ',' ORDER BY ordinal_position) AS cols FROM information_schema.columns WHERE table_name='t'")
const betaCols = await q('tenant_beta', "SELECT string_agg(column_name, ',' ORDER BY ordinal_position) AS cols FROM information_schema.columns WHERE table_name='t'")
console.log("tenant_alpha.t columns:", alphaCols.rows[0].cols)
console.log("tenant_beta.t  columns:", betaCols.rows[0].cols)

// dataroom's tables must NOT be visible from tenant_beta
const leak = await q('tenant_beta', "SELECT count(*)::int AS n FROM information_schema.tables WHERE table_name='_prisma_migrations'")
console.log('dataroom _prisma_migrations visible from tenant_beta? count =', leak.rows[0].n, leak.rows[0].n === 0 ? '(ISOLATED)' : '(LEAK!)')

console.log('\n=== 4. persistence note: data dirs on disk ===')
console.log('OK')
