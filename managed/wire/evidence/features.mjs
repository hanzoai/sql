import pg from 'pg'
const { Client } = pg
const t = async (label, sql) => {
  const c = new Client({ connectionString: `postgresql://hanzo:pw@127.0.0.1:55432/probe` })
  await c.connect()
  try { await c.query(sql); console.log('OK  ', label) }
  catch (e) { console.log('FAIL', label, '->', String(e.message).split('\n')[0]) }
  finally { await c.end() }
}
await t('pgcrypto ext', 'CREATE EXTENSION IF NOT EXISTS pgcrypto')
await t('uuid-ossp ext', 'CREATE EXTENSION IF NOT EXISTS "uuid-ossp"')
await t('pg_trgm ext', 'CREATE EXTENSION IF NOT EXISTS pg_trgm')
await t('vector ext', 'CREATE EXTENSION IF NOT EXISTS vector')
await t('citext ext', 'CREATE EXTENSION IF NOT EXISTS citext')
await t('gen_random_uuid builtin', 'SELECT gen_random_uuid()')
await t('generated column', 'CREATE TABLE g(a int, b int GENERATED ALWAYS AS (a+1) STORED)')
await t('partitioned table', 'CREATE TABLE p(a int) PARTITION BY RANGE(a)')
await t('LISTEN/NOTIFY', 'LISTEN chan')
await t('materialized view', 'CREATE MATERIALIZED VIEW mv AS SELECT 1 AS x')
await t('full text search', "SELECT to_tsvector('hello world') @@ to_tsquery('hello')")
await t('json/jsonb ops', "SELECT '{\"a\":1}'::jsonb -> 'a'")
await t('window function', 'SELECT row_number() OVER (ORDER BY 1)')
