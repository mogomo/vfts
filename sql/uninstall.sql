-- vtext uninstall: removes the library, every function and every procedure. Index tables stay
-- unless the schema is dropped (scripts/deploy.sh --undeploy --drop, which first drops every index
-- with its views and node caches).
DROP LIBRARY IF EXISTS vtext.vtext CASCADE;
DROP PROCEDURE IF EXISTS vtext.make_views(VARCHAR);
DROP PROCEDURE IF EXISTS vtext.create_index(VARCHAR, VARCHAR, VARCHAR);
DROP PROCEDURE IF EXISTS vtext.create_index(VARCHAR, VARCHAR);
DROP PROCEDURE IF EXISTS vtext.load_rows(VARCHAR, VARCHAR, VARCHAR);
DROP PROCEDURE IF EXISTS vtext.activate(VARCHAR, INT);
DROP PROCEDURE IF EXISTS vtext.load_all(VARCHAR);
DROP PROCEDURE IF EXISTS vtext.store_build(INT, INT);
DROP PROCEDURE IF EXISTS vtext.build_table();
DROP PROCEDURE IF EXISTS vtext.load_build(VARCHAR, VARCHAR);
DROP PROCEDURE IF EXISTS vtext.purge_segments(INT);
DROP PROCEDURE IF EXISTS vtext.refresh_index(VARCHAR);
DROP PROCEDURE IF EXISTS vtext.merge_step(VARCHAR, VARCHAR);
DROP PROCEDURE IF EXISTS vtext.merge(VARCHAR, INT);
DROP PROCEDURE IF EXISTS vtext.merge(VARCHAR);
DROP PROCEDURE IF EXISTS vtext.info(VARCHAR);
DROP PROCEDURE IF EXISTS vtext.drop_index(VARCHAR);
DROP PROCEDURE IF EXISTS vtext.rebuild(VARCHAR);
