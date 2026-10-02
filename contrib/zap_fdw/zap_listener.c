/*
 * zap_listener.c — canonical ZAP-HTTP listener for hanzo/sql.
 *
 * Registers a pool of background workers that answer the ZAP-HTTP transport
 * (github.com/zap-proto/http) on a configurable port (default 9651): each reads
 * length-prefixed request frames, routes by path to SPI (/query, /exec) or the
 * KV layer (/get, /set, /del), and writes a length-prefixed response frame.
 * This lets a hanzoai/orm ZAP client run SQL against Postgres with no sidecar.
 *
 * Two pools of workers serve the listener. Both start only once recovery is
 * finished, so a standby neither listens nor answers:
 *
 *   statement workers (zap.workers) each bind the port with SO_REUSEPORT, so
 *   the kernel spreads connections across them, and serve many connections
 *   each, a request at a time, each request in its own transaction that commits
 *   BEFORE the reply — a client told a write succeeded holds a committed write;
 *
 *   transaction workers (zap.tx_workers) each serve one connection. A
 *   statement worker that reads /begin hands the connection to the next free
 *   one, passing its descriptor over a datagram socket the postmaster opened
 *   for them, so a transaction never holds up anyone else's statements.
 *
 * /begin {"isolation": "serializable" | "repeatable read" | "read committed"}
 * opens a transaction; every /query and /exec on the connection then runs
 * inside it until /commit or /rollback. A statement that fails aborts it, and
 * the connection then refuses everything but /rollback. A transaction left idle
 * past zap.tx_idle_timeout is rolled back and its connection closed, as is one
 * whose client goes away.
 *
 * Errors carry the SQLSTATE: {"error": "...", "sqlstate": "40001"}. A
 * serialization failure or deadlock (40001, 40P01) answers 409, which a client
 * retries as a whole transaction; any other failure answers 500.
 */
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "access/xact.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"
#include "utils/timestamp.h"
#include "storage/pmsignal.h"
#include "lib/stringinfo.h"
#include "libpq/pqsignal.h"

#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "zap_protocol.h"

PG_MODULE_MAGIC;

#define ZAP_MAX_FRAME (64u << 20)   /* matches zap-proto/http MaxFrameSize */

/* GUCs */
static bool zap_enabled = false;
static int  zap_port = 9651;
static int  zap_workers = 4;
static char *zap_database = NULL;
static int  zap_tx_workers = 8;
static int  zap_tx_idle_timeout = 10000;    /* ms */
static int  zap_max_conns = 256;            /* per statement worker */

/* The two ends of the hand-off socket, opened by the postmaster in _PG_init and
 * inherited by every worker it forks. */
static int  zap_handoff[2] = {-1, -1};      /* [0] statement workers send, [1] transaction workers receive */

static volatile sig_atomic_t got_sigterm = false;

void _PG_init(void);
PGDLLEXPORT void zap_worker_main(Datum main_arg);
static void zap_sigterm_handler(SIGNAL_ARGS);

/* SQL execution (zap_spi.c) and KV layer (zap_kv.c) */
extern char *zap_sql_query(const char *body, int body_len, uint32_t *status);
extern char *zap_sql_exec(const char *body, int body_len, uint32_t *status);
extern void  zap_ensure_tables(void);
extern void  zap_append_json_string(StringInfo buf, const char *s);
extern char *zap_kv_get(const char *key);
extern char *zap_kv_set(const char *key, const char *value, const char *kind);
extern char *zap_kv_del(const char *key);

/*
 * Extract a string value for a key from a flat JSON object {"k":"v",...}.
 * Returns a palloc'd string or NULL. Used by the KV paths.
 */
static char *
json_extract_string(const char *json, const char *key)
{
    char search[256];
    const char *p, *start, *end;
    size_t len;
    char *result;

    snprintf(search, sizeof(search), "\"%s\"", key);
    p = strstr(json, search);
    if (!p)
        return NULL;
    p += strlen(search);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':')
        p++;
    if (*p != '"')
        return NULL;
    p++;
    start = p;
    while (*p && !(*p == '"' && *(p - 1) != '\\'))
        p++;
    if (*p != '"')
        return NULL;
    end = p;
    len = end - start;
    result = palloc(len + 1);
    memcpy(result, start, len);
    result[len] = '\0';
    return result;
}

