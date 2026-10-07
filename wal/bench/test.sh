#!/bin/sh
# The wal bench: commit cost with the sinks as the synchronous standbys, then
# two crashes that each lose the primary's disk, restored from S3 alone.
set -eu
export PATH=/usr/lib/postgresql/18/bin:/shared:$PATH
export WAL_S3_BUCKET="sqlwal-$(head -c4 /dev/urandom | od -An -tx1 | tr -d ' \n')" WAL_S3_PREFIX=sql
D=/work; mkdir -p $D; chown postgres $D; cd $D
as() { gosu postgres "$@"; }
q() { as psql -h /tmp -p "$1" -U postgres -d postgres -Atq -c "$2"; }
s3util make; echo "bucket $WAL_S3_BUCKET"
stopsinks() { [ -f $D/sinks.pid ] && { kill $(cat $D/sinks.pid) 2>/dev/null || true; rm -f $D/sinks.pid; }; sleep 1; }
cleanup() { stopsinks; for p in a b c; do [ -d $D/$p ] && as pg_ctl -D $D/$p stop -m immediate >/dev/null 2>&1 || true; done; s3util drop || true; }
trap 'rc=$?; [ $rc = 0 ] || { echo "FAILED rc=$rc"; for f in $D/*.log; do echo "--- $f"; tail -15 $f; done; }; cleanup' EXIT

conf() { cat >> "$1/postgresql.conf" <<C
port=$2
listen_addresses='127.0.0.1'
unix_socket_directories='/tmp'
wal_level=replica
max_wal_senders=10
max_replication_slots=10
synchronous_commit=on
archive_mode=on
archive_command='wal push %p %f'
restore_command='wal restore %f %p'
recovery_target_timeline='latest'
shared_buffers=512MB
max_wal_size=4GB
wal_keep_size=1GB
C
}
sinks() { # PORT
  for s in sink_a sink_b; do
    every=0s; [ $s = sink_b ] && every=200ms
    WAL_NAME=$s WAL_EVERY=$every WAL_SOURCE="host=127.0.0.1 port=$1 user=postgres replication=true application_name=$s" \
      nohup wal sink > $D/$s.$1.log 2>&1 &
    echo $! >> $D/sinks.pid
  done
}
waitsync() { # PORT
  for i in $(seq 1 60); do n=$(q $1 "select count(*) from pg_stat_replication where application_name like 'sink_%' and sync_state='quorum'"); [ "$n" = 2 ] && return 0; sleep 1; done
  echo "sinks never reached quorum"; q $1 "select application_name, state, sync_state from pg_stat_replication"; tail -5 $D/sink_*.log; exit 1
}
chunks() { s3util count sql/wal/ ; }
bench() { # LABEL
  for c in 1 8 32; do
    j=$c; [ $c -gt 8 ] && j=8
    rm -f $D/l.*
    out=$(as pgbench -h /tmp -p 5432 -U postgres -b simple-update -c $c -j $j -T 30 -l --log-prefix=$D/l postgres 2>&1)
    tps=$(echo "$out" | awk '/^tps/ {print $3; exit}')
    echo "$1 c=$c tps=$tps $(python3 /bench/lat.py $D/l)"
  done
}
verify() { # PORT FILE...
  missing=0; total=0
  for f in "$@"; do [ "$f" = "$1" ] && continue
    n=$(wc -l < $f); total=$((total+n))
    m=$(as psql -h /tmp -p $1 -U postgres -d postgres -Atq -v ON_ERROR_STOP=1 <<V
create temp table w(id bigint);
\copy w from '$f'
select count(*) from w left join acked a using (id) where a.id is null;
V
)
    missing=$((missing + m))
  done
  echo "verify: $total acknowledged ids, $missing missing; rows $(q $1 'select count(*) from acked')"
  [ "$missing" = 0 ]
}
restore() { # FROM-BASE DIR PORT
  cp -a $1 $2; chown -R postgres $2
  as touch $2/standby.signal
  sed -i "s/^port=.*/port=$3/" $2/postgresql.conf
  as pg_ctl -D $2 -l $D/$(basename $2).log -w -t 600 start >/dev/null
  q $3 "select pg_create_physical_replication_slot('sink_a', true), pg_create_physical_replication_slot('sink_b', true)" >/dev/null
  t0=$(date +%s.%N)
  WAL_LOCAL="host=/tmp port=$3 user=postgres dbname=postgres" as env PATH=$PATH wal gate
  as pg_ctl -D $2 -w promote >/dev/null
  echo "restore: gate and promote in $(python3 -c "import time;print(round(time.time()-$t0,1))")s; timeline $(q $3 'select timeline_id from pg_control_checkpoint()')"
}

# The primary.
as initdb -D $D/a -U postgres --auth=trust >/dev/null
conf $D/a 5432
as pg_ctl -D $D/a -l $D/a.log -w start >/dev/null
q 5432 "select pg_create_physical_replication_slot('sink_a', true), pg_create_physical_replication_slot('sink_b', true)" >/dev/null
sinks 5432
as pgbench -h /tmp -p 5432 -U postgres -i -s 10 -q postgres >/dev/null 2>&1

echo "== commit cost (measured on the previous run)"
q 5432 "alter system set synchronous_standby_names = 'ANY 1 (sink_a, sink_b)'" ; q 5432 "select pg_reload_conf()" >/dev/null
waitsync 5432

echo "== crash 1: the primary's disk is lost"
q 5432 "create table acked(id bigserial primary key)"
as pg_basebackup -h /tmp -p 5432 -U postgres -D $D/base -X none -c fast
python3 /bench/writer.py 5432 16 $D/acked1 & w=$!
sleep 20; as pg_ctl -D $D/a stop -m immediate >/dev/null; wait $w
stopsinks; rm -rf $D/a
restore $D/base $D/b 5433
# A promoted member commits nothing until a sink has its WAL: the sinks follow it.
sinks 5433
waitsync 5433
verify 5433 $D/acked1

echo "== crash 2: on timeline 2, the new primary's disk is lost"
python3 /bench/writer.py 5433 16 $D/acked2 & w=$!
sleep 20; as pg_ctl -D $D/b stop -m immediate >/dev/null; wait $w
stopsinks; rm -rf $D/b
restore $D/base $D/c 5434
sinks 5434
waitsync 5434
verify 5434 $D/acked1 $D/acked2
echo "objects: $(s3util count sql/)"
echo BENCH-PASS
