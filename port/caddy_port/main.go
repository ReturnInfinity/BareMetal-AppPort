// caddy.app -- the Caddy web server as a BareMetal app. See CADDY.md.
package main

import (
	"os"

	// First: sets HOME/XDG_* before Caddy's packages initialise.
	_ "baremetal/caddy/bmenv"

	caddycmd "github.com/caddyserver/caddy/v2/cmd"

	// The same module set as the stock caddy binary.
	_ "github.com/caddyserver/caddy/v2/modules/standard"
)

// Written by install.sh.
const caddyfile = "/caddy/Caddyfile"

func main() {
	// argv comes from Firecracker's args= token (crt0.c); with none,
	// run the Caddyfile install.sh deployed.
	if len(os.Args) <= 1 {
		os.Args = []string{"caddy", "run", "--config", caddyfile, "--adapter", "caddyfile"}
	}
	caddycmd.Main()
}
