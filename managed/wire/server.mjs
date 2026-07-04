// Hanzo SQL (pg-wire) — multi-tenant Postgres-wire endpoint over pglite.
//
// One TCP listener on :5432 fronts MANY tenant databases. Each tenant DB is a
// single embedded-Postgres data directory under HANZO_SQL_DATA (/data/<dbname>).
// On connect we PEEK the pg startup packet's `database` field, resolve (or
// lazily create, provision-on-demand) that tenant's pglite instance, and proxy
// the wire session to it. pglite IS Postgres compiled to WASM — full dialect
// (enums, arrays, timestamptz, gen_random_uuid, bytea, citext) — so an
// unmodified Prisma `provider = "postgresql"` schema runs with NO rewrite.
//
// Design (minimal + DRY): each tenant gets its own PGLiteSocketServer bound to a
// private Unix socket; the library owns the wire protocol, the per-db query
// serialization (QueryQueueManager) and multi-connection handling. This router
// is only a startup-peeking TCP -> per-tenant-Unix-socket dispatcher. We reuse
// the library's tested logic instead of reimplementing its (unexported) queue.
//
// Isolation: one data dir per dbname, one pglite writer per dir. Concurrent
// connections to the SAME db share that db's serialized queue (correct MVCC via
// pglite). Two different dbnames => two dirs => fully isolated data.

import { createServer, connect as netConnect } from 'node:net'
import { mkdirSync } from 'node:fs'
import { join } from 'node:path'
import { tmpdir } from 'node:os'
import { PGlite } from '@electric-sql/pglite'
import { PGLiteSocketServer } from '@electric-sql/pglite-socket'

const DATA_ROOT = process.env.HANZO_SQL_DATA || '/data'
const HOST = process.env.HANZO_SQL_HOST || '0.0.0.0'
const PORT = Number(process.env.HANZO_SQL_PORT || 5432)
const SOCK_DIR = process.env.HANZO_SQL_SOCKDIR || join(tmpdir(), 'hanzo-sql')
const MAX_CONN = Number(process.env.HANZO_SQL_MAX_CONN || 1000)
const DEBUG = process.env.HANZO_SQL_DEBUG === '1'

// pg wire startup constants (protocol v3).
const SSL_REQUEST_CODE = 80877103
const GSSENC_REQUEST_CODE = 80877104
const CANCEL_REQUEST_CODE = 80877102
const PROTOCOL_3_0 = 196608

// A tenant dbname is used as a filesystem path segment: constrain hard to defeat
// path traversal. Postgres identifiers are <= 63 bytes; we mirror that.
const DB_NAME_RE = /^[A-Za-z0-9_][A-Za-z0-9_-]{0,62}$/

const log = (...a) => console.log('[hanzo-sql]', ...a)
const dbg = (...a) => { if (DEBUG) console.log('[hanzo-sql:dbg]', ...a) }

// dbname -> Promise<{ sockPath }>. A Promise cache so concurrent first
// connections to the same NEW db share a single instance (never two writers on
// one dir).
const tenants = new Map()

mkdirSync(DATA_ROOT, { recursive: true })
mkdirSync(SOCK_DIR, { recursive: true })

/** Lazily create + start the per-tenant pglite-backed pg-wire server. */
function getTenant(dbname) {
  let existing = tenants.get(dbname)
  if (existing) return existing
  const created = (async () => {
    const dataDir = join(DATA_ROOT, dbname)
    mkdirSync(dataDir, { recursive: true })
    const db = await PGlite.create(dataDir)
    await db.waitReady
    const sockPath = join(SOCK_DIR, `${dbname}.sock`)
    const server = new PGLiteSocketServer({
      db,
      path: sockPath,
      maxConnections: MAX_CONN,
      debug: DEBUG,
    })
    await server.start()
    log(`provisioned tenant db=${dbname} dir=${dataDir}`)
    return { db, server, sockPath }
  })()
  tenants.set(dbname, created)
  created.catch((err) => {
    // Failed provision must not poison the cache forever.
    tenants.delete(dbname)
    log(`provision FAILED db=${dbname}:`, err?.message || err)
  })
  return created
}

/** Build a FATAL ErrorResponse packet ('E'). */
function errorResponse(code, message) {
  const body = Buffer.concat([
    Buffer.from(`S${'FATAL'}\0`, 'utf8'),
    Buffer.from(`C${code}\0`, 'utf8'),
    Buffer.from(`M${message}\0`, 'utf8'),
    Buffer.from([0]),
  ])
  const out = Buffer.alloc(5 + body.length)
  out[0] = 0x45 // 'E'
  out.writeInt32BE(body.length + 4, 1)
  body.copy(out, 5)
  return out
}

