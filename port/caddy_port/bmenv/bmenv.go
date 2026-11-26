// Package bmenv gives caddy.app the environment a BareMetal app doesn't
// have. It must run before Caddy's own package initialisation, which
// computes its data/config paths into package-level variables (e.g.
// caddy.ConfigAutosavePath): main imports it, and Go initialises a
// package whose only dependency is os before any github.com/... package
// that is waiting on more (packages that are ready are initialised in
// import-path order).
package bmenv

import "os"

// Home is where Caddy keeps its data (certificates, its local CA) and its
// config autosave, on disk.img. install.sh creates it.
const Home = "/caddy"

func init() {
	os.Setenv("HOME", Home)
	os.Setenv("XDG_DATA_HOME", Home+"/data")
	os.Setenv("XDG_CONFIG_HOME", Home+"/config")
}
