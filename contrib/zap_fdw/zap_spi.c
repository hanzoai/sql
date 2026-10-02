/*
 * zap_spi.c — execute ZAP-HTTP /query and /exec requests via SPI.
 *
 * A request body is JSON {"sql": "... $1 $2 ...", "args": ["a", "b"]}. Args are
 * bound as parameters of unknown type, so Postgres coerces each like a string
 * literal — the same value serves a text, jsonb, or timestamptz column without
 * the caller casting. /query returns the rows as a JSON array of objects;
 * /exec returns the affected-row count.
 *
 * The caller (zap_listener) runs each request inside a transaction with an
 * active snapshot — its own, or the client's open one — so these run plain SPI.
 * Any statement may write: an INSERT ... RETURNING is a /query.
 */
#include "postgres.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "lib/stringinfo.h"
#include "catalog/pg_type_d.h"   /* type OIDs */
#include "utils/hsearch.h"
#include "utils/memutils.h"

char *zap_sql_query(const char *body, int body_len, uint32_t *status);
char *zap_sql_exec(const char *body, int body_len, uint32_t *status);
void zap_ensure_tables(void);
void zap_append_json_string(StringInfo buf, const char *s);

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

/*
 * Each worker keeps the plans of the statements it has run, keyed by their text.
 * A statement names its kind as a literal and binds its values, so one shape of
 * statement plans once: without this every request was planned anew, and against
 * a table carrying a partial index per indexed field, planning cost more than
 * running (2.3 ms against 0.2 ms for a user lookup). PostgreSQL invalidates a
 * kept plan when its table's indexes or definition change, so a plan is never
 * stale. The cache holds at most ZAP_PLANS statements (a megabyte a worker); past
 * that it starts over.
 */
#define ZAP_PLANS 1024
#define ZAP_PLAN_KEY 1024   /* longer statements are not cached */

typedef struct ZapPlan
{
    char        key[ZAP_PLAN_KEY];  /* the statement text, then the argument count */
    SPIPlanPtr  plan;
} ZapPlan;

static HTAB *zap_plans = NULL;

/* Whether sql is a statement whose plan may be kept: one that reads or writes rows. */
static bool
cacheable(const char *sql)
{
    while (*sql == ' ' || *sql == '\t' || *sql == '\n' || *sql == '\r' || *sql == '(')
        sql++;
    return pg_strncasecmp(sql, "SELECT", 6) == 0 || pg_strncasecmp(sql, "INSERT", 6) == 0 ||
           pg_strncasecmp(sql, "UPDATE", 6) == 0 || pg_strncasecmp(sql, "DELETE", 6) == 0 ||
           pg_strncasecmp(sql, "WITH", 4) == 0;
}

/* The kept plan for sql with nargs text arguments, preparing it on first use; NULL
 * when the statement is not one to keep. */
static SPIPlanPtr
kept_plan(const char *sql, int nargs, Oid *types)
{
    char key[ZAP_PLAN_KEY];
    ZapPlan *entry;
    bool found;
    SPIPlanPtr plan;
    int n;

    if (!cacheable(sql))
        return NULL;
    n = snprintf(key, sizeof(key), "%s\x01%d", sql, nargs);
    if (n < 0 || n >= (int) sizeof(key))
        return NULL;
    memset(key + n, 0, sizeof(key) - n);

    if (zap_plans == NULL || hash_get_num_entries(zap_plans) >= ZAP_PLANS)
    {
        HASHCTL ctl;

        if (zap_plans != NULL)
        {
            HASH_SEQ_STATUS seq;

            hash_seq_init(&seq, zap_plans);
            while ((entry = hash_seq_search(&seq)) != NULL)
                SPI_freeplan(entry->plan);
            hash_destroy(zap_plans);
        }
        memset(&ctl, 0, sizeof(ctl));
        ctl.keysize = ZAP_PLAN_KEY;
        ctl.entrysize = sizeof(ZapPlan);
        ctl.hcxt = TopMemoryContext;
        zap_plans = hash_create("zap plans", 256, &ctl, HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);
    }

    entry = hash_search(zap_plans, key, HASH_FIND, &found);
    if (found)
        return entry->plan;

    plan = SPI_prepare(sql, nargs, types);
    if (plan == NULL)
        return NULL;
    if (SPI_keepplan(plan) != 0)
        return plan;
    entry = hash_search(zap_plans, key, HASH_ENTER, &found);
    entry->plan = plan;
    return plan;
}

/* Execute sql with the parsed args bound as text parameters. */
static int
run_sql(const char *sql, char **args, bool *argnull, int nargs, bool read_only)
{
    Oid *types = NULL;
    Datum *vals = NULL;
    char *nulls = NULL;
    SPIPlanPtr plan;
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
    plan = kept_plan(sql, nargs, types);
    if (plan != NULL)
        return SPI_execute_plan(plan, vals, nulls, read_only, 0);
    return SPI_execute_with_args(sql, nargs, types, vals, nulls, read_only, 0);
}

/* Append s as a JSON string: quoted, with quotes, backslashes and control
 * characters escaped. */
