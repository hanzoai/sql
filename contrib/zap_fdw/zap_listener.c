/*
 * zap_listener.c — ZAP binary protocol listener for hanzo/sql.
 *
 * Registers a background worker that listens for ZAP connections on
 * a configurable port (default 9651). Incoming ZAP messages are parsed
 * and routed to SPI for SQL execution or to the KV layer.
 *
 * This makes hanzo/sql speak ZAP natively — no sidecar needed.
 */
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "utils/guc.h"
#include "tcop/utility.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "zap_protocol.h"

PG_MODULE_MAGIC;

/* GUC variables */
static int zap_port = 9651;

/* Forward declarations */
void _PG_init(void);
PGDLLEXPORT void zap_worker_main(Datum main_arg);

/* External SPI functions */
extern char *zap_execute_query(const char *sql, int sql_len);
extern char *zap_execute_statement(const char *sql, int sql_len);
extern void zap_kv_ensure_table(void);
extern char *zap_kv_get(const char *key);
extern char *zap_kv_set(const char *key, const char *value, const char *kind);
extern char *zap_kv_del(const char *key);

/*
 * Extract a string value for a given key from a JSON object.
 * Handles simple JSON like {"key":"value","other":"thing"}.
 * Returns a palloc'd string or NULL if not found.
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

    /* skip whitespace and colon */
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':')
        p++;

    if (*p != '"')
        return NULL;
    p++; /* skip opening quote */

    start = p;
    /* find closing quote, handling \" escapes */
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

/*
 * Handle a single ZAP message from a client connection.
 */
static void
handle_zap_message(int client_fd, const uint8_t *data, size_t data_len)
{
    ZapHeader hdr;
    uint32_t path_len, body_len;
    const char *path;
    const uint8_t *body;
    char *result = NULL;
    uint32_t status = 200;

    if (!zap_parse_header(data, data_len, &hdr))
    {
        const char *err = "{\"error\":\"invalid ZAP header\"}";
        uint32_t resp_size;
        uint8_t *resp = zap_build_response(400, (const uint8_t *)err,
                                            strlen(err), &resp_size);
        if (resp)
        {
            send(client_fd, resp, resp_size, 0);
            free(resp);
        }
        return;
    }

    /* Extract path and body from root object */
    path = zap_read_text(data, data_len, hdr.root_offset,
                         ZAP_FIELD_PATH, &path_len);
    body = zap_read_bytes(data, data_len, hdr.root_offset,
                          ZAP_FIELD_BODY, &body_len);

    /* Route by message type */
    if (hdr.msg_type == ZAP_MSG_SQL || hdr.msg_type == 0)
    {
        /* SQL operations */
        if (path && path_len > 0)
        {
            /* Extract SQL from JSON body: {"sql": "...", "args": [...]} */
            /* Body is raw SQL or JSON — extract and execute */
            char *sql_buf = NULL;

            if (body && body_len > 0)
            {
                /* Simple JSON extraction — find "sql" field value */
                /* In production, use a proper JSON parser */
                sql_buf = palloc(body_len + 1);
                memcpy(sql_buf, body, body_len);
                sql_buf[body_len] = '\0';
            }

            if (sql_buf)
            {
                if (strncmp(path, "/query", 6) == 0)
                    result = zap_execute_query(sql_buf, strlen(sql_buf));
                else if (strncmp(path, "/exec", 5) == 0)
                    result = zap_execute_statement(sql_buf, strlen(sql_buf));
                pfree(sql_buf);
            }
        }
    }
    else if (hdr.msg_type == ZAP_MSG_KV)
    {
        /* KV operations — body is JSON: {"key":"...", "value":"...", "kind":"..."} */
        if (path && strncmp(path, "/get", 4) == 0)
        {
            if (body && body_len > 0)
            {
                char *json_buf = palloc(body_len + 1);
                memcpy(json_buf, body, body_len);
                json_buf[body_len] = '\0';

                char *key_str = json_extract_string(json_buf, "key");
                if (key_str)
                {
                    result = zap_kv_get(key_str);
                    pfree(key_str);
                    if (!result)
                    {
                        result = "{\"error\":\"not found\"}";
                        status = 404;
                    }
                }
                else
                {
                    result = "{\"error\":\"key required in JSON body\"}";
                    status = 400;
                }
                pfree(json_buf);
            }
        }
        else if (path && strncmp(path, "/set", 4) == 0)
        {
            if (body && body_len > 0)
            {
                char *json_buf = palloc(body_len + 1);
                memcpy(json_buf, body, body_len);
                json_buf[body_len] = '\0';

                char *key_str = json_extract_string(json_buf, "key");
                char *val_str = json_extract_string(json_buf, "value");
                char *kind_str = json_extract_string(json_buf, "kind");

                if (key_str && val_str)
                {
                    result = zap_kv_set(key_str, val_str, kind_str ? kind_str : "");
                }
                else
                {
                    result = "{\"error\":\"key and value required in JSON body\"}";
                    status = 400;
                }

                if (key_str) pfree(key_str);
                if (val_str) pfree(val_str);
                if (kind_str) pfree(kind_str);
                pfree(json_buf);
            }
        }
        else if (path && strncmp(path, "/del", 4) == 0)
        {
            if (body && body_len > 0)
            {
                char *json_buf = palloc(body_len + 1);
                memcpy(json_buf, body, body_len);
                json_buf[body_len] = '\0';

                char *key_str = json_extract_string(json_buf, "key");
                if (key_str)
                {
                    result = zap_kv_del(key_str);
                    pfree(key_str);
                }
                else
                {
                    result = "{\"error\":\"key required in JSON body\"}";
                    status = 400;
                }
                pfree(json_buf);
            }
        }
    }

    if (!result)
    {
        result = "{\"error\":\"no result\"}";
        status = 500;
    }

    /* Build and send ZAP response */
    {
        uint32_t resp_size;
        uint8_t *resp = zap_build_response(status, (const uint8_t *)result,
                                            strlen(result), &resp_size);
        if (resp)
        {
            send(client_fd, resp, resp_size, 0);
            free(resp);
        }
    }
}

