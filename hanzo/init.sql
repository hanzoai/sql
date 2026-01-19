-- Hanzo PostgreSQL Initialization

-- Enable extensions
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";
CREATE EXTENSION IF NOT EXISTS pg_trgm;
CREATE EXTENSION IF NOT EXISTS btree_gin;
CREATE EXTENSION IF NOT EXISTS pg_stat_statements;

-- Create schemas for multi-tenancy
CREATE SCHEMA IF NOT EXISTS hanzo;
CREATE SCHEMA IF NOT EXISTS console;
CREATE SCHEMA IF NOT EXISTS iam;

-- Set search path
ALTER DATABASE hanzo SET search_path TO hanzo, public;

-- Create service roles
DO $$
BEGIN
    -- Console service role
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'hanzo_console') THEN
        CREATE ROLE hanzo_console WITH LOGIN PASSWORD 'console_password';
    END IF;

    -- IAM service role
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'hanzo_iam') THEN
        CREATE ROLE hanzo_iam WITH LOGIN PASSWORD 'iam_password';
    END IF;

    -- Read-only analytics role
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'hanzo_readonly') THEN
        CREATE ROLE hanzo_readonly WITH LOGIN PASSWORD 'readonly_password';
    END IF;
END
$$;

-- Grant permissions
GRANT USAGE ON SCHEMA hanzo TO hanzo_console, hanzo_iam, hanzo_readonly;
GRANT USAGE ON SCHEMA console TO hanzo_console;
GRANT USAGE ON SCHEMA iam TO hanzo_iam;

-- Core tables in hanzo schema
CREATE TABLE IF NOT EXISTS hanzo.organizations (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    name VARCHAR(255) NOT NULL,
    slug VARCHAR(255) NOT NULL UNIQUE,
    settings JSONB DEFAULT '{}',
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS hanzo.projects (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    organization_id UUID REFERENCES hanzo.organizations(id) ON DELETE CASCADE,
    name VARCHAR(255) NOT NULL,
    slug VARCHAR(255) NOT NULL,
    type VARCHAR(50) NOT NULL CHECK (type IN ('website', 'api', 'app', 'base')),
    settings JSONB DEFAULT '{}',
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW(),
    UNIQUE(organization_id, slug)
);

-- Vector embeddings table for AI features
CREATE TABLE IF NOT EXISTS hanzo.embeddings (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    project_id UUID REFERENCES hanzo.projects(id) ON DELETE CASCADE,
    content_type VARCHAR(50) NOT NULL, -- 'document', 'message', 'code'
    content_id VARCHAR(255) NOT NULL,
    embedding vector(1536), -- OpenAI ada-002 dimensions
    metadata JSONB DEFAULT '{}',
    created_at TIMESTAMPTZ DEFAULT NOW(),
    UNIQUE(project_id, content_type, content_id)
);

-- Create vector index
CREATE INDEX IF NOT EXISTS embeddings_vector_idx ON hanzo.embeddings
USING ivfflat (embedding vector_cosine_ops) WITH (lists = 100);

-- Create text search indexes
CREATE INDEX IF NOT EXISTS organizations_name_trgm_idx ON hanzo.organizations
USING gin (name gin_trgm_ops);
CREATE INDEX IF NOT EXISTS projects_name_trgm_idx ON hanzo.projects
USING gin (name gin_trgm_ops);

-- Updated timestamp trigger
CREATE OR REPLACE FUNCTION hanzo.update_updated_at()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER organizations_updated_at
    BEFORE UPDATE ON hanzo.organizations
    FOR EACH ROW EXECUTE FUNCTION hanzo.update_updated_at();

CREATE TRIGGER projects_updated_at
    BEFORE UPDATE ON hanzo.projects
    FOR EACH ROW EXECUTE FUNCTION hanzo.update_updated_at();

-- Grant table permissions
GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA hanzo TO hanzo_console, hanzo_iam;
GRANT SELECT ON ALL TABLES IN SCHEMA hanzo TO hanzo_readonly;
GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA hanzo TO hanzo_console, hanzo_iam;