/* ---- length-prefixed frame I/O (shutdown-aware) ---- */

/* Read exactly n bytes, waking on the latch so shutdown is prompt. Returns 0 on
 * success, -1 on EOF, error, or shutdown. */
static int
read_fully(int fd, uint8_t *buf, uint32_t n)
{
    uint32_t got = 0;

    while (got < n)
    {
        int rc = WaitLatchOrSocket(MyLatch,
                                   WL_LATCH_SET | WL_SOCKET_READABLE | WL_EXIT_ON_PM_DEATH,
                                   fd, -1L, PG_WAIT_EXTENSION);
        ResetLatch(MyLatch);
        if (got_sigterm)
            return -1;
        if (rc & WL_SOCKET_READABLE)
        {
            ssize_t r = recv(fd, buf + got, n - got, 0);
            if (r > 0)
                got += (uint32_t) r;
            else if (r == 0)
                return -1;      /* peer closed */
            else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                return -1;
        }
    }
    return 0;
}

/* Read one length-prefixed frame into a palloc'd buffer. Returns 0, or -1 on
 * close/shutdown/oversize. */
static int
read_frame(int fd, uint8_t **frame, uint32_t *flen)
{
    uint8_t hdr[4];
    uint32_t n;

    if (read_fully(fd, hdr, 4) < 0)
        return -1;
    n = zap_rd_u32be(hdr);
    if (n < ZAP_HEADER_SIZE || n > ZAP_MAX_FRAME)
        return -1;
    *frame = palloc(n);
    if (read_fully(fd, *frame, n) < 0)
        return -1;
    *flen = n;
    return 0;
}

static int
write_all(int fd, const uint8_t *buf, uint32_t n)
{
    uint32_t sent = 0;

    while (sent < n)
    {
        ssize_t w = send(fd, buf + sent, n - sent, MSG_NOSIGNAL);
        if (w > 0)
            sent += (uint32_t) w;
        else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            return -1;
    }
    return 0;
}

/* Build [BE length][response frame] for status + JSON body. palloc'd. */
static uint8_t *
build_response(uint32_t status, const char *body, uint32_t body_len, uint32_t *outlen)
{
    uint32_t frame_len = ZAP_HEADER_SIZE + ZAP_RESP_SLOTSIZE + body_len;
    uint32_t total = 4 + frame_len;
    uint8_t *out = palloc0(total);
    uint8_t *f = out + 4;
    uint32_t root = ZAP_ROOT_OFFSET;

    zap_wr_u32be(out, frame_len);

    memcpy(f, ZAP_MAGIC, 4);
    zap_wr_u16(f + 4, 1);                                    /* version */
    zap_wr_u16(f + 6, (uint16_t) (ZAP_FRAME_RESPONSE << 8)); /* flags: type<<8 */
    zap_wr_u32(f + 8, ZAP_ROOT_OFFSET);                      /* rootOffset */
    zap_wr_u32(f + 12, frame_len);                           /* size */

    zap_wr_u16(f + root + ZAP_RESP_STATUS, (uint16_t) status);
    if (body_len > 0)
    {
        uint32_t slot = root + ZAP_RESP_BODY;
        uint32_t data_start = root + ZAP_RESP_SLOTSIZE;
        zap_wr_u32(f + slot, data_start - slot);
        zap_wr_u32(f + slot + 4, body_len);
        memcpy(f + data_start, body, body_len);
    }

    *outlen = total;
    return out;
}

/* One accepted connection and the transaction it holds open, if any. */
typedef struct ZapConn
{
    int     fd;
    bool    in_tx;      /* a client transaction is open on this connection */
    bool    failed;     /* a statement failed inside it; only /rollback is accepted */
    TimestampTz last;   /* when its last frame arrived */
} ZapConn;

/* Run a request's SQL or KV operation. Called inside a transaction with an
 * active snapshot. The result is palloc'd in the current (transaction) context. */
