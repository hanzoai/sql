/*-------------------------------------------------------------------------
 *
 * token.c
 *	  Check a hanzo-sql token: a compact JWS signed RS256 by the sql key.
 *
 * A token is <header>.<claims>.<signature>, each part base64url without
 * padding.  The checks run in this order, and the claims are not even parsed
 * until the signature has verified:
 *
 *	  the whole token is at most 8 KiB and has exactly three parts;
 *	  the header names alg RS256 and a kid, which selects the public key;
 *	  the signature verifies over "<header>.<claims>" with that key;
 *	  iss is https://hanzo.id, aud contains hanzo-sql, typ is sql;
 *	  act is absent;
 *	  sub is "sql:" and a role;
 *	  exp is no more than 60 s past, iat and nbf (which may be absent) no more
 *	  than 60 s ahead, and exp - iat is in (0, 600] s.
 *
 * typ is a claim.  The JOSE header's own typ (JWT, usually) is not read.
 *
 * JSON is read with PostgreSQL's jsonapi.  A member that a check names may
 * appear once, and as the kind of value the check expects; nesting past
 * MAX_DEPTH is refused as the parser reaches it.  Members that no check names
 * are skipped.
 *
 * This file is compiled twice: as part of the server module, and with
 * -DFRONTEND into the fuzz target.  It allocates only through jsonapi.
 *
 * contrib/hanzo_iam/token.c
 *
 *-------------------------------------------------------------------------
 */
#ifdef FRONTEND
#include "postgres_fe.h"
#else
#include "postgres.h"
#endif

#include <openssl/err.h>
#include <openssl/evp.h>

#include "common/jsonapi.h"
#include "mb/pg_wchar.h"

#include "token.h"

#define MAX_DEPTH		4		/* the claims object, aud, and two more */
#define MIN_KEY_BITS	2048

typedef enum Field
{
	F_NONE,						/* between members */
	F_OTHER,					/* a member no check names */
	F_ALG,
	F_KID,
	F_ISS,
	F_AUD,
	F_SUB,
	F_TYP,
	F_EXP,
	F_NBF,
	F_IAT,
	F_ACT
} Field;

typedef struct Doc
{
	bool		claims;			/* reading the claims, else the header */
	int			depth;			/* open objects and arrays */
	Field		field;			/* the top-level member being read */
	bool		in_aud;			/* inside the aud array */
	uint32		seen;			/* bit per Field, set at its first member */
	const char *reason;			/* set by the first rule that fails */
	bool		aud;			/* aud names hanzo-sql */
	int64		exp;
	int64		nbf;
	int64		iat;
	char		kid[HANZO_IAM_MAX_KID + 1];
	char		role[HANZO_IAM_MAX_ROLE + 1];
} Doc;

#define SEEN(f)			(UINT32_C(1) << (f))

static const struct
{
	const char *name;
	bool		claims;
	Field		field;
}			members[] = {
	{"alg", false, F_ALG},
	{"kid", false, F_KID},
	{"iss", true, F_ISS},
	{"aud", true, F_AUD},
	{"sub", true, F_SUB},
	{"typ", true, F_TYP},
	{"exp", true, F_EXP},
	{"nbf", true, F_NBF},
	{"iat", true, F_IAT},
	{"act", true, F_ACT},
};

static JsonParseErrorType
fail(Doc *d, const char *reason)
{
	if (d->reason == NULL)
		d->reason = reason;
	return JSON_SEM_ACTION_FAILED;
}

static JsonParseErrorType
malformed(Doc *d)
{
	return fail(d, d->claims ? "malformed claims" : "malformed header");
}

/* Letters, digits, '.', '_' and '-'; no leading '.', so no "..", no path. */
static bool
kid_ok(const char *s)
{
	size_t		n = strlen(s);
	size_t		i;

	if (n == 0 || n > HANZO_IAM_MAX_KID || s[0] == '.')
		return false;
	for (i = 0; i < n; i++)
	{
		char		c = s[i];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
			return false;
	}
	return true;
}

