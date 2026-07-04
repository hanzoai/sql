// Hanzo SQL (pg-wire) — multi-tenant Postgres-wire endpoint over pglite.
//
// One TCP listener fronts MANY tenant databases. Each tenant DB is a single
// embedded-Postgres data directory under HANZO_SQL_DATA (/data/<dbname>),
// provisioned on first connect. pglite IS Postgres compiled to WASM — full
// dialect — so an unmodified Prisma `provider = "postgresql"` schema runs with
// NO rewrite. Analogue of docdb (FerretDB → Mongo-wire per tenant).
//
// The router TERMINATES the pg wire protocol and drives pglite's
// `execProtocolRaw` directly (it does NOT proxy to @electric-sql/pglite-socket,
// whose per-message forwarding emits premature ReadyForQuery and hangs strict
// clients like asyncpg, and which crashes pglite on `COPY … FROM stdin`).
//
// Per connection:
//   - Peek the startup packet's `database`, resolve/create the tenant pglite
//     instance, then run the session against it.
//   - The startup message + every frontend message batch → `db.execProtocolRaw`.
//     Messages are BATCHED to each Sync/Flush/simple-Query boundary so pipelined
//     extended-protocol prepares (Parse+Describe+Sync) are processed atomically
//     — the fix that makes asyncpg's named-prepared-statement cache work.
//   - `COPY … FROM stdin` is INTERCEPTED (it crashes pglite over the wire) and
//     executed via pglite's programmatic COPY (`FROM '/dev/blob'`); the wire
//     responses (CopyInResponse/CommandComplete/ReadyForQuery) are synthesized.
//     `COPY … TO stdout` needs no special handling (works over the raw wire).
//   - Per-db serialization with transaction affinity: pglite is ONE session per
//     data dir, so concurrent connections share a serialized lane and a
//     transaction's statements are kept together.
//
// Verified clients: node-postgres/Prisma, psql (incl. COPY), asyncpg. See
// LLM.md + evidence/.

import { createServer } from 'node:net'
import { mkdirSync } from 'node:fs'
import { join } from 'node:path'
import { PGlite } from '@electric-sql/pglite'

const DATA_ROOT = process.env.HANZO_SQL_DATA || '/data'
const HOST = process.env.HANZO_SQL_HOST || '0.0.0.0'
// HANZO_SQL_LISTEN_PORT is the canonical name — it can't collide with a k8s
// service-link env (`<SVC>_PORT=tcp://…`). We still read the legacy
// HANZO_SQL_PORT, but ONLY if it parses as an integer, so a leaked
// `HANZO_SQL_PORT=tcp://10.0.0.1:5432` never becomes NaN and crashes the boot.
const PORT = firstPort(process.env.HANZO_SQL_LISTEN_PORT, process.env.HANZO_SQL_PORT, 5432)
const DEBUG = process.env.HANZO_SQL_DEBUG === '1'

function firstPort(...vals) {
  for (const v of vals) {
    const n = Number(v)
    if (Number.isInteger(n) && n > 0 && n < 65536) return n
  }
  return 5432
}

const SSL_REQUEST_CODE = 80877103
const GSSENC_REQUEST_CODE = 80877104
const CANCEL_REQUEST_CODE = 80877102
const PROTOCOL_3_0 = 196608
// A tenant dbname is a filesystem path segment: constrain hard (defeat traversal;
// mirror the 63-byte pg identifier limit).
const DB_NAME_RE = /^[A-Za-z0-9_][A-Za-z0-9_-]{0,62}$/
// Only intercept a statement that STARTS with COPY … FROM stdin (so a COPY
// appearing inside a string literal is never misdetected).
const COPY_FROM_STDIN = /^\s*copy\b[\s\S]*?\bfrom\s+stdin\b/i
const rewriteCopyFromStdin = (sql) => sql.replace(/\bfrom\s+stdin\b/i, "FROM '/dev/blob'")

const log = (...a) => console.log('[hanzo-sql]', ...a)
const dbg = (...a) => { if (DEBUG) console.log('[hanzo-sql:dbg]', ...a) }

mkdirSync(DATA_ROOT, { recursive: true })

// ── per-db serial executor with transaction affinity ───────────────────────
// pglite is a single backend session per data dir; serialize all protocol calls
// and, while a transaction is open, only run messages from its owning connection.
class Lane {
  constructor(db) { this.db = db; this.q = []; this.busy = false; this.owner = null }
  run(connId, job) { return new Promise((res, rej) => { this.q.push({ connId, job, res, rej }); this._pump() }) }
  async _pump() {
    if (this.busy) return
    this.busy = true
    try {
      while (this.q.length) {
        let item
        if (this.db.isInTransaction() && this.owner != null) {
          const i = this.q.findIndex((x) => x.connId === this.owner)
          if (i < 0) break // wait for the transaction owner's next message
          item = this.q.splice(i, 1)[0]
        } else {
          item = this.q.shift()
        }
        try { item.res(await item.job(this.db)) } catch (e) { item.rej(e) }
        this.owner = this.db.isInTransaction() ? item.connId : null
      }
    } finally { this.busy = false }
  }
  async release(connId) {
    this.q = this.q.filter((x) => (x.connId === connId ? (x.rej(new Error('closed')), false) : true))
    if (this.owner === connId) {
      this.owner = null
      try { if (this.db.isInTransaction()) await this.db.exec('ROLLBACK') } catch {}
    }
    this._pump()
  }
}

