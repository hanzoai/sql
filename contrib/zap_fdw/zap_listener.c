/*
 * zap_listener.c — canonical ZAP-HTTP listener for hanzo/sql.
 *
 * Registers a pool of background workers that answer the ZAP-HTTP transport
 * (github.com/zap-proto/http) on a configurable port (default 9651): each reads
 * length-prefixed request frames, routes by path to SPI (/query, /exec) or the
 * KV layer (/get, /set, /del), and writes a length-prefixed response frame.
 * This lets a hanzoai/orm ZAP client run SQL against Postgres with no sidecar.
 *
 * Each worker owns one connection at a time; the pool (zap.workers) binds the
 * same port with SO_REUSEPORT so the kernel spreads the client's pooled
 * connections across workers. Each request runs in its own transaction with an
 * active snapshot, and its reply is sent before the commit frees the buffer.
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
#include "libpq/pqsignal.h"

#include <signal.h>
#include <fcntl.h>
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

static volatile sig_atomic_t got_sigterm = false;

void _PG_init(void);
PGDLLEXPORT void zap_worker_main(Datum main_arg);
static void zap_sigterm_handler(SIGNAL_ARGS);

/* SQL execution (zap_spi.c) and KV layer (zap_kv.c) */
extern char *zap_sql_query(const char *body, int body_len, uint32_t *status);
extern char *zap_sql_exec(const char *body, int body_len, uint32_t *status);
extern void  zap_ensure_tables(void);
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

/* Route a decoded request to SQL or KV, inside a transaction, and reply. */
static void
dispatch(int fd, const char *path, const uint8_t *body, uint32_t body_len)
{
    MemoryContext ctx = CurrentMemoryContext;
    char *result = NULL;
    uint32_t status = 200;
    uint8_t *resp;
    uint32_t resp_len;

    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();
    PushActiveSnapshot(GetTransactionSnapshot());

    PG_TRY();
    {
    if (strncmp(path, "/query", 6) == 0 && body)
        result = zap_sql_query((const char *) body, body_len, &status);
    else if (strncmp(path, "/exec", 5) == 0 && body)
        result = zap_sql_exec((const char *) body, body_len, &status);
    else if (strncmp(path, "/get", 4) == 0 && body)
    {
        char *json = palloc(body_len + 1), *key;
        memcpy(json, body, body_len);
        json[body_len] = '\0';
        key = json_extract_string(json, "key");
        if (key)
        {
            result = zap_kv_get(key);
            if (!result) { result = "{\"error\":\"not found\"}"; status = 404; }
        }
        else { result = "{\"error\":\"key required\"}"; status = 400; }
    }
    else if (strncmp(path, "/set", 4) == 0 && body)
    {
        char *json = palloc(body_len + 1), *key, *val, *kind;
        memcpy(json, body, body_len);
        json[body_len] = '\0';
        key = json_extract_string(json, "key");
        val = json_extract_string(json, "value");
        kind = json_extract_string(json, "kind");
        if (key && val)
            result = zap_kv_set(key, val, kind ? kind : "");
        else { result = "{\"error\":\"key and value required\"}"; status = 400; }
    }
    else if (strncmp(path, "/del", 4) == 0 && body)
    {
        char *json = palloc(body_len + 1), *key;
        memcpy(json, body, body_len);
        json[body_len] = '\0';
        key = json_extract_string(json, "key");
        if (key)
            result = zap_kv_del(key);
        else { result = "{\"error\":\"key required\"}"; status = 400; }
    }
    else
    {
        result = "{\"error\":\"unknown path\"}";
        status = 404;
    }

    if (!result)
    {
        result = "{\"error\":\"no result\"}";
        status = 500;
    }

    /* Reply before the commit frees result/resp. */
    resp = build_response(status, result, (uint32_t) strlen(result), &resp_len);
    write_all(fd, resp, resp_len);

    PopActiveSnapshot();
    CommitTransactionCommand();
    }
    PG_CATCH();
    {
        ErrorData *edata;

        MemoryContextSwitchTo(ctx);
        edata = CopyErrorData();
        FlushErrorState();
        AbortCurrentTransaction();
        elog(LOG, "zap: %s failed: %s", path[0] ? path : "(none)", edata->message);
        FreeErrorData(edata);

        MemoryContextSwitchTo(ctx);
        resp = build_response(500, "{\"error\":\"query failed\"}",
                              (uint32_t) strlen("{\"error\":\"query failed\"}"), &resp_len);
        write_all(fd, resp, resp_len);
    }
    PG_END_TRY();
}