static char *
run_request(const char *path, const uint8_t *body, uint32_t body_len, uint32_t *status)
{
    char *result = NULL;

    *status = 200;
    if (strcmp(path, "/query") == 0 && body)
        result = zap_sql_query((const char *) body, body_len, status);
    else if (strcmp(path, "/exec") == 0 && body)
        result = zap_sql_exec((const char *) body, body_len, status);
    else if (strcmp(path, "/get") == 0 && body)
    {
        char *json = palloc(body_len + 1), *key;
        memcpy(json, body, body_len);
        json[body_len] = '\0';
        key = json_extract_string(json, "key");
        if (key)
        {
            result = zap_kv_get(key);
            if (!result) { result = "{\"error\":\"not found\"}"; *status = 404; }
        }
        else { result = "{\"error\":\"key required\"}"; *status = 400; }
    }
    else if (strcmp(path, "/set") == 0 && body)
    {
        char *json = palloc(body_len + 1), *key, *val, *kind;
        memcpy(json, body, body_len);
        json[body_len] = '\0';
        key = json_extract_string(json, "key");
        val = json_extract_string(json, "value");
        kind = json_extract_string(json, "kind");
        if (key && val)
            result = zap_kv_set(key, val, kind ? kind : "");
        else { result = "{\"error\":\"key and value required\"}"; *status = 400; }
    }
    else if (strcmp(path, "/del") == 0 && body)
    {
        char *json = palloc(body_len + 1), *key;
        memcpy(json, body, body_len);
        json[body_len] = '\0';
        key = json_extract_string(json, "key");
        if (key)
            result = zap_kv_del(key);
        else { result = "{\"error\":\"key required\"}"; *status = 400; }
    }
    else
    {
        result = "{\"error\":\"unknown path\"}";
        *status = 404;
    }
    if (!result)
    {
        result = "{\"error\":\"no result\"}";
        *status = 500;
    }
    return result;
}

/* Reply with status and a JSON body built in ctx, which outlives the transaction. */
static int
reply(int fd, MemoryContext ctx, uint32_t status, const char *body)
{
    MemoryContext old = MemoryContextSwitchTo(ctx);
    uint32_t len;
    uint8_t *resp = build_response(status, body, (uint32_t) strlen(body), &len);
    int rc;

    MemoryContextSwitchTo(old);
    rc = write_all(fd, resp, len);
    pfree(resp);
    return rc;
}

/* The error a caught ERROR answers with: 409 for a serialization failure or a
 * deadlock, which a client retries; 500 otherwise. Called in PG_CATCH, before
 * the transaction is aborted. Builds the body in ctx. */
static char *
caught_error(MemoryContext ctx, const char *path, uint32_t *status)
{
    ErrorData *edata;
    StringInfoData buf;
    const char *state;

    MemoryContextSwitchTo(ctx);
    edata = CopyErrorData();
    FlushErrorState();
    state = unpack_sql_state(edata->sqlerrcode);
    *status = (edata->sqlerrcode == ERRCODE_T_R_SERIALIZATION_FAILURE ||
               edata->sqlerrcode == ERRCODE_T_R_DEADLOCK_DETECTED) ? 409 : 500;
    elog(LOG, "zap: %s failed: %s (%s)", path[0] ? path : "(none)", edata->message, state);
    initStringInfo(&buf);
    appendStringInfoString(&buf, "{\"error\":");
    zap_append_json_string(&buf, edata->message ? edata->message : "query failed");
    appendStringInfo(&buf, ",\"sqlstate\":\"%s\"}", state);
    FreeErrorData(edata);
    return buf.data;
}

/* The isolation a /begin body names, or -1 for one that is not an isolation. */
static int
parse_isolation(const uint8_t *body, uint32_t body_len)
{
    char *iso = NULL;

    if (body && body_len > 0)
    {
        char *json = palloc(body_len + 1);
        memcpy(json, body, body_len);
        json[body_len] = '\0';
        iso = json_extract_string(json, "isolation");
    }
    if (iso == NULL || strcmp(iso, "read committed") == 0)
        return XACT_READ_COMMITTED;
    if (strcmp(iso, "repeatable read") == 0)
        return XACT_REPEATABLE_READ;
    if (strcmp(iso, "serializable") == 0)
        return XACT_SERIALIZABLE;
    return -1;
}

/* Begin a client transaction at level and acknowledge it. */
static int
begin_tx(ZapConn *c, MemoryContext ctx, int level)
{
    if (c->in_tx || c->failed)
        return reply(c->fd, ctx, 409, "{\"error\":\"a transaction is already open on this connection\"}");
    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();
    XactIsoLevel = level;
    c->in_tx = true;
    c->failed = false;
    return reply(c->fd, ctx, 200, "{\"ok\":true}");
}