// ── tenant cache (Promise-cache: exactly one instance per dir) ──────────────
const tenants = new Map()
function getTenant(dbname) {
  const hit = tenants.get(dbname)
  if (hit) return hit
  const created = (async () => {
    const dataDir = join(DATA_ROOT, dbname)
    mkdirSync(dataDir, { recursive: true })
    const db = await PGlite.create(dataDir)
    await db.waitReady
    log(`provisioned tenant db=${dbname} dir=${dataDir}`)
    return { db, lane: new Lane(db) }
  })()
  tenants.set(dbname, created)
  created.catch((e) => { tenants.delete(dbname); log(`provision FAILED db=${dbname}:`, e?.message || e) })
  return created
}

// ── wire response builders ──────────────────────────────────────────────────
function msg(typeChar, body) {
  const out = Buffer.alloc(5 + body.length)
  out[0] = typeChar.charCodeAt(0)
  out.writeInt32BE(4 + body.length, 1)
  body.copy(out, 5)
  return out
}
const readyForQuery = (s) => msg('Z', Buffer.from(s)) // 'I' idle | 'T' in txn | 'E' failed
const commandComplete = (tag) => msg('C', Buffer.from(tag + '\0'))
const copyInResponse = () => msg('G', Buffer.from([0, 0, 0])) // text format, 0 columns
function errorResponse(code, message) {
  const body = Buffer.concat([
    Buffer.from('SFATAL\0'), Buffer.from('VFATAL\0'),
    Buffer.from(`C${code}\0`), Buffer.from(`M${message}\0`), Buffer.from([0]),
  ])
  return msg('E', body)
}

function parseStartupDatabase(body) {
  const parts = []
  let start = 0
  for (let i = 0; i < body.length; i++) { if (body[i] === 0) { parts.push(body.toString('utf8', start, i)); start = i + 1 } }
  const p = {}
  for (let i = 0; i + 1 < parts.length; i += 2) { if (parts[i] === '') break; p[parts[i]] = parts[i + 1] }
  return p.database || p.user || null
}

// ── per-connection named-object namespacing ─────────────────────────────────
// pglite is ONE backend session per data dir, but prepared statements and
// portals are session-scoped in Postgres (each real connection has its own).
// Without isolation, connection B reusing a name that connection A prepared
// (e.g. asyncpg's `__asyncpg_stmt_1__` across a pool) collides:
// "prepared statement already exists". We prefix every NON-EMPTY statement/
// portal name in the extended-protocol messages (Parse/Bind/Describe/Close/
// Execute) with a per-connection tag, giving each connection its own namespace
// on the shared session. Unnamed ('') stay unnamed. Backend replies carry no
// statement name, so no reverse rewrite is needed.
const NUL = Buffer.from([0])
function cstr(buf, off) { const e = buf.indexOf(0, off); return [buf.subarray(off, e), e + 1] }
function rewriteNamed(message, pfx) {
  const ns = (name) => (name.length === 0 ? name : Buffer.concat([pfx, name]))
  const type = message[0]
  const body = message.subarray(5)
  if (type === 0x50) { // Parse: stmt, query, rest
    const [stmt, o1] = cstr(body, 0)
    return msg('P', Buffer.concat([ns(stmt), NUL, body.subarray(o1)]))
  }
  if (type === 0x42) { // Bind: portal, stmt, rest
    const [portal, o1] = cstr(body, 0)
    const [stmt, o2] = cstr(body, o1)
    return msg('B', Buffer.concat([ns(portal), NUL, ns(stmt), NUL, body.subarray(o2)]))
  }
  if (type === 0x44 || type === 0x43) { // Describe/Close: kind byte, name
    const [name, o1] = cstr(body, 1)
    return msg(String.fromCharCode(type), Buffer.concat([body.subarray(0, 1), ns(name), NUL, body.subarray(o1)]))
  }
  if (type === 0x45) { // Execute: portal, maxRows
    const [portal, o1] = cstr(body, 0)
    return msg('E', Buffer.concat([ns(portal), NUL, body.subarray(o1)]))
  }
  return message
}