void
zap_append_json_string(StringInfo buf, const char *s)
{
    appendStringInfoChar(buf, '"');
    for (; *s; s++)
    {
        unsigned char c = (unsigned char) *s;

        switch (c)
        {
            case '"':  appendStringInfoString(buf, "\\\""); break;
            case '\\': appendStringInfoString(buf, "\\\\"); break;
            case '\n': appendStringInfoString(buf, "\\n"); break;
            case '\r': appendStringInfoString(buf, "\\r"); break;
            case '\t': appendStringInfoString(buf, "\\t"); break;
            default:
                if (c < 0x20)
                    appendStringInfo(buf, "\\u%04x", c);
                else
                    appendStringInfoChar(buf, (char) c);
        }
    }
    appendStringInfoChar(buf, '"');
}

/* Append one SPI result value as JSON by its column type: json and jsonb as the
 * document they hold, a boolean as true or false, a number as its digits, and
 * everything else as a string. Guessing from the text cannot work: an id that
 * begins with a digit is not a number, and a string may contain a quote. */
static void
append_value(StringInfo buf, char *value, Oid type)
{
    if (value == NULL)
    {
        appendStringInfoString(buf, "null");
        return;
    }
    switch (type)
    {
        case JSONOID:
        case JSONBOID:
            appendStringInfoString(buf, value);
            break;
        case BOOLOID:
            appendStringInfoString(buf, value[0] == 't' ? "true" : "false");
            break;
        case INT2OID:
        case INT4OID:
        case INT8OID:
        case OIDOID:
        case NUMERICOID:
        case FLOAT4OID:
        case FLOAT8OID:
            if (strcmp(value, "NaN") == 0 || strcmp(value, "Infinity") == 0 ||
                strcmp(value, "-Infinity") == 0)
                zap_append_json_string(buf, value);
            else
                appendStringInfoString(buf, value);
            break;
        default:
            zap_append_json_string(buf, value);
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
    ret = run_sql(sql, args, argnull, nargs, false);
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
            zap_append_json_string(&buf, colname);
            appendStringInfoChar(&buf, ':');
            append_value(&buf, value, SPI_gettypeid(SPI_tuptable->tupdesc, j + 1));
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
 * as jsonb, which is what the ORM reads fields of with ->> and indexes) and the
 * _zap_kv store. A transaction advisory lock serializes workers so concurrent
 * CREATE IF NOT EXISTS can't race on the type catalog. Runs inside the caller's
 * transaction.
 *
 * A table an earlier version created with text columns is converted in place:
 * data to jsonb, the two timestamps to timestamptz. An empty value, the old
 * columns' default, becomes an empty document or the epoch.
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
        "data jsonb NOT NULL DEFAULT '{}'::jsonb, "
        "created_at timestamptz NOT NULL DEFAULT now(), "
        "updated_at timestamptz NOT NULL DEFAULT now(), "
        "deleted boolean NOT NULL DEFAULT false)", false, 0);
    SPI_execute(
        "DO $do$ BEGIN "
        "IF (SELECT data_type FROM information_schema.columns "
        "    WHERE table_schema = current_schema() AND table_name = '_entities' AND column_name = 'data') = 'text' THEN "
        "  ALTER TABLE _entities ALTER COLUMN data DROP DEFAULT; "
        "  ALTER TABLE _entities ALTER COLUMN data TYPE jsonb USING (CASE WHEN data = '' THEN '{}' ELSE data END)::jsonb; "
        "  ALTER TABLE _entities ALTER COLUMN data SET DEFAULT '{}'::jsonb; "
        "END IF; "
        "IF (SELECT data_type FROM information_schema.columns "
        "    WHERE table_schema = current_schema() AND table_name = '_entities' AND column_name = 'created_at') = 'text' THEN "
        "  ALTER TABLE _entities ALTER COLUMN created_at DROP DEFAULT, ALTER COLUMN updated_at DROP DEFAULT; "
        "  ALTER TABLE _entities "
        "    ALTER COLUMN created_at TYPE timestamptz USING (CASE WHEN created_at = '' THEN 'epoch' ELSE created_at END)::timestamptz, "
        "    ALTER COLUMN updated_at TYPE timestamptz USING (CASE WHEN updated_at = '' THEN 'epoch' ELSE updated_at END)::timestamptz; "
        "  ALTER TABLE _entities ALTER COLUMN created_at SET DEFAULT now(), ALTER COLUMN updated_at SET DEFAULT now(); "
        "END IF; END $do$", false, 0);
    SPI_execute("CREATE INDEX IF NOT EXISTS _entities_kind ON _entities (kind)", false, 0);
    SPI_execute(
        "CREATE TABLE IF NOT EXISTS _zap_kv ("
        "key text PRIMARY KEY, "
        "kind text NOT NULL DEFAULT '', "
        "value jsonb NOT NULL DEFAULT '{}'::jsonb, "
        "deleted boolean NOT NULL DEFAULT false, "
        "created_at timestamptz NOT NULL DEFAULT now(), "
        "updated_at timestamptz NOT NULL DEFAULT now())", false, 0);
    SPI_finish();
}