/* Letters, digits, '_' and '-'. */
static bool
role_ok(const char *s)
{
	size_t		n = strlen(s);
	size_t		i;

	if (n == 0 || n > HANZO_IAM_MAX_ROLE)
		return false;
	for (i = 0; i < n; i++)
	{
		char		c = s[i];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			  (c >= '0' && c <= '9') || c == '_' || c == '-'))
			return false;
	}
	return true;
}

/* A NumericDate: whole seconds, at most 15 digits. */
static bool
seconds(const char *s, int64 *v)
{
	size_t		n = strlen(s);
	size_t		i;

	if (n == 0 || n > 15)
		return false;
	*v = 0;
	for (i = 0; i < n; i++)
	{
		if (s[i] < '0' || s[i] > '9')
			return false;
		*v = *v * 10 + (s[i] - '0');
	}
	return true;
}

static JsonParseErrorType
on_object_start(void *state)
{
	Doc		   *d = state;

	if (d->depth > 0 && d->field != F_OTHER)
		return malformed(d);
	if (++d->depth > MAX_DEPTH)
		return malformed(d);
	return JSON_SUCCESS;
}

static JsonParseErrorType
on_object_end(void *state)
{
	Doc		   *d = state;

	d->depth--;
	return JSON_SUCCESS;
}

static JsonParseErrorType
on_array_start(void *state)
{
	Doc		   *d = state;

	if (d->depth == 0)
		return malformed(d);
	if (d->depth == 1 && d->field == F_AUD && !d->in_aud)
		d->in_aud = true;
	else if (d->field != F_OTHER)
		return malformed(d);
	if (++d->depth > MAX_DEPTH)
		return malformed(d);
	return JSON_SUCCESS;
}

static JsonParseErrorType
on_array_end(void *state)
{
	Doc		   *d = state;

	d->depth--;
	if (d->depth == 1)
		d->in_aud = false;
	return JSON_SUCCESS;
}

static JsonParseErrorType
on_field_start(void *state, char *name, bool isnull)
{
	Doc		   *d = state;
	Field		f = F_OTHER;
	size_t		i;

	/* Members of nested objects belong to a member no check names. */
	if (d->depth != 1)
		return JSON_SUCCESS;

	for (i = 0; i < lengthof(members); i++)
	{
		if (members[i].claims == d->claims && strcmp(name, members[i].name) == 0)
		{
			f = members[i].field;
			break;
		}
	}

	if (f == F_ACT)
		return fail(d, "act claim present");
	if (f != F_OTHER)
	{
		if (d->seen & SEEN(f))
			return fail(d, "duplicate member");
		d->seen |= SEEN(f);
	}
	d->field = f;
	return JSON_SUCCESS;
}

static JsonParseErrorType
on_field_end(void *state, char *name, bool isnull)
{
	Doc		   *d = state;

	if (d->depth == 1)
		d->field = F_NONE;
	return JSON_SUCCESS;
}