/* Commit the connection's transaction, answering 409 if it cannot be. */
static int
commit_tx(ZapConn *c, MemoryContext ctx)
{
    uint32_t status = 200;
    char *body = "{\"ok\":true}";

    if (c->failed)
    {
        c->failed = false;
        return reply(c->fd, ctx, 409, "{\"error\":\"the transaction failed and was rolled back\"}");
    }
    if (!c->in_tx)
        return reply(c->fd, ctx, 400, "{\"error\":\"no transaction is open on this connection\"}");

    c->in_tx = false;
    PG_TRY();
    {
        CommitTransactionCommand();
    }
    PG_CATCH();
    {
        body = caught_error(ctx, "/commit", &status);
        AbortCurrentTransaction();
    }
    PG_END_TRY();
    MemoryContextSwitchTo(ctx);
    return reply(c->fd, ctx, status, body);
}

/* Roll back the connection's transaction, if one is open. */
static void
abort_tx(ZapConn *c)
{
    if (c->in_tx)
        AbortCurrentTransaction();
    c->in_tx = false;
    c->failed = false;
}

/* Run one statement inside the connection's open transaction and reply. A
 * failure aborts the transaction and leaves the connection failed. */
static int
tx_request(ZapConn *c, MemoryContext ctx, const char *path, const uint8_t *body, uint32_t body_len)
{
    uint32_t status = 200;
    char *result = NULL;
    char *out = NULL;

    if (c->failed)
        return reply(c->fd, ctx, 409, "{\"error\":\"the transaction failed; roll it back\"}");

    PG_TRY();
    {
        SetCurrentStatementStartTimestamp();
        PushActiveSnapshot(GetTransactionSnapshot());
        result = run_request(path, body, body_len, &status);
        PopActiveSnapshot();
        CommandCounterIncrement();
        MemoryContextSwitchTo(ctx);
        out = pstrdup(result);
    }
    PG_CATCH();
    {
        out = caught_error(ctx, path, &status);
        AbortCurrentTransaction();
        c->in_tx = false;
        c->failed = true;
    }
    PG_END_TRY();
    MemoryContextSwitchTo(ctx);
    return reply(c->fd, ctx, status, out);
}

/* Run one request in its own transaction, commit it, then reply. */
static int
single_request(ZapConn *c, MemoryContext ctx, const char *path, const uint8_t *body, uint32_t body_len)
{
    uint32_t status = 200;
    char *result = NULL;
    char *out = NULL;

    PG_TRY();
    {
        SetCurrentStatementStartTimestamp();
        StartTransactionCommand();
        PushActiveSnapshot(GetTransactionSnapshot());
        result = run_request(path, body, body_len, &status);
        PopActiveSnapshot();
        MemoryContextSwitchTo(ctx);
        out = pstrdup(result);
        CommitTransactionCommand();
    }
    PG_CATCH();
    {
        out = caught_error(ctx, path, &status);
        AbortCurrentTransaction();
    }
    PG_END_TRY();
    MemoryContextSwitchTo(ctx);
    return reply(c->fd, ctx, status, out);
}

/* What a statement worker does with a frame it cannot answer itself. */
typedef enum
{
    FRAME_DONE,         /* answered */
    FRAME_CLOSE,        /* close the connection */
    FRAME_HANDOFF       /* /begin: give the connection to a transaction worker */
} FrameOutcome;

/* Decode one request frame and answer it. A statement worker (tx_worker false)
 * answers /begin with FRAME_HANDOFF and *level set; a transaction worker opens
 * the transaction itself. */
