#!/usr/bin/env bash
# Hanzo SQL entrypoint — our names on the outside, upstream's on the inside.
#
# The image is built FROM postgres, so the server and its own entrypoint read
# POSTGRES_* and PGDATA. Those are the BINARY's interface, not ours, and
# renaming them in a chart would produce a chart that does not boot.
#
# So the rename happens HERE, once, at the boundary: a Hanzo surface sets SQL_*,
# this maps it to what the server reads, and nothing downstream ever says
# postgres. Same move as kv:// replacing redis:// and the s3 binary replacing
# weed — the fork owns its identifiers.
#
# Upstream names still work when set, so an existing deployment is not broken by
# this landing; SQL_* wins when both are present.
set -euo pipefail

alias_env() {
  local ours="$1" theirs="$2"
  if [ -n "${!ours:-}" ]; then
    export "$theirs=${!ours}"
  fi
}

alias_env SQL_USER      POSTGRES_USER
alias_env SQL_PASSWORD  POSTGRES_PASSWORD
alias_env SQL_DB        POSTGRES_DB
alias_env SQL_DATA      PGDATA
alias_env SQL_HOST_AUTH_METHOD POSTGRES_HOST_AUTH_METHOD
alias_env SQL_INITDB_ARGS      POSTGRES_INITDB_ARGS

exec docker-entrypoint.sh "$@"
