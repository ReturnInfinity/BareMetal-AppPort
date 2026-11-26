// webserver.go -- Go counterpart to webserver.c/webserver.zig/etc: an
// HTTP server with a hit counter, built on the standard net/http package.
// Each connection gets its own goroutine, and the runtime's netpoller
// (epoll, see GO.md) waits on the sockets.
package main

import (
	"fmt"
	"log"
	"net/http"
	"runtime"
	"sync/atomic"
)

var hits atomic.Uint64

func main() {
	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		n := hits.Add(1)
		log.Printf("%s %s from %s", r.Method, r.URL.Path, r.RemoteAddr)
		fmt.Fprintf(w, "Hello from Go %s on BareMetal! You are visitor #%d.\n", runtime.Version(), n)
	})

	log.Println("listening on :80")
	log.Fatal(http.ListenAndServe(":80", nil))
}
