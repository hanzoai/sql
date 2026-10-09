/*-------------------------------------------------------------------------
 *
 * hanzo_iam.c
 *	  OAuth validator for hanzo-sql.
 *
 * The server hands this module the bearer token of a connection that chose
 * the oauth method in pg_hba.conf.  The token is a hanzo-sql token (see
 * token.c): IAM's own tokens never verify, because trust is only the public
 * key files <kid>.pem in the directory hanzo_iam.dir, one per signing key.
 * Rotation adds a file, runs both keys, then removes the old file.  A key is
 * read from its file on every connection, so a removed file stops working at
 * once.  Nothing here talks to IAM.
 *
 * The token's subject is "sql:<role>".  The role is the authenticated
 * identity, and the connection is authorized only when it is the role the
 * client asked for, so pg_hba.conf lines use delegate_ident_mapping=1.
 *
 * Why a token was refused goes to the server log as one fixed reason, never to
 * the client.  A key file that cannot be opened or is not a PEM public key is
 * also named by its path in hanzo_iam.dir; the only client text in that path
 * is a kid, which has passed the key id rule (letters, digits, '.', '_', '-').
 *
 * contrib/hanzo_iam/hanzo_iam.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>

#include "fmgr.h"
#include "libpq/oauth.h"
#include "storage/fd.h"
#include "utils/guc.h"

#include "token.h"

PG_MODULE_MAGIC;

#define MAX_KEY_FILE	16384	/* bytes; an RSA-4096 public key is under 1 KiB */

static char *key_dir = NULL;

static bool validate(const ValidatorModuleState *state, const char *token,
					 const char *role, ValidatorModuleResult *result);

static const OAuthValidatorCallbacks callbacks = {
	PG_OAUTH_VALIDATOR_MAGIC,
	.validate_cb = validate
};

void
_PG_init(void)
{
	DefineCustomStringVariable("hanzo_iam.dir",
							   "Directory of the <kid>.pem public keys that sign hanzo-sql tokens.",
							   NULL,
							   &key_dir,
							   NULL,
							   PGC_SIGHUP,
							   0,
							   NULL, NULL, NULL);

	MarkGUCPrefixReserved("hanzo_iam");
}

const OAuthValidatorCallbacks *
_PG_oauth_validator_module_init(void)
{
	return &callbacks;
}

/* The public key in hanzo_iam.dir/<kid>.pem, or NULL.  The kid is checked. */
static EVP_PKEY *
load_key(const char *kid, void *arg)
{
	char		path[MAXPGPATH];
	char		pem[MAX_KEY_FILE + 1];
	FILE	   *fp;
	size_t		n;
	BIO		   *bio;
	EVP_PKEY   *key;

	if (key_dir == NULL || key_dir[0] == '\0')
	{
		ereport(LOG, errmsg("hanzo_iam: hanzo_iam.dir is not set"));
		return NULL;
	}
	if (snprintf(path, sizeof(path), "%s/%s.pem", key_dir, kid) >= (int) sizeof(path))
		return NULL;

	fp = AllocateFile(path, "r");
	if (fp == NULL)
	{
		if (errno != ENOENT)
			ereport(LOG, errmsg("hanzo_iam: could not open \"%s\": %m", path));
		return NULL;
	}
	n = fread(pem, 1, sizeof(pem), fp);
	FreeFile(fp);
	if (n == 0 || n > MAX_KEY_FILE)
		return NULL;

	bio = BIO_new_mem_buf(pem, (int) n);
	key = bio ? PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL) : NULL;
	BIO_free(bio);
	ERR_clear_error();

	if (key == NULL)
		ereport(LOG, errmsg("hanzo_iam: \"%s\" is not a PEM public key", path));
	return key;
}

static bool
validate(const ValidatorModuleState *state, const char *token,
		 const char *role, ValidatorModuleResult *result)
{
	char		subject[HANZO_IAM_MAX_ROLE + 1];
	const char *reason;

	result->authorized = false;
	result->authn_id = NULL;

	reason = hanzo_iam_check(token, strlen(token), (int64) time(NULL),
							 load_key, NULL, subject);
	if (reason != NULL)
	{
		ereport(LOG, errmsg("hanzo_iam: %s", reason));
		return true;
	}

	/* The token is genuine: name its role, whoever it was presented for. */
	result->authn_id = pstrdup(subject);
	if (strcmp(subject, role) != 0)
	{
		ereport(LOG, errmsg("hanzo_iam: role mismatch"));
		return true;
	}

	result->authorized = true;
	return true;
}
