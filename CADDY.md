# Caddy on BareMetal

`caddy.app` is the [Caddy](https://caddyserver.com/) web server (v2.11.9,
the stock module set) as a BareMetal app. It's built on the Go port
(`GO.md`) and runs the Caddyfile on the disk image.

```
APP_NAME=caddy.app ./build-go-app.sh port/caddy_port   # setup.sh does this when Go is installed
port/caddy_port/install.sh /path/to/disk.img [Caddyfile] [site-dir]
```

From the top-level `BareMetal-App` repo, `./1-build.sh caddy [Caddyfile]
[site-dir]` does both and builds `baremetal.elf` (see `caddy.md` there).

## What works

Tested under QEMU (`2-run-qemu.sh` with port forwarding) and Firecracker:

- **HTTP:** static files (`file_server`), `encode gzip`, `respond`,
  access logs to the console. 200 requests, 10 at a time, all served.
- **HTTPS with `tls internal`:** Caddy creates its local CA, keys and
  certificates on the EXT2 disk (`/caddy/data`) and reuses them on the next
  boot. HTTP/2 and HTTP/3 (QUIC over UDP) both work; 200 HTTPS requests,
  10 at a time, averaged 12 ms including the handshake.
- **The admin API** (`localhost:2019`) starts if you leave it on. The
  default Caddyfile turns it off, since nothing outside the VM can reach
  it.
- **Idle:** about 0.1% of a host CPU.

Automatic HTTPS from a public CA (Let's Encrypt / ZeroSSL) should work
with a real domain that points at the VM, but is untested. Everything it
uses (outbound HTTPS, DNS, certificate storage) is tested.

## Layout on disk.img

| Path | What |
|---|---|
| `/caddy/Caddyfile` | The config. `caddy.app` runs `caddy run --config /caddy/Caddyfile --adapter caddyfile` |
| `/caddy/www/` | The sample site's files |
| `/caddy/data/` | Caddy's data: certificates, its local CA, locks |
| `/caddy/config/` | Config autosave |

`install.sh` replaces the Caddyfile and `/caddy/www`, and leaves
`/caddy/data` alone, so certificates survive a redeploy.

## How it works

- `port/caddy_port/main.go` imports Caddy's standard modules and calls
  `caddycmd.Main()`, Caddy's own command-line entry point. With no
  arguments it runs the Caddyfile above. Arguments from Firecracker's
  `args=` token (`./2-run.sh <args>`) replace that command line, e.g.
  `./2-run-qemu.sh version` or `./2-run-qemu.sh validate --config
  /caddy/Caddyfile --adapter caddyfile`.
- `port/caddy_port/bmenv` sets `HOME` and `XDG_DATA_HOME`/`XDG_CONFIG_HOME`
  to `/caddy/...`. BareMetal apps have no environment, and Caddy computes
  its storage paths into package-level variables, so this has to happen
  during package initialisation, before Caddy's own packages initialise.
- `go.mod`/`go.sum` pin Caddy and all its dependencies. The first build
  downloads them into the Go module cache.

## What the port needed

Beyond the Go port itself (`GO.md`), Caddy needed these shim changes, all
in `posix_shim.c`/`ext4_shim.c` and available to every app:

- **Lazy, aligned `PROT_NONE` reservations.** Go reserves 128 MiB for
  each 64 MiB heap arena and commits a few MiB at a time. Backing
  reservations with memory up front ran out of RAM during Caddy's package
  initialisation. Now a reservation is only address space until
  `MAP_FIXED` or `mprotect()` commits part of it, and reservations of
  64 MiB or more start on a 64 MiB boundary, so Go keeps the whole
  128 MiB as two arenas instead of trimming it. Before that, the trimmed
  ends fragmented the free list and about one boot in five ran out of
  memory with 126 MiB free.
- **`fsync`/`fdatasync`** (certificate storage syncs its lock files), the
  **`chmod`** family (it sets modes on keys and certificates),
  **`O_CREAT|O_EXCL`** (its lock files), `readlinkat`, `sysinfo`, and
  **`sendmmsg`/`recvmmsg`** (HTTP/3 batches its UDP I/O).

`sysinfo` was an instructive bug: musl's `struct sysinfo` has 256 more
reserved bytes than the kernel's, so zeroing `sizeof(struct sysinfo)`
wrote past the end of the Go caller's buffer and crashed at startup with a
jump to a garbage address.

## Limits

- **Memory.** Under Firecracker: boot memory of at least 60 MiB, and an
  even number (`MEMSIZE=64 ./2-run.sh`); the rest is hot-plugged as
  needed. The image is about 57 MiB, and an odd `MEMSIZE` hits a
  pre-existing hot-plug bug (`OPENISSUES.md`). Under QEMU, which has no
  hot-plug, 256 MiB (its default) works and 192 MiB doesn't. On the host,
  an idle Caddy VM uses about 180 MiB (a plain C app's VM: 44 MiB; the
  small Go web server example: 67 MiB).
- **About 12 connections at once** (16 sockets in `net_shim.c`).
- **IPv4 only.**
- **A harmless warning at startup:** `failed to set GOMAXPROCS ... open
  /proc/self/mountinfo` -- Caddy looks for Linux cgroup limits, which don't
  exist here. It runs with one CPU either way.
- **Nothing that needs a process model:** no `exec`-based modules.
