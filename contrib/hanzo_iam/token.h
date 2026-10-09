/*-------------------------------------------------------------------------
 *
 * token.h
 *	  Check a hanzo-sql token: a compact JWS signed RS256 by the sql key.
 *
 * The three entry points are pure: they read their arguments and nothing
 * else, so the server module and the fuzz target share them.  Each returns
 * NULL when the input is acceptable and otherwise a static string naming the
 * first rule it broke.
 *
 * Include postgres.h or postgres_fe.h first: int64 comes from it.
 *
 * contrib/hanzo_iam/token.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HANZO_IAM_TOKEN_H
#define HANZO_IAM_TOKEN_H

#include <openssl/evp.h>

#define HANZO_IAM_ISSUER		"https://hanzo.id"
#define HANZO_IAM_AUDIENCE		"hanzo-sql"
#define HANZO_IAM_TYPE			"sql"
#define HANZO_IAM_PREFIX		"sql:"

#define HANZO_IAM_MAX_TOKEN		8192	/* bytes of the whole token */
#define HANZO_IAM_MAX_KID		64
#define HANZO_IAM_MAX_ROLE		63
#define HANZO_IAM_SKEW			60		/* seconds of clock difference */
#define HANZO_IAM_LIFETIME		600		/* seconds from iat to exp */

/*
 * Returns a new reference to the public key named kid, or NULL.  The caller
 * frees it.
 */
typedef EVP_PKEY *(*hanzo_iam_keys) (const char *kid, void *arg);

/*
 * Check the JOSE header: alg is RS256 and kid names a key file.  Copies kid
 * into kid[HANZO_IAM_MAX_KID + 1].
 */
extern const char *hanzo_iam_header(const char *json, size_t len, char *kid);

/*
 * Check the claims against the time now (seconds since the epoch).  Copies
 * the role the subject names into role[HANZO_IAM_MAX_ROLE + 1].
 */
extern const char *hanzo_iam_claims(const char *json, size_t len, int64 now,
									char *role);

/*
 * Check a whole token: its size and shape, the header, the key, the
 * signature, and only then the claims.
 */
extern const char *hanzo_iam_check(const char *token, size_t len, int64 now,
								   hanzo_iam_keys keys, void *arg,
								   char *role);

#endif							/* HANZO_IAM_TOKEN_H */
