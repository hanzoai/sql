/*-------------------------------------------------------------------------
 *
 * fuzz.c
 *	  libFuzzer target for token.c.
 *
 * The first input byte picks the entry point, the rest is its input:
 *
 *	  0	 a whole token, which ends at the signature
 *	  1	 claims JSON
 *	  2	 header JSON
 *	  3	 header JSON, a NUL, then claims JSON, signed by a key the target makes
 *		 and encoded as a token, so the claims parser reads what a signed
 *		 token carries
 *
 * The clock is fixed at NOW.  A token or JSON that passes must also leave
 * a role or kid that fits its buffer and its rules.
 *
 * A token is refused past 8192 bytes, so -max_len must be well past that for
 * the target to see the bound from both sides; corpus/token-8192, token-8193
 * and signed-8192 sit on it.
 *
 *	  make fuzz
 *	  fuzz/fuzz -max_len=9500 -dict=fuzz/token.dict -max_total_time=600 fuzz/corpus
 *
 * contrib/hanzo_iam/fuzz/fuzz.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <openssl/evp.h>

#include "token.h"

#define NOW		INT64CONST(1800000000)

static EVP_PKEY *key;

static EVP_PKEY *
lookup(const char *kid, void *arg)
{
	EVP_PKEY_up_ref(key);
	return key;
}

int
LLVMFuzzerInitialize(int *argc, char ***argv)
{
	key = EVP_PKEY_Q_keygen(NULL, NULL, "RSA", (size_t) 2048);
	if (key == NULL)
		abort();
	return 0;
}

/* base64url, no padding; the number of bytes written. */
static size_t
b64(const uint8 *in, size_t len, char *out)
{
	static const char abc[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	size_t		n = 0;
	size_t		i;

	for (i = 0; i + 2 < len; i += 3)
	{
		uint32		v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];

		out[n++] = abc[v >> 18];
		out[n++] = abc[(v >> 12) & 63];
		out[n++] = abc[(v >> 6) & 63];
		out[n++] = abc[v & 63];
	}
	if (len - i == 1)
	{
		out[n++] = abc[in[i] >> 2];
		out[n++] = abc[(in[i] & 3) << 4];
	}
	else if (len - i == 2)
	{
		uint32		v = (in[i] << 8) | in[i + 1];

		out[n++] = abc[v >> 10];
		out[n++] = abc[(v >> 4) & 63];
		out[n++] = abc[(v & 15) << 2];
	}
	return n;
}

/* header.claims.signature, signed RS256 with key; NUL-terminated, or NULL. */
static char *
sign(const uint8 *header, size_t hlen, const uint8 *claims, size_t clen, size_t *len)
{
	size_t		cap = (hlen + clen) * 4 / 3 + 8 + 400;
	char	   *tok = malloc(cap);
	uint8		sig[512];
	size_t		siglen = sizeof(sig);
	size_t		n = 0;
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	int			ok;

	if (tok == NULL || ctx == NULL)
		abort();
	n += b64(header, hlen, tok + n);
	tok[n++] = '.';
	n += b64(claims, clen, tok + n);
	ok = EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, key) == 1 &&
		EVP_DigestSign(ctx, sig, &siglen, (const uint8 *) tok, n) == 1;
	EVP_MD_CTX_free(ctx);
	if (!ok)
		abort();
	tok[n++] = '.';
	n += b64(sig, siglen, tok + n);
	tok[n] = '\0';
	*len = n;
	return tok;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char		role[HANZO_IAM_MAX_ROLE + 1];
	char		kid[HANZO_IAM_MAX_KID + 1];
	const char *in = (const char *) data + 1;
	const char *reason = "";
	char	   *tok;
	const uint8 *nul;
	size_t		len;

	if (size == 0)
		return 0;
	size--;

	switch (data[0] % 4)
	{
		case 0:
			reason = hanzo_iam_check(in, size, NOW, lookup, NULL, role);
			break;
		case 1:
			reason = hanzo_iam_claims(in, size, NOW, role);
			break;
		case 2:
			reason = hanzo_iam_header(in, size, kid);
			if (reason == NULL && (kid[0] == '\0' || kid[0] == '.' || strchr(kid, '/')))
				abort();
			return 0;
		case 3:
			nul = memchr(in, '\0', size);
			if (nul == NULL)
				return 0;
			tok = sign((const uint8 *) in, nul - (const uint8 *) in,
					   nul + 1, size - (nul + 1 - (const uint8 *) in), &len);
			reason = hanzo_iam_check(tok, len, NOW, lookup, NULL, role);
			free(tok);
			break;
	}

	if (reason == NULL && (role[0] == '\0' || strlen(role) > HANZO_IAM_MAX_ROLE))
		abort();
	return 0;
}
