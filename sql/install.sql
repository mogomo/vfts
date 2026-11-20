-- vtext install. Run through scripts/deploy.sh (make deploy), which sets :libfile and :fenced.
-- Safe to run again: every object is created with IF NOT EXISTS or OR REPLACE.

CREATE SCHEMA IF NOT EXISTS vtext;

CREATE OR REPLACE LIBRARY vtext.vtext AS :libfile LANGUAGE 'C++';

-- Inspection
CREATE OR REPLACE TRANSFORM FUNCTION vtext.tversion AS LANGUAGE 'C++' NAME 'TVersionFactory' LIBRARY vtext.vtext :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vtext.tokenize AS LANGUAGE 'C++' NAME 'TokenizeFactory1' LIBRARY vtext.vtext :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vtext.tokenize AS LANGUAGE 'C++' NAME 'TokenizeFactory2' LIBRARY vtext.vtext :fenced;

GRANT USAGE ON SCHEMA vtext TO PUBLIC;
GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA vtext TO PUBLIC;