/*
 * Background worker main loop — listens for ZAP connections.
 */
void
zap_worker_main(Datum main_arg)
{
    int server_fd;
    struct sockaddr_in addr;

    /* Register signal handlers */
    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);

    /* Ensure KV table exists */
    zap_kv_ensure_table();

    /* Create TCP socket */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        elog(ERROR, "zap: socket() failed: %s", strerror(errno));
        return;
    }

    {
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(zap_port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        elog(ERROR, "zap: bind() port %d failed: %s", zap_port, strerror(errno));
        close(server_fd);
        return;
    }

    if (listen(server_fd, 32) < 0)
    {
        elog(ERROR, "zap: listen() failed: %s", strerror(errno));
        close(server_fd);
        return;
    }

    elog(LOG, "zap: listening on port %d", zap_port);

    /* Main accept loop */
    while (!got_sigterm)
    {
        int client_fd;
        uint8_t buf[65536];
        ssize_t n;

        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0)
        {
            if (errno == EINTR)
                continue;
            elog(WARNING, "zap: accept() failed: %s", strerror(errno));
            continue;
        }

        /* Read ZAP message */
        n = recv(client_fd, buf, sizeof(buf), 0);
        if (n > 0)
            handle_zap_message(client_fd, buf, (size_t)n);

        close(client_fd);
    }

    close(server_fd);
    elog(LOG, "zap: listener shutting down");
}

/* Signal handler flag */
static volatile sig_atomic_t got_sigterm = false;

static void
zap_sigterm_handler(SIGNAL_ARGS)
{
    int save_errno = errno;
    got_sigterm = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

/*
 * Module initialization — register background worker and GUCs.
 */
void
_PG_init(void)
{
    BackgroundWorker worker;

    /* Define GUC for port */
    DefineCustomIntVariable("zap.port",
                           "Port for ZAP binary protocol listener",
                           NULL,
                           &zap_port,
                           9651,   /* default */
                           1024,   /* min */
                           65535,  /* max */
                           PGC_POSTMASTER,
                           0,
                           NULL, NULL, NULL);

    /* Register background worker */
    memset(&worker, 0, sizeof(worker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "zap listener");
    snprintf(worker.bgw_type, BGW_MAXLEN, "zap listener");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = 5;  /* restart after 5 seconds on crash */
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "zap_fdw");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "zap_worker_main");
    worker.bgw_main_arg = (Datum)0;
    worker.bgw_notify_pid = 0;

    RegisterBackgroundWorker(&worker);
}
