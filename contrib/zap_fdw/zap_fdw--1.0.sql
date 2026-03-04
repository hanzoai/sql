-- ZAP binary protocol extension for hanzo/sql
-- Provides native ZAP listener on port 9651 (configurable via zap.port)

-- Create KV table for ZAP KV operations
CREATE TABLE IF NOT EXISTS _zap_kv (
    key TEXT PRIMARY KEY,
    kind TEXT NOT NULL DEFAULT '',
    value JSONB NOT NULL DEFAULT '{}'::jsonb,
    deleted BOOLEAN NOT NULL DEFAULT false,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS _zap_kv_kind_idx ON _zap_kv(kind) WHERE deleted = false;
CREATE INDEX IF NOT EXISTS _zap_kv_kind_key_idx ON _zap_kv(kind, key) WHERE deleted = false;
