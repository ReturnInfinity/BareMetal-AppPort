-- hello.sql -- run with: ./1-build.sh examples/sqlite/hello/hello.sql && ./2-run.sh
-- The database (/sqlite/main.db on disk.img) persists between runs, so
-- the visit count goes up each time.
CREATE TABLE IF NOT EXISTS visits(id INTEGER PRIMARY KEY, at TEXT DEFAULT CURRENT_TIMESTAMP);
INSERT INTO visits DEFAULT VALUES;
SELECT 'Hello from SQLite ' || sqlite_version() || ' on BareMetal!' AS greeting;
SELECT count(*) AS visits FROM visits;