static FrameOutcome
handle_frame(ZapConn *c, MemoryContext ctx, const uint8_t *frame, uint32_t flen,
             bool tx_worker, int *level)
{
    uint32_t root, size, path_len, body_len;
    const uint8_t *path_b, *body_b;
    char path[128];
    int rc;

    if (zap_frame_root(frame, flen, ZAP_FRAME_REQUEST, &root, &size) < 0)
        return reply(c->fd, ctx, 400, "{\"error\":\"bad frame\"}") < 0 ? FRAME_CLOSE : FRAME_DONE;

    path_b = zap_read_var(frame, size, root, ZAP_REQ_TARGET, &path_len);
    body_b = zap_read_var(frame, size, root, ZAP_REQ_BODY, &body_len);

    if (path_b)
    {
        uint32_t n = path_len < sizeof(path) - 1 ? path_len : sizeof(path) - 1;
        memcpy(path, path_b, n);
        path[n] = '\0';
    }
    else
        path[0] = '\0';

    if (strcmp(path, "/begin") == 0)
    {
        *level = parse_isolation(body_b, body_len);
        if (*level < 0)
            rc = reply(c->fd, ctx, 400, "{\"error\":\"isolation is read committed, repeatable read or serializable\"}");
        else if (!tx_worker)
            return FRAME_HANDOFF;
        else
            rc = begin_tx(c, ctx, *level);
    }
    else if (strcmp(path, "/commit") == 0)
        rc = commit_tx(c, ctx);
    else if (strcmp(path, "/rollback") == 0)
    {
        abort_tx(c);
        rc = reply(c->fd, ctx, 200, "{\"ok\":true}");
    }
    else if (c->in_tx || c->failed)
        rc = tx_request(c, ctx, path, body_b, body_len);
    else
        rc = single_request(c, ctx, path, body_b, body_len);
    return rc < 0 ? FRAME_CLOSE : FRAME_DONE;
}

/* Read and answer one frame from c. */
static FrameOutcome
serve_frame(ZapConn *c, MemoryContext ctx, bool tx_worker, int *level)
{
    uint8_t *frame;
    uint32_t flen;
    FrameOutcome out;

    MemoryContextSwitchTo(ctx);
    if (read_frame(c->fd, &frame, &flen) < 0)
        return FRAME_CLOSE;
    c->last = GetCurrentTimestamp();
    out = handle_frame(c, ctx, frame, flen, tx_worker, level);
    MemoryContextSwitchTo(ctx);
    MemoryContextReset(ctx);
    return out;
}

/* ---- hand-off: a connection and its isolation, one datagram ---- */

/* Send fd and level to the next free transaction worker. The datagram is queued
 * if none is free; the descriptor travels with it. Returns 0 or -1. */
static int
handoff_send(int fd, int level)
{
    struct msghdr msg;
    struct iovec iov;
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cm;
    int32_t payload = level;

    memset(&msg, 0, sizeof(msg));
    memset(cbuf, 0, sizeof(cbuf));
    iov.iov_base = &payload;
    iov.iov_len = sizeof(payload);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);
    cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    for (;;)
    {
        if (sendmsg(zap_handoff[0], &msg, 0) >= 0)
            return 0;
        if (errno != EINTR)
            return -1;
    }
}

/* Receive a handed-off connection, or -1 when none is waiting. */
static int
handoff_recv(int *level)
{
    struct msghdr msg;
    struct iovec iov;
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cm;
    int32_t payload = 0;
    int fd = -1;

    memset(&msg, 0, sizeof(msg));
    iov.iov_base = &payload;
    iov.iov_len = sizeof(payload);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);
    if (recvmsg(zap_handoff[1], &msg, MSG_DONTWAIT) < (ssize_t) sizeof(payload))
        return -1;
    for (cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm))
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS)
            memcpy(&fd, CMSG_DATA(cm), sizeof(int));
    *level = payload;
    return fd;
}

/* Close c, rolling back any transaction it left open. */
static void
close_conn(ZapConn *c)
{
    abort_tx(c);
    close(c->fd);
    c->fd = -1;
}

static const char *
zap_resolve_database(void)
{
    const char *db;

    if (zap_database && zap_database[0] != '\0')
        return zap_database;
    db = getenv("POSTGRES_DB");
    if (db && db[0] != '\0')
        return db;
    return "postgres";
}

/* Connect the worker to its database and make sure the tables exist. */
static void
zap_worker_init(void)
{
    pqsignal(SIGTERM, zap_sigterm_handler);
    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnection(zap_resolve_database(), NULL, 0);

    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();
    PushActiveSnapshot(GetTransactionSnapshot());
    zap_ensure_tables();
    PopActiveSnapshot();
    CommitTransactionCommand();
}

/* A statement worker: accepts from the shared port, answers many connections a
 * request at a time, and hands a connection that opens a transaction away. */
/* Bind the port for this statement worker. */
static int
zap_listen(void)
{
    struct sockaddr_in addr;
    int opt = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        ereport(ERROR, (errmsg("zap: socket() failed: %m")));
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(zap_port);
    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
        ereport(ERROR, (errmsg("zap: bind() port %d failed: %m", zap_port)));
    if (listen(fd, 1024) < 0)
        ereport(ERROR, (errmsg("zap: listen() failed: %m")));
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
        ereport(ERROR, (errmsg("zap: fcntl(O_NONBLOCK) failed: %m")));
    return fd;
}