let connSeq = 0
function handleConnection(socket) {
  socket.setNoDelay(true)
  const connId = ++connSeq
  const namePfx = Buffer.from(`\x01hz${connId}\x01`) // per-connection stmt/portal namespace
  let buf = Buffer.alloc(0)
  let tenant = null
  let started = false // startup message consumed
  let inCopy = false
  let copySql = null
  let copyChunks = []
  let pending = []
  let closed = false

  socket.on('error', (e) => dbg(`conn#${connId} error:`, e?.message || e))
  socket.on('close', () => { closed = true; if (tenant) tenant.lane.release(connId) })

  const write = (b) => { if (!closed && socket.writable) socket.write(b) }
  const fatal = (code, m) => { write(errorResponse(code, m)); try { socket.end() } catch {} }
  const exec = (batch) => tenant.lane.run(connId, async (db) => {
    const resp = await db.execProtocolRaw(new Uint8Array(batch))
    if (resp && resp.length) write(Buffer.from(resp))
  })
  const flushPending = () => { if (pending.length) { const b = Buffer.concat(pending); pending = []; exec(b) } }
  const enqueueWrite = (...bufs) => tenant.lane.run(connId, async () => { for (const b of bufs) write(b) })
  const runCopyIn = (sql, data) => tenant.lane.run(connId, async (db) => {
    try {
      const res = await db.query(sql, [], { blob: new Blob([data]) })
      write(commandComplete(`COPY ${res.affectedRows ?? 0}`))
    } catch (e) {
      write(errorResponse('22000', `COPY failed: ${e?.message || e}`))
    }
    write(readyForQuery(db.isInTransaction() ? 'T' : 'I'))
  })

  socket.on('data', (chunk) => {
    buf = Buffer.concat([buf, chunk])
    try { drive() } catch (e) { log(`conn#${connId} drive error:`, e?.message || e); try { socket.destroy() } catch {} }
  })

  function drive() {
    if (!started) {
      while (buf.length >= 8) {
        const len = buf.readInt32BE(0), code = buf.readInt32BE(4)
        if (len === 8 && (code === SSL_REQUEST_CODE || code === GSSENC_REQUEST_CODE)) { write(Buffer.from('N')); buf = buf.subarray(8); continue }
        if (len === 16 && code === CANCEL_REQUEST_CODE) { started = true; try { socket.end() } catch {}; return }
        break
      }
      if (buf.length < 8) return
      const len = buf.readInt32BE(0)
      if (len < 8 || len > (1 << 20)) throw new Error(`bad startup length ${len}`)
      if (buf.length < len) return
      if (buf.readInt32BE(4) !== PROTOCOL_3_0) throw new Error('unsupported protocol version')
      const dbname = parseStartupDatabase(buf.subarray(8, len))
      const startupBytes = Buffer.from(buf.subarray(0, len))
      buf = buf.subarray(len)
      started = true
      if (!dbname || !DB_NAME_RE.test(dbname)) return fatal('3D000', `invalid database name: ${dbname || '(none)'}`)
      getTenant(dbname).then((t) => {
        if (closed) return
        tenant = t
        exec(startupBytes) // auth + ParameterStatus + BackendKeyData + ReadyForQuery
        driveSession()
      }).catch((e) => fatal('08006', `backend unavailable: ${e?.message || e}`))
      return
    }
    if (tenant) driveSession()
  }

  function driveSession() {
    while (buf.length >= 5) {
      const type = buf[0]
      const len = buf.readInt32BE(1)
      const total = 1 + len
      if (buf.length < total) break
      const message = Buffer.from(buf.subarray(0, total))
      const body = buf.subarray(5, total)
      buf = buf.subarray(total)

      if (type === 0x58) { // 'X' Terminate — close THIS connection; never forward to the shared session
        flushPending()
        tenant.lane.run(connId, async () => { try { socket.end() } catch {} }) // ends after in-flight responses flush
        return
      }

      if (inCopy) {
        if (type === 0x64) copyChunks.push(Buffer.from(body))                                     // CopyData
        else if (type === 0x63) { const data = Buffer.concat(copyChunks); copyChunks = []; inCopy = false; const sql = copySql; copySql = null; runCopyIn(sql, data) } // CopyDone
        else if (type === 0x66) { copyChunks = []; inCopy = false; copySql = null; enqueueWrite(errorResponse('57014', 'COPY from stdin aborted by client'), readyForQuery('I')) } // CopyFail
        continue
      }

      if (type === 0x51) { // 'Q' simple query
        const sql = body.subarray(0, body.length - 1).toString('utf8')
        if (COPY_FROM_STDIN.test(sql)) {
          flushPending()
          inCopy = true
          copySql = rewriteCopyFromStdin(sql)
          copyChunks = []
          enqueueWrite(copyInResponse())
          continue
        }
        pending.push(message)
        flushPending() // simple Query is self-syncing
        continue
      }
      pending.push(rewriteNamed(message, namePfx)) // isolate named stmts/portals per connection
      if (type === 0x53 || type === 0x48) flushPending() // Sync | Flush -> process the batch
    }
  }
}

const listener = createServer(handleConnection)
listener.on('error', (e) => { log('listener error:', e); process.exitCode = 1 })
listener.listen(PORT, HOST, () => log(`pg-wire router on ${HOST}:${PORT} data=${DATA_ROOT}`))

async function shutdown(sig) {
  log(`${sig}: draining`)
  listener.close()
  for (const [name, p] of tenants) { try { const t = await p; await t.db.close(); dbg(`closed ${name}`) } catch {} }
  process.exit(0)
}
process.on('SIGTERM', () => shutdown('SIGTERM'))
process.on('SIGINT', () => shutdown('SIGINT'))
