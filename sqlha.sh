#!/bin/sh
# sqlha — sql as one member of a replicated cluster.
#
# Patroni holds the cluster's leader lease in the Kubernetes API, streams the
# leader's WAL to every other member, and promotes a member when the leader's
# lease lapses. A leader that cannot renew its lease demotes itself, so two
# members never take writes at once. It labels each pod with its role, and the
# role label is what the sql Service selects.
#
# The cluster's shape is the Patroni config the deployment mounts at
# SQL_HA_CONFIG. What differs per pod comes from the downward API, here.
set -eu

: "${SQL_HA_CONFIG:?}" "${POD_NAME:?}" "${POD_IP:?}" "${POD_NAMESPACE:?}"
: "${SQL_USER:?}" "${SQL_PASSWORD:?}" "${SQL_REPLICATION_USER:?}" "${SQL_REPLICATION_PASSWORD:?}"

export PATRONI_NAME="$POD_NAME"
export PATRONI_KUBERNETES_NAMESPACE="$POD_NAMESPACE"
export PATRONI_KUBERNETES_POD_IP="$POD_IP"
export PATRONI_POSTGRESQL_CONNECT_ADDRESS="$POD_IP:5432"
export PATRONI_RESTAPI_CONNECT_ADDRESS="$POD_IP:8008"
export PATRONI_SUPERUSER_USERNAME="$SQL_USER"
export PATRONI_SUPERUSER_PASSWORD="$SQL_PASSWORD"
export PATRONI_REWIND_USERNAME="$SQL_USER"
export PATRONI_REWIND_PASSWORD="$SQL_PASSWORD"
export PATRONI_REPLICATION_USERNAME="$SQL_REPLICATION_USER"
export PATRONI_REPLICATION_PASSWORD="$SQL_REPLICATION_PASSWORD"

# The data directory must belong to postgres and be private to it. Walking a
# populated directory on every boot is slow, so only a mismatch is repaired.
data="${SQL_DATA:-/var/lib/postgresql/data/pgdata}"
mkdir -p "$data"
parent=$(dirname "$data")
[ "$(stat -c %U "$parent")" = postgres ] || chown -R postgres:postgres "$parent"
chmod 700 "$data"

exec gosu postgres patroni "$SQL_HA_CONFIG"
