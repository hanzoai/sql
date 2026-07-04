// Hanzo SQL — tenant provisioning helper (pg-wire path).
//
// Declarative provisioning is the operator: one `ManagedDatabase`/`SQL` CR per
// tenant reconciles to a pglite pod + PVC + Service `<tenant>.<ns>.svc:5432`.
// This helper is the imperative dev/CI counterpart and the DATABASE_URL emitter
// the operator status would otherwise carry: it pre-initialises a tenant's data
// directory (so first connect is not cold) and prints the connection string a
// consumer receives (synced into a k8s Secret via a KMSSecret CR, then injected
// as env `DATABASE_URL` valueFrom secretKeyRef — same mechanism docdb uses for
// MONGO_URI).
//
//   node provision.mjs <tenant> [--data /data] [--svc <tenant>.hanzo.svc] [--port 5432]
import { PGlite } from '@electric-sql/pglite'
import { mkdirSync } from 'node:fs'
import { join } from 'node:path'

const args = process.argv.slice(2)
const tenant = args[0]
if (!tenant || tenant.startsWith('--')) {
  console.error('usage: node provision.mjs <tenant> [--data DIR] [--svc HOST] [--port N]')
  process.exit(2)
}
const opt = (name, def) => {
  const i = args.indexOf(name)
  return i >= 0 && args[i + 1] ? args[i + 1] : def
}
const dataRoot = opt('--data', '/data')
const svc = opt('--svc', `${tenant}.hanzo.svc.cluster.local`)
const port = opt('--port', '5432')

const dir = join(dataRoot, tenant)
mkdirSync(dir, { recursive: true })
const db = await PGlite.create(dir)
await db.waitReady
await db.close()

// pglite runs a single superuser role `postgres`, no password (auth is network
// isolation + one-tenant-per-endpoint). Prisma needs `?schema=public`.
const url = `postgresql://postgres@${svc}:${port}/postgres?schema=public`
console.log(url)
