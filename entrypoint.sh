#!/usr/bin/env bash
# Hanzo SQL entrypoint — our names, and only ours.
#
# The server underneath reads POSTGRES_* and PGDATA because it is built FROM
# postgres. That is the BINARY's interface, and it is not a Hanzo surface: a
# values file, a chart, a KMS path and an env in a manifest all say SQL_.
#
# The mapping happens HERE, once, at the boundary — the same move as kv://
# replacing redis:// and the s3 binary replacing weed. The fork owns its
# identifiers; that is what makes it a fork rather than a retag.
#
# THERE IS NO POSTGRES_* FALLBACK, deliberately. Accepting both means both keep
# being set, both get documented, and the rename never finishes — a compatibility
# shim is how an old name outlives the thing that replaced it. One name, one way.
set -euo pipefail

require() {
  local ours="$1" theirs="$2"
  if [ -z "${!ours:-}" ]; then
    echo "sql: ${ours} is required (Hanzo SQL reads ${ours}, never ${theirs})" >&2
    exit 1
  fi
  export "$theirs=${!ours}"
}

optional() {
  local ours="$1" theirs="$2"
  [ -n "${!ours:-}" ] && export "$theirs=${!ours}"
  return 0
}

require  SQL_USER      POSTGRES_USER
require  SQL_DB        POSTGRES_DB
optional SQL_PASSWORD  POSTGRES_PASSWORD
optional SQL_DATA      PGDATA
optional SQL_HOST_AUTH_METHOD POSTGRES_HOST_AUTH_METHOD
optional SQL_INITDB_ARGS      POSTGRES_INITDB_ARGS

exec docker-entrypoint.sh "$@"