/** Extract the `database` (fallback `user`) param from a StartupMessage body. */
function parseStartupDatabase(body) {
  // body = null-terminated key\0value\0 ... \0 (after the 4-byte protocol field)
  const parts = []
  let start = 0
  for (let i = 0; i < body.length; i++) {
    if (body[i] === 0) {
      parts.push(body.toString('utf8', start, i))
      start = i + 1
    }
  }
  const params = {}
  for (let i = 0; i + 1 < parts.length; i += 2) {
    if (parts[i] === '') break
    params[parts[i]] = parts[i + 1]
  }
  return params.database || params.user || null
}

function handleConnection(socket) {
  socket.setNoDelay(true)
  let buf = Buffer.alloc(0)
  let dispatched = false

  const onError = (err) => {
    dbg('client socket error during peek:', err?.message || err)
  }
  socket.on('error', onError)

  const onData = (chunk) => {
    if (dispatched) return
    buf = Buffer.concat([buf, chunk])
    try {
      tryDispatch()
    } catch (err) {
      log('startup peek error:', err?.message || err)
      try { socket.destroy() } catch {}
    }
  }
  socket.on('data', onData)

  function tryDispatch() {
    // Strip any leading SSL/GSSENC negotiation packets; answer 'N' (no TLS —
    // in-cluster traffic is on the pod network). Clients (Prisma default
    // sslmode=prefer, psql) then send the real StartupMessage in the clear.
    while (buf.length >= 8) {
      const len = buf.readInt32BE(0)
      const code = buf.readInt32BE(4)
      if (len === 8 && (code === SSL_REQUEST_CODE || code === GSSENC_REQUEST_CODE)) {
        if (socket.writable) socket.write(Buffer.from('N'))
        buf = buf.subarray(8)
        continue
      }
      if (len === 16 && code === CANCEL_REQUEST_CODE) {
        // Query cancellation targets another backend; pglite has none. Close.
        dispatched = true
        socket.removeListener('data', onData)
        try { socket.end() } catch {}
        return
      }
      break
    }
    // Need the full StartupMessage: int32 length + int32 protocol, then params.
    if (buf.length < 8) return
    const len = buf.readInt32BE(0)
    if (len < 8 || len > 1 << 20) throw new Error(`bad startup length ${len}`)
    if (buf.length < len) return // wait for the rest
    const protocol = buf.readInt32BE(4)
    if (protocol !== PROTOCOL_3_0) throw new Error(`unsupported protocol ${protocol}`)

    const dbname = parseStartupDatabase(buf.subarray(8, len))
    dispatched = true
    socket.removeListener('data', onData)
    // Keep onError attached across the async getTenant() await below — a client
    // reset during provisioning must not throw an unhandled 'error'.
    socket.pause()

    if (!dbname || !DB_NAME_RE.test(dbname)) {
      if (socket.writable) {
        socket.write(errorResponse('3D000', `invalid database name: ${dbname || '(none)'}`))
      }
      try { socket.end() } catch {}
      return
    }

    // buf currently holds the StartupMessage (+ any pipelined bytes). Forward it
    // verbatim to the tenant backend, then splice the two sockets together.
    const startupBytes = buf
    getTenant(dbname).then(({ sockPath }) => {
      const backend = netConnect(sockPath)
      backend.setNoDelay(true)
      const teardown = () => { try { socket.destroy() } catch {}; try { backend.destroy() } catch {} }
      backend.on('error', (err) => { dbg(`backend db=${dbname} error:`, err?.message || err); teardown() })
      socket.on('error', (err) => { dbg(`client db=${dbname} error:`, err?.message || err); teardown() })
      backend.on('connect', () => {
        backend.write(startupBytes)
        socket.pipe(backend)
        backend.pipe(socket)
        dbg(`spliced client -> db=${dbname}`)
      })
    }).catch((err) => {
      log(`dispatch failed db=${dbname}:`, err?.message || err)
      if (socket.writable) socket.write(errorResponse('08006', `backend unavailable: ${err?.message || err}`))
      try { socket.end() } catch {}
    })
  }
}

const listener = createServer(handleConnection)
listener.on('error', (err) => { log('listener error:', err); process.exitCode = 1 })
listener.listen(PORT, HOST, () => {
  log(`pg-wire router on ${HOST}:${PORT} data=${DATA_ROOT} sockdir=${SOCK_DIR} maxConnPerDb=${MAX_CONN}`)
})

async function shutdown(sig) {
  log(`${sig}: draining`)
  listener.close()
  for (const [name, p] of tenants) {
    try {
      const t = await p
      await t.server.stop()
      await t.db.close()
      dbg(`closed tenant db=${name}`)
    } catch (err) {
      dbg(`shutdown db=${name} error:`, err?.message || err)
    }
  }
  process.exit(0)
}
process.on('SIGTERM', () => shutdown('SIGTERM'))
process.on('SIGINT', () => shutdown('SIGINT'))