/* Decode one request frame and dispatch it. */
static void
handle_frame(int fd, const uint8_t *frame, uint32_t flen)
{
    uint32_t root, size, path_len, body_len;
    const uint8_t *path_b, *body_b;
    char path[128];

    if (zap_frame_root(frame, flen, ZAP_FRAME_REQUEST, &root, &size) < 0)
    {
        uint32_t rl;
        uint8_t *resp = build_response(400, "{\"error\":\"bad frame\"}",
                                       (uint32_t) strlen("{\"error\":\"bad frame\"}"), &rl);
        write_all(fd, resp, rl);
        return;
    }

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

    dispatch(fd, path, body_b, body_len);
}

/* Serve one connection: read frames until the peer closes or we shut down. */
static void
serve_connection(int fd)
{
    MemoryContext msgctx = AllocSetContextCreate(TopMemoryContext,
                                                 "zap message",
                                                 ALLOCSET_DEFAULT_SIZES);
    MemoryContext old = CurrentMemoryContext;

    for (;;)
    {
        uint8_t *frame;
        uint32_t flen;

        MemoryContextSwitchTo(msgctx);
        if (got_sigterm || read_frame(fd, &frame, &flen) < 0)
            break;
        handle_frame(fd, frame, flen);
        MemoryContextSwitchTo(msgctx);
        MemoryContextReset(msgctx);
    }

    MemoryContextSwitchTo(old);
    MemoryContextDelete(msgctx);
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

void
zap_worker_main(Datum main_arg)
{
    int server_fd;
    struct sockaddr_in addr;
    const char *dbname;
    int opt = 1;

    pqsignal(SIGTERM, zap_sigterm_handler);
    BackgroundWorkerUnblockSignals();

    dbname = zap_resolve_database();
    BackgroundWorkerInitializeConnection(dbname, NULL, 0);

    /* Provision the entity + KV tables (in a transaction with a snapshot). */
    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();
    PushActiveSnapshot(GetTransactionSnapshot());
    zap_ensure_tables();
    PopActiveSnapshot();
    CommitTransactionCommand();

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        int e = errno;
        elog(ERROR, "zap: socket() failed: %s", strerror(e));
        return;
    }
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(zap_port);

    if (bind(server_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
    {
        int e = errno;
        elog(ERROR, "zap: bind() port %d failed: %s", zap_port, strerror(e));
        close(server_fd);
        return;
    }
    if (listen(server_fd, 128) < 0)
    {
        int e = errno;
        elog(ERROR, "zap: listen() failed: %s", strerror(e));
        close(server_fd);
        return;
    }
    if (fcntl(server_fd, F_SETFL, O_NONBLOCK) < 0)
    {
        int e = errno;
        elog(ERROR, "zap: fcntl(O_NONBLOCK) failed: %s", strerror(e));
        close(server_fd);
        return;
    }

    elog(LOG, "zap: listening on port %d", zap_port);

    while (!got_sigterm)
    {
        int client_fd;
        int rc = WaitLatchOrSocket(MyLatch,
                                   WL_LATCH_SET | WL_SOCKET_READABLE | WL_EXIT_ON_PM_DEATH,
                                   server_fd, -1L, PG_WAIT_EXTENSION);
        ResetLatch(MyLatch);
        if (got_sigterm)
            break;
        if (!(rc & WL_SOCKET_READABLE))
            continue;

        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0)
        {
            int e = errno;
            if (e == EINTR || e == EAGAIN || e == EWOULDBLOCK)
                continue;
            elog(WARNING, "zap: accept() failed: %s", strerror(e));
            continue;
        }
        if (fcntl(client_fd, F_SETFL, O_NONBLOCK) == 0)
            serve_connection(client_fd);
        close(client_fd);
    }

    close(server_fd);
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
                            "Number of ZAP-HTTP listener workers (concurrent connections).",
                            NULL, &zap_workers, 4, 1, 64,
                            PGC_POSTMASTER, 0, NULL, NULL, NULL);
    DefineCustomStringVariable("zap.database",
                               "Database the ZAP-HTTP listener connects to (default $POSTGRES_DB).",
                               NULL, &zap_database, NULL,
                               PGC_POSTMASTER, 0, NULL, NULL, NULL);

    if (!process_shared_preload_libraries_in_progress || !zap_enabled)
        return;

    for (i = 0; i < zap_workers; i++)
    {
        BackgroundWorker worker;

        memset(&worker, 0, sizeof(worker));
        worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
        worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
        worker.bgw_restart_time = 5;
        snprintf(worker.bgw_library_name, BGW_MAXLEN, "zap_fdw");
        snprintf(worker.bgw_function_name, BGW_MAXLEN, "zap_worker_main");
        snprintf(worker.bgw_name, BGW_MAXLEN, "zap listener %d", i);
        snprintf(worker.bgw_type, BGW_MAXLEN, "zap listener");
        worker.bgw_main_arg = Int32GetDatum(i);
        RegisterBackgroundWorker(&worker);
    }
    elog(LOG, "zap: registered %d listener worker(s) on port %d", zap_workers, zap_port);
}
