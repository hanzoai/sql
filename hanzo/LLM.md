# Hanzo Relational (PostgreSQL Fork)

## Overview

**Hanzo Relational** is a fork of PostgreSQL optimized for the Hanzo AI platform's transactional workloads. It provides:

- **Primary OLTP Database** - Core business data
- **pgvector Extension** - Vector embeddings for AI
- **Multi-tenant Support** - Organization-scoped schemas
- **High Availability** - Streaming replication ready

Repository: https://github.com/hanzoai/sql

## Quick Start

```bash
# Start PostgreSQL with Hanzo config
cd hanzo
docker compose up -d

# Connect to PostgreSQL
docker exec -it hanzo-postgres psql -U hanzo -d hanzo
```

## Hanzo Extensions

Pre-configured extensions:
- `pgvector` - Vector similarity search
- `pg_stat_statements` - Query performance
- `uuid-ossp` - UUID generation
- `pg_trgm` - Trigram text search
- `btree_gin` - GIN index support

## Integration Points

### With hanzo/console (LangFuse fork)

Console uses PostgreSQL for metadata:
```env
DATABASE_URL=postgresql://hanzo:password@localhost:5432/hanzo
```

### With hanzo/iam (Casdoor fork)

IAM stores user/org data:
```env
driverName=postgres
dataSourceName=host=localhost port=5432 user=hanzo password=password dbname=hanzo
```

### With hanzo/base (PocketBase fork)

Base instances use embedded SQLite, but orchestration metadata in PostgreSQL.

## Syncing with Upstream

```bash
# Fetch upstream changes
git fetch upstream

# Merge upstream master
git merge upstream/master

# Keep hanzo/ directory
git checkout --ours hanzo/

git push origin master
```

## Performance Tuning

### Recommended Settings (16GB RAM)

```sql
-- postgresql.conf
shared_buffers = 4GB
effective_cache_size = 12GB
maintenance_work_mem = 1GB
checkpoint_completion_target = 0.9
wal_buffers = 64MB
default_statistics_target = 100
random_page_cost = 1.1
effective_io_concurrency = 200
work_mem = 52428kB
min_wal_size = 1GB
max_wal_size = 4GB
max_worker_processes = 8
max_parallel_workers_per_gather = 4
max_parallel_workers = 8
max_parallel_maintenance_workers = 4
```

## Docker Compose

See `hanzo/compose.yml` for local development with:
- PostgreSQL 17
- pgAdmin for management
- pgvector pre-installed

## Related Repositories

- **hanzo/console** - AI observability (uses for metadata)
- **hanzo/iam** - Identity management (users, orgs)
- **hanzo/datastore** - ClickHouse fork (OLAP)
- **hanzo/memory** - Redis fork (caching)
