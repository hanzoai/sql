/*
 * zap_spi.c — execute ZAP-HTTP /query and /exec requests via SPI.
 *
 * A request body is JSON {"sql": "... $1 $2 ...", "args": ["a", "b"]}. Args are
 * bound as parameters of unknown type, so Postgres coerces each like a string
 * literal — the same value serves a text, jsonb, or timestamptz column without
 * the caller casting. /query returns the rows as a JSON array of objects;
 * /exec returns the affected-row count.
 *
 * The caller (zap_listener) opens a transaction with an active snapshot around
 * each request, so these run plain SPI.
 */
#include "postgres.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "lib/stringinfo.h"
#include "catalog/pg_type_d.h"   /* UNKNOWNOID */

char *zap_sql_query(const char *body, int body_len, uint32_t *status);
char *zap_sql_exec(const char *body, int body_len, uint32_t *status);
void zap_ensure_tables(void);

/* ---- minimal JSON reader for {"sql": "...", "args": ["...", ...]} ---- */

static void
skip_ws(const char **pp)
{
    const char *p = *pp;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    *pp = p;
}

static void
append_utf8(StringInfo s, unsigned int cp)
{
    if (cp < 0x80)
        appendStringInfoChar(s, (char) cp);
    else if (cp < 0x800)
    {
        appendStringInfoChar(s, (char) (0xC0 | (cp >> 6)));
        appendStringInfoChar(s, (char) (0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
        appendStringInfoChar(s, (char) (0xE0 | (cp >> 12)));
        appendStringInfoChar(s, (char) (0x80 | ((cp >> 6) & 0x3F)));
        appendStringInfoChar(s, (char) (0x80 | (cp & 0x3F)));
    }
    else
    {
        appendStringInfoChar(s, (char) (0xF0 | (cp >> 18)));
        appendStringInfoChar(s, (char) (0x80 | ((cp >> 12) & 0x3F)));
        appendStringInfoChar(s, (char) (0x80 | ((cp >> 6) & 0x3F)));
        appendStringInfoChar(s, (char) (0x80 | (cp & 0x3F)));
    }
}

static int
hex4(const char *p)
{
    int v = 0, i;
    for (i = 0; i < 4; i++)
    {
        char c = p[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = (v << 4) | d;
    }
    return v;
}

/* Parse a JSON string at *pp (points at the opening quote). Returns a palloc'd
 * decoded string and advances *pp past the closing quote; NULL if malformed. */
static char *
json_string(const char **pp)
{
    const char *p = *pp;
    StringInfoData s;

    if (*p != '"')
        return NULL;
    p++;
    initStringInfo(&s);
    while (*p && *p != '"')
    {
        if (*p == '\\')
        {
            p++;
            switch (*p)
            {
                case '"': appendStringInfoChar(&s, '"'); p++; break;
                case '\\': appendStringInfoChar(&s, '\\'); p++; break;
                case '/': appendStringInfoChar(&s, '/'); p++; break;
                case 'n': appendStringInfoChar(&s, '\n'); p++; break;
                case 't': appendStringInfoChar(&s, '\t'); p++; break;
                case 'r': appendStringInfoChar(&s, '\r'); p++; break;
                case 'b': appendStringInfoChar(&s, '\b'); p++; break;
                case 'f': appendStringInfoChar(&s, '\f'); p++; break;
                case 'u':
                {
                    int cp = hex4(p + 1);
                    if (cp < 0) { pfree(s.data); return NULL; }
                    p += 5;     /* 'u' + 4 hex */
                    if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u')
                    {
                        int lo = hex4(p + 2);
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                        {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p += 6;
                        }
                    }
                    append_utf8(&s, (unsigned int) cp);
                    break;
                }
                default:
                    if (*p) { appendStringInfoChar(&s, *p); p++; }
                    break;
            }
        }
        else
            appendStringInfoChar(&s, *p++);
    }
    if (*p != '"') { pfree(s.data); return NULL; }
    p++;
    *pp = p;
    return s.data;
}

/* Read a bare JSON token (number/true/false/null) as text; sets *is_null. */
static char *
json_token(const char **pp, bool *is_null)
{
    const char *p = *pp, *start;
    StringInfoData s;

    skip_ws(&p);
    start = p;
    while (*p && *p != ',' && *p != ']' && *p != '}' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
        p++;
    initStringInfo(&s);
    appendBinaryStringInfo(&s, start, p - start);
    *pp = p;
    *is_null = (s.len == 4 && strncmp(s.data, "null", 4) == 0);
    return s.data;
}

/* Parse {"sql": ..., "args": [...]} (keys in any order). */
static bool
parse_sql_body(const char *json, char **sql_out,
               char ***args_out, bool **argnull_out, int *nargs_out)
{
    const char *p = strchr(json, '{');
    char **args = NULL;
    bool *argnull = NULL;
    int n = 0, cap = 0;

    *sql_out = NULL; *args_out = NULL; *argnull_out = NULL; *nargs_out = 0;
    if (!p)
        return false;
    p++;

    for (;;)
    {
        char *key;
        skip_ws(&p);
        if (*p == '}' || *p == '\0')
            break;
        if (*p != '"')
            break;
        key = json_string(&p);
        if (!key)
            return false;
        skip_ws(&p);
        if (*p == ':')
            p++;
        skip_ws(&p);

        if (strcmp(key, "sql") == 0)
            *sql_out = json_string(&p);
        else if (strcmp(key, "args") == 0 && *p == '[')
        {
            p++;
            for (;;)
            {
                char *v;
                bool vnull = false;
                skip_ws(&p);
                if (*p == ']' || *p == '\0')
                    break;
                if (*p == '"')
                    v = json_string(&p);
                else
                    v = json_token(&p, &vnull);
                if (n == cap)
                {
                    cap = cap ? cap * 2 : 8;
                    args = args ? repalloc(args, sizeof(char *) * cap)
                                : palloc(sizeof(char *) * cap);
                    argnull = argnull ? repalloc(argnull, sizeof(bool) * cap)
                                      : palloc(sizeof(bool) * cap);
                }
                args[n] = v;
                argnull[n] = vnull;
                n++;
                skip_ws(&p);
                if (*p == ',')
                    p++;
            }
            if (*p == ']')
                p++;
        }
        else
        {
            /* skip an unrecognized value */
            if (*p == '"')
                json_string(&p);
            else
            {
                bool dn;
                json_token(&p, &dn);
            }
        }
        skip_ws(&p);
        if (*p == ',')
            p++;
    }

    *args_out = args;
    *argnull_out = argnull;
    *nargs_out = n;
    return (*sql_out != NULL);
}

/* Execute sql with the parsed args bound as unknown-type parameters. */
static int
run_sql(const char *sql, char **args, bool *argnull, int nargs, bool read_only)
{
    Oid *types = NULL;
    Datum *vals = NULL;
    char *nulls = NULL;
    int i;

    if (nargs > 0)
    {
        types = palloc(sizeof(Oid) * nargs);
        vals = palloc(sizeof(Datum) * nargs);
        nulls = palloc(nargs);
        for (i = 0; i < nargs; i++)
        {
            types[i] = TEXTOID;
            if (argnull[i])
            {
                vals[i] = (Datum) 0;
                nulls[i] = 'n';
            }
            else
            {
                vals[i] = CStringGetTextDatum(args[i]);
                nulls[i] = ' ';
            }
        }
    }
    return SPI_execute_with_args(sql, nargs, types, vals, nulls, read_only, 0);
}

/* Append one SPI result value as JSON: a JSON scalar/object emitted verbatim,
 * anything else quoted as a string. */
static void
append_value(StringInfo buf, char *value)
{
    if (value == NULL)
    {
        appendStringInfoString(buf, "null");
        return;
    }
    if (value[0] == '{' || value[0] == '[' || value[0] == '"' ||
        strcmp(value, "true") == 0 || strcmp(value, "false") == 0 ||
        (value[0] >= '0' && value[0] <= '9') || value[0] == '-')
        appendStringInfoString(buf, value);
    else
    {
        appendStringInfoChar(buf, '"');
        appendStringInfoString(buf, value);
        appendStringInfoChar(buf, '"');
    }
}

/*
 * /query — run a SELECT (or DML ... RETURNING) and return the rows as a JSON
 * array of objects. Status 200 on success (even for zero rows).
 */
char *
zap_sql_query(const char *body, int body_len, uint32_t *status)
{
    char *json, *sql, **args;
    bool *argnull;
    int nargs, ret, i, j;
    StringInfoData buf;

    json = palloc(body_len + 1);
    memcpy(json, body, body_len);
    json[body_len] = '\0';

    initStringInfo(&buf);
    if (!parse_sql_body(json, &sql, &args, &argnull, &nargs))
    {
        *status = 400;
        appendStringInfoString(&buf, "{\"error\":\"missing sql\"}");
        return buf.data;
    }

    SPI_connect();
    ret = run_sql(sql, args, argnull, nargs, true);
    if (ret < 0)
    {
        SPI_finish();
        *status = 500;
        resetStringInfo(&buf);
        appendStringInfo(&buf, "{\"error\":\"SPI error %d\"}", ret);
        return buf.data;
    }

    appendStringInfoChar(&buf, '[');
    for (i = 0; i < (int) SPI_processed; i++)
    {
        if (i > 0)
            appendStringInfoChar(&buf, ',');
        appendStringInfoChar(&buf, '{');
        for (j = 0; j < SPI_tuptable->tupdesc->natts; j++)
        {
            char *colname = NameStr(TupleDescAttr(SPI_tuptable->tupdesc, j)->attname);
            char *value = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, j + 1);
            if (j > 0)
                appendStringInfoChar(&buf, ',');
            appendStringInfo(&buf, "\"%s\":", colname);
            append_value(&buf, value);
        }
        appendStringInfoChar(&buf, '}');
    }
    appendStringInfoChar(&buf, ']');

    SPI_finish();
    *status = 200;
    return buf.data;
}

/*
 * /exec — run a statement and return the affected-row count. Status 200 on
 * success.
 */
char *
zap_sql_exec(const char *body, int body_len, uint32_t *status)
{
    char *json, *sql, **args;
    bool *argnull;
    int nargs, ret;
    StringInfoData buf;

    json = palloc(body_len + 1);
    memcpy(json, body, body_len);
    json[body_len] = '\0';

    initStringInfo(&buf);
    if (!parse_sql_body(json, &sql, &args, &argnull, &nargs))
    {
        *status = 400;
        appendStringInfoString(&buf, "{\"error\":\"missing sql\"}");
        return buf.data;
    }

    SPI_connect();
    ret = run_sql(sql, args, argnull, nargs, false);
    if (ret < 0)
    {
        SPI_finish();
        *status = 500;
        appendStringInfo(&buf, "{\"error\":\"SPI error %d\"}", ret);
        return buf.data;
    }

    appendStringInfo(&buf, "{\"affected\":%lu}", (unsigned long) SPI_processed);
    SPI_finish();
    *status = 200;
    return buf.data;
}

/*
 * Provision the tables the ORM's ZAP SQL and KV backends expect: a single
 * _entities store keyed by id (kind distinguishes rows, data holds the entity
 * JSON) and the _zap_kv store. A transaction advisory lock serializes workers so
 * concurrent CREATE IF NOT EXISTS can't race on the type catalog. Runs inside
 * the caller's transaction.
 */
void
zap_ensure_tables(void)
{
    SPI_connect();
    SPI_execute("SELECT pg_advisory_xact_lock(491900001)", false, 0);
    SPI_execute(
        "CREATE TABLE IF NOT EXISTS _entities ("
        "id text PRIMARY KEY, "
        "kind text NOT NULL DEFAULT '', "
        "data text NOT NULL DEFAULT '', "
        "created_at text NOT NULL DEFAULT '', "
        "updated_at text NOT NULL DEFAULT '', "
        "deleted boolean NOT NULL DEFAULT false)", false, 0);
    SPI_execute("CREATE INDEX IF NOT EXISTS _entities_kind ON _entities (kind)", false, 0);
    SPI_execute(
        "CREATE TABLE IF NOT EXISTS _zap_kv ("
        "key text PRIMARY KEY, "
        "kind text NOT NULL DEFAULT '', "
        "value jsonb NOT NULL DEFAULT '{}'::jsonb, "
        "deleted boolean NOT NULL DEFAULT false, "
        "created_at timestamptz NOT NULL DEFAULT now(), "
        "updated_at timestamptz NOT NULL DEFAULT now())", false, 0);
    /*
     * The ORM's ZAP backend addresses entity fields with SQLite's
     * json_extract(data, '$.a.b'); provide it over jsonb so the one dialect
     * runs unchanged on Postgres. Returns the value as text, matching SQLite.
     */
    SPI_execute(
        "CREATE OR REPLACE FUNCTION json_extract(j text, path text) RETURNS text "
        "LANGUAGE sql IMMUTABLE AS $fn$ "
        "SELECT CASE WHEN j IS NULL OR j = '' THEN NULL "
        "ELSE (j::jsonb #>> string_to_array(ltrim(path, '$.'), '.')) END $fn$", false, 0);
    SPI_finish();
}