static void
statement_loop(void)
{
    int zap_listen_fd = zap_listen();
    MemoryContext msgctx = AllocSetContextCreate(TopMemoryContext, "zap message", ALLOCSET_DEFAULT_SIZES);
    ZapConn *conns = palloc0(sizeof(ZapConn) * zap_max_conns);
    struct pollfd *pfds = palloc0(sizeof(struct pollfd) * (zap_max_conns + 1));
    int nconns = 0;

    while (!got_sigterm)
    {
        int nfds = 0, i, rc;

        CHECK_FOR_INTERRUPTS();
        if (!PostmasterIsAlive())
            proc_exit(1);

        pfds[nfds].fd = nconns < zap_max_conns ? zap_listen_fd : -1;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;
        for (i = 0; i < nconns; i++)
        {
            pfds[nfds].fd = conns[i].fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        rc = poll(pfds, nfds, 200);
        if (rc <= 0)
            continue;

        if (pfds[0].revents & POLLIN)
        {
            int fd = accept(zap_listen_fd, NULL, NULL);
            if (fd >= 0)
            {
                if (fcntl(fd, F_SETFL, O_NONBLOCK) == 0)
                {
                    conns[nconns].fd = fd;
                    conns[nconns].in_tx = false;
                    conns[nconns].failed = false;
                    conns[nconns].last = GetCurrentTimestamp();
                    nconns++;
                }
                else
                    close(fd);
            }
        }
        for (i = 1; i < nfds; i++)
        {
            ZapConn *c = &conns[i - 1];
            int level = 0;

            if (c->fd < 0 || !(pfds[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            switch (serve_frame(c, msgctx, false, &level))
            {
                case FRAME_DONE:
                    break;
                case FRAME_HANDOFF:
                    if (handoff_send(c->fd, level) < 0)
                    {
                        int e = errno;
                        elog(WARNING, "zap: hand-off failed: %s", strerror(e));
                    }
                    /* The transaction worker holds its own copy now. */
                    close(c->fd);
                    c->fd = -1;
                    break;
                case FRAME_CLOSE:
                    close(c->fd);
                    c->fd = -1;
                    break;
            }
        }
        for (i = 0; i < nconns;)
        {
            if (conns[i].fd < 0)
                conns[i] = conns[--nconns];
            else
                i++;
        }
    }
    for (int i = 0; i < nconns; i++)
        close(conns[i].fd);
    close(zap_listen_fd);
}

/* A transaction worker: takes one handed-off connection at a time, opens its
 * transaction, and serves the connection until the client closes it. */
static void
transaction_loop(void)
{
    MemoryContext msgctx = AllocSetContextCreate(TopMemoryContext, "zap message", ALLOCSET_DEFAULT_SIZES);

    while (!got_sigterm)
    {
        ZapConn c;
        int level = 0;
        int rc;

        CHECK_FOR_INTERRUPTS();
        rc = WaitLatchOrSocket(MyLatch,
                               WL_LATCH_SET | WL_SOCKET_READABLE | WL_EXIT_ON_PM_DEATH,
                               zap_handoff[1], -1L, PG_WAIT_EXTENSION);
        ResetLatch(MyLatch);
        if (got_sigterm)
            break;
        if (!(rc & WL_SOCKET_READABLE))
            continue;
        c.fd = handoff_recv(&level);
        if (c.fd < 0)
            continue;   /* another worker took it */
        if (fcntl(c.fd, F_SETFL, O_NONBLOCK) != 0)
        {
            close(c.fd);
            continue;
        }
        c.in_tx = false;
        c.failed = false;
        c.last = GetCurrentTimestamp();

        MemoryContextSwitchTo(msgctx);
        if (begin_tx(&c, msgctx, level) < 0)
        {
            close_conn(&c);
            MemoryContextReset(msgctx);
            continue;
        }
        MemoryContextReset(msgctx);

        /* Serve the connection until it closes, idles out, or we shut down. */
        while (!got_sigterm && c.fd >= 0)
        {
            struct pollfd pfd;
            long waited = (long) ((GetCurrentTimestamp() - c.last) / 1000);
            int next = 0;

            CHECK_FOR_INTERRUPTS();
            if (!PostmasterIsAlive())
                proc_exit(1);
            if (waited >= zap_tx_idle_timeout)
            {
                if (c.in_tx)
                    elog(LOG, "zap: transaction idle %ld ms; rolled back", waited);
                close_conn(&c);
                break;
            }
            pfd.fd = c.fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (poll(&pfd, 1, (int) Min(200, zap_tx_idle_timeout - waited)) <= 0)
                continue;
            if (serve_frame(&c, msgctx, true, &next) == FRAME_CLOSE)
                close_conn(&c);
        }
        if (c.fd >= 0)
            close_conn(&c);
    }
}

void
zap_worker_main(Datum main_arg)
{
    bool tx_worker = DatumGetInt32(main_arg) >= zap_workers;

    zap_worker_init();
    elog(LOG, "zap: %s worker serving port %d", tx_worker ? "transaction" : "statement", zap_port);
    if (tx_worker)
        transaction_loop();
    else
        statement_loop();
    elog(LOG, "zap: listener shutting down");
}

static void
zap_sigterm_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    got_sigterm = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

/* Open the hand-off socket in the postmaster, so every worker it forks shares
 * the one queue. */
static void
zap_open_handoff(void)
{
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, zap_handoff) < 0)
        ereport(FATAL, (errmsg("zap: socketpair() failed: %m")));
}

void
_PG_init(void)
{
    int i;

    DefineCustomBoolVariable("zap.enabled",
                             "Enable the ZAP-HTTP listener background workers.",
                             NULL, &zap_enabled, false,
                             PGC_POSTMASTER, 0, NULL, NULL, NULL);
    DefineCustomIntVariable("zap.port",
                            "Port the ZAP-HTTP listener binds.",
                            NULL, &zap_port, 9651, 1, 65535,
                            PGC_POSTMASTER, 0, NULL, NULL, NULL);
    DefineCustomIntVariable("zap.workers",
                            "ZAP-HTTP statement workers: each answers many connections, a request at a time.",
                            NULL, &zap_workers, 4, 1, 64,
                            PGC_POSTMASTER, 0, NULL, NULL, NULL);
    DefineCustomIntVariable("zap.tx_workers",
                            "ZAP-HTTP transaction workers: each serves one connection holding a transaction.",
                            NULL, &zap_tx_workers, 8, 1, 256,
                            PGC_POSTMASTER, 0, NULL, NULL, NULL);
    DefineCustomStringVariable("zap.database",
                               "Database the ZAP-HTTP listener connects to (default $POSTGRES_DB).",
                               NULL, &zap_database, NULL,
                               PGC_POSTMASTER, 0, NULL, NULL, NULL);
    DefineCustomIntVariable("zap.tx_idle_timeout",
                            "Milliseconds a transaction connection may sit idle before it is closed and its transaction rolled back.",
                            NULL, &zap_tx_idle_timeout, 10000, 100, 3600000,
                            PGC_POSTMASTER, GUC_UNIT_MS, NULL, NULL, NULL);
    DefineCustomIntVariable("zap.max_connections",
                            "Connections one ZAP-HTTP statement worker serves at once.",
                            NULL, &zap_max_conns, 256, 1, 4096,
                            PGC_POSTMASTER, 0, NULL, NULL, NULL);

    if (!process_shared_preload_libraries_in_progress || !zap_enabled)
        return;

    zap_open_handoff();

    for (i = 0; i < zap_workers + zap_tx_workers; i++)
    {
        BackgroundWorker worker;

        memset(&worker, 0, sizeof(worker));
        worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
        worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
        worker.bgw_restart_time = 5;
        snprintf(worker.bgw_library_name, BGW_MAXLEN, "zap_fdw");
        snprintf(worker.bgw_function_name, BGW_MAXLEN, "zap_worker_main");
        snprintf(worker.bgw_name, BGW_MAXLEN, "zap %s %d",
                 i < zap_workers ? "statement" : "transaction", i);
        snprintf(worker.bgw_type, BGW_MAXLEN, "zap listener");
        worker.bgw_main_arg = Int32GetDatum(i);
        RegisterBackgroundWorker(&worker);
    }
    elog(LOG, "zap: %d statement and %d transaction worker(s) on port %d",
         zap_workers, zap_tx_workers, zap_port);
}
