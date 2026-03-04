-- Hanzo SQL: Enable extensions
-- Using DO blocks so individual extension failures don't abort the init script.
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cron;
CREATE EXTENSION IF NOT EXISTS postgis;
CREATE EXTENSION IF NOT EXISTS tsm_system_rows;

-- DocumentDB extensions may fail if shared_preload_libraries config differs;
-- wrap in DO block to keep PG running regardless.
DO $$ BEGIN
  CREATE EXTENSION IF NOT EXISTS documentdb_core;
EXCEPTION WHEN OTHERS THEN
  RAISE WARNING 'documentdb_core extension not loaded: %', SQLERRM;
END $$;

DO $$ BEGIN
  CREATE EXTENSION IF NOT EXISTS documentdb;
EXCEPTION WHEN OTHERS THEN
  RAISE WARNING 'documentdb extension not loaded: %', SQLERRM;
END $$;
