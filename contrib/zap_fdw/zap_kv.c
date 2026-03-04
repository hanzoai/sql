/*
 * zap_kv.c — Key-value operations over ZAP for hanzo/sql.
 *
 * Provides a simple KV interface backed by a SQL table,
 * for use when hanzo/orm targets ZapKV but the data lives in hanzo/sql.
 */
#include "postgres.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "lib/stringinfo.h"

/*
 * Ensure the _zap_kv table exists.
 */
void
zap_kv_ensure_table(void)
{
    SPI_connect();
    SPI_execute(
        "CREATE TABLE IF NOT EXISTS _zap_kv ("
        "  key TEXT PRIMARY KEY,"
        "  kind TEXT NOT NULL DEFAULT '',"
        "  value JSONB NOT NULL DEFAULT '{}'::jsonb,"
        "  deleted BOOLEAN NOT NULL DEFAULT false,"
        "  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
        "  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()"
        ")",
        false, 0);
    SPI_execute(
        "CREATE INDEX IF NOT EXISTS _zap_kv_kind_idx ON _zap_kv(kind) WHERE deleted = false",
        false, 0);
    SPI_finish();
}

/*
 * GET: Retrieve a value by key.
 * Returns JSON string (palloc'd) or NULL if not found.
 */
char *
zap_kv_get(const char *key)
{
    StringInfoData buf;
    int ret;
    char *result = NULL;

    initStringInfo(&buf);

    SPI_connect();

    {
        Oid argtypes[1] = {TEXTOID};
        Datum values[1];
        values[0] = CStringGetTextDatum(key);

        ret = SPI_execute_with_args(
            "SELECT value::text FROM _zap_kv WHERE key = $1 AND deleted = false",
            1, argtypes, values, NULL, true, 1);
    }

    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        char *val = SPI_getvalue(SPI_tuptable->vals[0],
                                SPI_tuptable->tupdesc, 1);
        if (val)
            result = pstrdup(val);
    }

    SPI_finish();
    return result;
}

/*
 * SET: Upsert a key-value pair.
 * Returns JSON status.
 */
char *
zap_kv_set(const char *key, const char *value, const char *kind)
{
    int ret;

    SPI_connect();

    {
        Oid argtypes[3] = {TEXTOID, TEXTOID, TEXTOID};
        Datum values[3];
        values[0] = CStringGetTextDatum(key);
        values[1] = CStringGetTextDatum(value ? value : "{}");
        values[2] = CStringGetTextDatum(kind ? kind : "");

        ret = SPI_execute_with_args(
            "INSERT INTO _zap_kv (key, value, kind, updated_at) "
            "VALUES ($1, $2::jsonb, $3, NOW()) "
            "ON CONFLICT (key) DO UPDATE SET "
            "value = $2::jsonb, kind = $3, updated_at = NOW(), deleted = false",
            3, argtypes, values, NULL, false, 0);
    }

    SPI_finish();

    if (ret < 0)
        return "{\"error\":\"set failed\"}";
    return "{\"status\":\"ok\"}";
}

/*
 * DEL: Soft-delete a key.
 */
char *
zap_kv_del(const char *key)
{
    int ret;

    SPI_connect();

    {
        Oid argtypes[1] = {TEXTOID};
        Datum values[1];
        values[0] = CStringGetTextDatum(key);

        ret = SPI_execute_with_args(
            "UPDATE _zap_kv SET deleted = true, updated_at = NOW() WHERE key = $1",
            1, argtypes, values, NULL, false, 0);
    }

    SPI_finish();

    if (ret < 0)
        return "{\"error\":\"del failed\"}";
    return "{\"status\":\"ok\"}";
}
