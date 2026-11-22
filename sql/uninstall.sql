-- vfts uninstall: removes the library, every function and every procedure. Index tables stay
-- unless the schema is dropped (scripts/deploy.sh --undeploy --drop, which first drops every index
-- with its views and node caches).
DROP LIBRARY IF EXISTS vfts.vfts CASCADE;
DROP PROCEDURE IF EXISTS vfts.make_views(VARCHAR);
DROP PROCEDURE IF EXISTS vfts.create_index(VARCHAR, VARCHAR, VARCHAR);
DROP PROCEDURE IF EXISTS vfts.create_index(VARCHAR, VARCHAR);
DROP PROCEDURE IF EXISTS vfts.load_rows(VARCHAR, VARCHAR, VARCHAR);
DROP PROCEDURE IF EXISTS vfts.activate(VARCHAR, INT);
DROP PROCEDURE IF EXISTS vfts.load_all(VARCHAR);
DROP PROCEDURE IF EXISTS vfts.store_build(INT, INT);
DROP PROCEDURE IF EXISTS vfts.build_table();
DROP PROCEDURE IF EXISTS vfts.load_build(VARCHAR, VARCHAR);
DROP PROCEDURE IF EXISTS vfts.purge_segments(INT);
DROP PROCEDURE IF EXISTS vfts.refresh_index(VARCHAR);
DROP PROCEDURE IF EXISTS vfts.merge_step(VARCHAR, VARCHAR);
DROP PROCEDURE IF EXISTS vfts.merge(VARCHAR, INT);
DROP PROCEDURE IF EXISTS vfts.merge(VARCHAR);
DROP PROCEDURE IF EXISTS vfts.info(VARCHAR);
DROP PROCEDURE IF EXISTS vfts.drop_index(VARCHAR);
DROP PROCEDURE IF EXISTS vfts.rebuild(VARCHAR);
DROP PROCEDURE IF EXISTS vfts.detect_access(VARCHAR);
DROP PROCEDURE IF EXISTS vfts.new_key(INT);
DROP PROCEDURE IF EXISTS vfts.check_access(VARCHAR);
DROP PROCEDURE IF EXISTS vfts.upgrade_access();
