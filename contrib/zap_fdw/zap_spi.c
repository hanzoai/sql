/*
 * zap_spi.c — Execute SQL queries received via ZAP and return results.
 *
 * Uses SPI (Server Programming Interface) to execute queries
 * and converts results to JSON for ZAP response bodies.
 */
#include "postgres.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "lib/stringinfo.h"
#include "zap_protocol.h"

/*
 * Execute a SQL query via SPI and return results as JSON.
 * The JSON format is an array of objects: [{"col1": val1, ...}, ...]
 *
 * Returns a palloc'd string. Caller must pfree.
 */
char *
zap_execute_query(const char *sql, int sql_len)
{
    StringInfoData buf;
    int ret;
    int i, j;

    initStringInfo(&buf);

    SPI_connect();

    ret = SPI_execute(sql, true /* read-only */, 0 /* no limit */);

    if (ret != SPI_OK_SELECT && ret != SPI_OK_INSERT_RETURNING &&
        ret != SPI_OK_UPDATE_RETURNING && ret != SPI_OK_DELETE_RETURNING)
    {
        /* For non-SELECT queries (INSERT/UPDATE/DELETE) */
        if (ret == SPI_OK_INSERT || ret == SPI_OK_UPDATE || ret == SPI_OK_DELETE)
        {
            appendStringInfo(&buf, "{\"affected\":%lu}",
                           (unsigned long)SPI_processed);
            SPI_finish();
            return buf.data;
        }

        SPI_finish();
        appendStringInfo(&buf, "{\"error\":\"SPI error: %d\"}", ret);
        return buf.data;
    }

    /* Build JSON array from SPI results */
    appendStringInfoChar(&buf, '[');

    for (i = 0; i < (int)SPI_processed; i++)
    {
        if (i > 0)
            appendStringInfoChar(&buf, ',');

        appendStringInfoChar(&buf, '{');

        for (j = 0; j < SPI_tuptable->tupdesc->natts; j++)
        {
            char *colname;
            char *value;
            bool isnull;

            if (j > 0)
                appendStringInfoChar(&buf, ',');

            colname = NameStr(TupleDescAttr(SPI_tuptable->tupdesc, j)->attname);
            value = SPI_getvalue(SPI_tuptable->vals[i],
                               SPI_tuptable->tupdesc, j + 1);
            isnull = SPI_getbinval(SPI_tuptable->vals[i],
                                   SPI_tuptable->tupdesc, j + 1,
                                   &isnull) == (Datum)0 && isnull;

            appendStringInfo(&buf, "\"%s\":", colname);

            if (isnull || value == NULL)
                appendStringInfoString(&buf, "null");
            else
            {
                /* Try to detect if value is already JSON-like */
                if (value[0] == '{' || value[0] == '[' || value[0] == '"' ||
                    strcmp(value, "true") == 0 || strcmp(value, "false") == 0 ||
                    (value[0] >= '0' && value[0] <= '9') || value[0] == '-')
                    appendStringInfoString(&buf, value);
                else
                {
                    /* Quote as string */
                    appendStringInfoChar(&buf, '"');
                    appendStringInfoString(&buf, value);
                    appendStringInfoChar(&buf, '"');
                }
            }
        }

        appendStringInfoChar(&buf, '}');
    }

    appendStringInfoChar(&buf, ']');

    SPI_finish();

    return buf.data;
}

/*
 * Execute a non-query SQL statement (INSERT/UPDATE/DELETE).
 * Returns affected row count as JSON.
 */
char *
zap_execute_statement(const char *sql, int sql_len)
{
    StringInfoData buf;
    int ret;

    initStringInfo(&buf);

    SPI_connect();

    ret = SPI_execute(sql, false /* read-write */, 0);

    if (ret < 0)
    {
        SPI_finish();
        appendStringInfo(&buf, "{\"error\":\"SPI error: %d\"}", ret);
        return buf.data;
    }

    appendStringInfo(&buf, "{\"affected\":%lu}",
                   (unsigned long)SPI_processed);

    SPI_finish();

    return buf.data;
}
