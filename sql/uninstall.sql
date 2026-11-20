-- vtext uninstall: removes the library and every function. Index tables stay unless the schema
-- is dropped (scripts/deploy.sh --undeploy --drop).
DROP LIBRARY IF EXISTS vtext.vtext CASCADE;