static JsonParseErrorType
on_scalar(void *state, char *value, JsonTokenType type)
{
	Doc		   *d = state;
	int64		n = 0;

	if (d->depth == 0)
		return malformed(d);

	switch (d->field)
	{
		case F_ALG:
			if (type != JSON_TOKEN_STRING)
				return malformed(d);
			if (strcmp(value, "RS256") != 0)
				return fail(d, "unsupported algorithm");
			break;

		case F_KID:
			if (type != JSON_TOKEN_STRING)
				return malformed(d);
			if (!kid_ok(value))
				return fail(d, "bad key id");
			strlcpy(d->kid, value, sizeof(d->kid));
			break;

		case F_ISS:
			if (type != JSON_TOKEN_STRING)
				return malformed(d);
			if (strcmp(value, HANZO_IAM_ISSUER) != 0)
				return fail(d, "wrong issuer");
			break;

		case F_AUD:
			/* A string, or an array of strings; one must be ours. */
			if (type != JSON_TOKEN_STRING)
				return malformed(d);
			if (strcmp(value, HANZO_IAM_AUDIENCE) == 0)
				d->aud = true;
			break;

		case F_TYP:
			if (type != JSON_TOKEN_STRING)
				return malformed(d);
			if (strcmp(value, HANZO_IAM_TYPE) != 0)
				return fail(d, "wrong token type");
			break;

		case F_SUB:
			if (type != JSON_TOKEN_STRING)
				return malformed(d);
			if (strncmp(value, HANZO_IAM_PREFIX, strlen(HANZO_IAM_PREFIX)) != 0 ||
				!role_ok(value + strlen(HANZO_IAM_PREFIX)))
				return fail(d, "bad subject");
			strlcpy(d->role, value + strlen(HANZO_IAM_PREFIX), sizeof(d->role));
			break;

		case F_EXP:
		case F_NBF:
		case F_IAT:
			if (type != JSON_TOKEN_NUMBER || !seconds(value, &n))
				return malformed(d);
			if (d->field == F_EXP)
				d->exp = n;
			else if (d->field == F_NBF)
				d->nbf = n;
			else
				d->iat = n;
			break;

		default:
			break;
	}
	return JSON_SUCCESS;
}

/* Read one JSON document into d.  NULL when it parsed and no rule failed. */
static const char *
parse(Doc *d, const char *json, size_t len)
{
	JsonLexContext stack;
	JsonLexContext *lex;
	JsonSemAction sem = {
		.semstate = d,
		.object_start = on_object_start,
		.object_end = on_object_end,
		.array_start = on_array_start,
		.array_end = on_array_end,
		.object_field_start = on_field_start,
		.object_field_end = on_field_end,
		.scalar = on_scalar,
	};
	JsonParseErrorType r;

	lex = makeJsonLexContextCstringLen(&stack, json, len, PG_UTF8, true);
	setJsonLexContextOwnsTokens(lex, true);
	r = pg_parse_json(lex, &sem);
	freeJsonLexContext(lex);

	if (r != JSON_SUCCESS)
		return d->reason ? d->reason
			: (d->claims ? "malformed claims" : "malformed header");
	return NULL;
}

const char *
hanzo_iam_header(const char *json, size_t len, char *kid)
{
	Doc			d;
	const char *reason;

	memset(&d, 0, sizeof(d));
	reason = parse(&d, json, len);
	if (reason)
		return reason;
	if (!(d.seen & SEEN(F_ALG)))
		return "unsupported algorithm";
	if (!(d.seen & SEEN(F_KID)))
		return "bad key id";
	strlcpy(kid, d.kid, HANZO_IAM_MAX_KID + 1);
	return NULL;
}

const char *
hanzo_iam_claims(const char *json, size_t len, int64 now, char *role)
{
	Doc			d;
	const char *reason;

	memset(&d, 0, sizeof(d));
	d.claims = true;
	reason = parse(&d, json, len);
	if (reason)
		return reason;

	if (!(d.seen & SEEN(F_ISS)))
		return "wrong issuer";
	if (!d.aud)
		return "wrong audience";
	if (!(d.seen & SEEN(F_TYP)))
		return "wrong token type";
	if (!(d.seen & SEEN(F_SUB)))
		return "bad subject";
	if (!(d.seen & SEEN(F_EXP)))
		return "missing exp";
	if (!(d.seen & SEEN(F_IAT)))
		return "missing iat";

	if (d.exp + HANZO_IAM_SKEW < now)
		return "token expired";
	if (d.iat > now + HANZO_IAM_SKEW ||
		((d.seen & SEEN(F_NBF)) && d.nbf > now + HANZO_IAM_SKEW))
		return "token not yet valid";
	if (d.exp <= d.iat || d.exp - d.iat > HANZO_IAM_LIFETIME)
		return "lifetime out of range";

	strlcpy(role, d.role, HANZO_IAM_MAX_ROLE + 1);
	return NULL;
}

