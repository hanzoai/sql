// Hanzo SQL — pg-wire endpoint over embedded Postgres (pglite), one tenant per process.
//
// This is the OPTION-A follow-up path (see ../LLM.md): a real `postgres://`
// endpoint for a single tenant whose data lives in one embedded Postgres data
// directory on a PVC. pglite is Postgres compiled to WASM — FULL pg dialect
// (enums, arrays, gen_random_uuid, citext, timestamptz, bytea), so an unmodified
// Prisma `provider = "postgresql"` schema runs against it with no rewrite. It is
// the endpoint for consumers that cannot adopt the file:SQLite path (schemas we
// cannot rewrite, or external ManagedDatabase customers who demand pg-wire).
//
// One process serves ONE tenant DB. Many tenants = many pods, each its own CR +
// PVC + Service (the operator's per-CR StatefulSet/Service machinery — no bespoke
// router). Fronting many tenant files from one process is a later optimization
// (routing on the pg-wire startup `database` param to a per-tenant handler); it
// is intentionally NOT built here.
//
// pglite is single-connection/single-writer by construction; that matches the
// per-tenant embedded model. It is not a replacement for a shared multi-writer
// cluster.
import { PGlite } from '@electric-sql/pglite'
import { PGLiteSocketServer } from '@electric-sql/pglite-socket'
import { mkdirSync } from 'node:fs'

const dataDir = process.env.HANZO_SQL_DATA || '/data/db'
const host = process.env.HANZO_SQL_HOST || '0.0.0.0'
const port = Number(process.env.HANZO_SQL_PORT || 5432)

mkdirSync(dataDir, { recursive: true }) // pglite mkdirs only the leaf, not parents
const db = await PGlite.create(dataDir)
await db.waitReady

const server = new PGLiteSocketServer({ db, host, port })
await server.start()
console.log(`[hanzo-sql] pg-wire on ${host}:${port} data=${dataDir}`)

const shutdown = async (sig) => {
  console.log(`[hanzo-sql] ${sig}: draining`)
  try { await server.stop() } finally { await db.close() }
  process.exit(0)
}
process.on('SIGTERM', () => shutdown('SIGTERM'))
process.on('SIGINT', () => shutdown('SIGINT'))