/*
 * base64url without padding.  The number of bytes written, or -1 for input
 * that is not base64url or that decodes to more than cap bytes; out is never
 * written past cap.
 */
static int
unb64(const char *in, size_t len, uint8 *out, size_t cap)
{
	size_t		i;
	size_t		n = 0;
	uint32		acc = 0;
	int			bits = 0;

	if (len % 4 == 1)
		return -1;
	for (i = 0; i < len; i++)
	{
		char		c = in[i];
		int			v;

		if (c >= 'A' && c <= 'Z')
			v = c - 'A';
		else if (c >= 'a' && c <= 'z')
			v = c - 'a' + 26;
		else if (c >= '0' && c <= '9')
			v = c - '0' + 52;
		else if (c == '-')
			v = 62;
		else if (c == '_')
			v = 63;
		else
			return -1;

		acc = (acc << 6) | v;
		bits += 6;
		if (bits >= 8)
		{
			bits -= 8;
			if (n >= cap)
				return -1;
			out[n++] = (acc >> bits) & 0xFF;
			acc &= (UINT32_C(1) << bits) - 1;
		}
	}
	return (int) n;
}

/* RSASSA-PKCS1-v1_5 with SHA-256 over msg. */
static const char *
rs256(EVP_PKEY *key, const uint8 *msg, size_t msglen,
	  const uint8 *sig, size_t siglen)
{
	EVP_MD_CTX *ctx;
	bool		ok;

	if (EVP_PKEY_base_id(key) != EVP_PKEY_RSA || EVP_PKEY_bits(key) < MIN_KEY_BITS)
		return "unusable key";

	ctx = EVP_MD_CTX_new();
	ok = ctx != NULL &&
		EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, key) == 1 &&
		EVP_DigestVerify(ctx, sig, siglen, msg, msglen) == 1;
	EVP_MD_CTX_free(ctx);
	ERR_clear_error();

	return ok ? NULL : "bad signature";
}

const char *
hanzo_iam_check(const char *token, size_t len, int64 now,
				hanzo_iam_keys keys, void *arg, char *role)
{
	uint8		header[HANZO_IAM_MAX_TOKEN * 3 / 4];
	uint8		claims[HANZO_IAM_MAX_TOKEN * 3 / 4];
	uint8		sig[HANZO_IAM_MAX_TOKEN * 3 / 4];
	char		kid[HANZO_IAM_MAX_KID + 1];
	const char *end = token + len;
	const char *dot1;
	const char *dot2;
	int			hlen;
	int			clen;
	int			slen;
	const char *reason;
	EVP_PKEY   *key;

	if (len > HANZO_IAM_MAX_TOKEN)
		return "token too large";

	/* Exactly three non-empty parts. */
	dot1 = memchr(token, '.', len);
	dot2 = dot1 ? memchr(dot1 + 1, '.', end - (dot1 + 1)) : NULL;
	if (dot2 == NULL || memchr(dot2 + 1, '.', end - (dot2 + 1)) != NULL ||
		dot1 == token || dot2 == dot1 + 1 || dot2 + 1 == end)
		return "malformed token";

	hlen = unb64(token, dot1 - token, header, sizeof(header));
	clen = unb64(dot1 + 1, dot2 - (dot1 + 1), claims, sizeof(claims));
	slen = unb64(dot2 + 1, end - (dot2 + 1), sig, sizeof(sig));
	if (hlen < 0 || clen < 0 || slen < 0)
		return "malformed token";

	reason = hanzo_iam_header((const char *) header, hlen, kid);
	if (reason)
		return reason;

	key = keys(kid, arg);
	if (key == NULL)
		return "unknown key";
	reason = rs256(key, (const uint8 *) token, dot2 - token, sig, slen);
	EVP_PKEY_free(key);
	if (reason)
		return reason;

	return hanzo_iam_claims((const char *) claims, clen, now, role);
}
