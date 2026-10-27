# lighttpd support

BareMetal-AppPort ships [lighttpd](https://www.lighttpd.net/) 1.4.85 as
`lighttpd.app`, built against the same musl/lwIP/lwext4 port every other
app here uses. It follows the `python.app`/`lua.app` pattern: `./setup.sh`
builds the server once, and what changes per deployment is its config
and document root on the EXT2 disk image. lighttpd's sources compile
unmodified; the port-side work is a hand-written `config.h`, a static
module list, a small entry point, and the shim features lighttpd needs
(real `poll()`, non-blocking sockets, `/dev/null`, `dup2()` onto the std
fds -- see `OPENISSUES.md`).

## Running it

From this directory:

```
port/lighttpd_port/install.sh /path/to/disk.img [lighttpd.conf] [docroot]
```

With no extra arguments it installs `port/lighttpd_port/lighttpd.conf`
as `/lighttpd/lighttpd.conf` and the contents of `port/lighttpd_port/www/`
under `/lighttpd/www` (replacing whatever was there), plus an empty
`/lighttpd/tmp` for request-body spooling. It uses `debugfs -w`, so no
root and no loop mount. Then build the unikernel around the prebuilt
`lighttpd.app` the same way as any other app (copy it into
`BareMetal-Firecracker/sys/`, `./build.sh lighttpd.app`) and boot it;
lighttpd listens on port 80 and logs to the serial console.

The VM's arguments (see `port/crt0.c`) are appended to lighttpd's own
command line, after the `-D -f /lighttpd/lighttpd.conf` that
`port/lighttpd_port/lighttpd.c` always passes -- e.g. `-tt` checks the
config and exits.

### Writing your own config

Start from `port/lighttpd_port/lighttpd.conf`. Things that differ from a
normal lighttpd host:

- **`server.max-fds` must be at least 128.** lighttpd indexes its fd
  tables by fd number, and this port numbers sockets from 100 to 115
  (EXT2 files from 3, `/dev/null` from 90).
- **`server.max-connections` should stay at or below about 12**, so a
  burst can't take every one of `net_shim.c`'s 16 sockets.
- **Don't set `server.errorlog`, `server.pid-file`, `server.username`/
  `groupname` or `server.chroot`.** There is no syslog, no process or
  uid model, and lighttpd already logs to stderr (the serial console)
  in the foreground.
- **Use prefix/suffix/equality conditions (`=^`, `=$`, `==`), not
  regexes (`=~`).** PCRE2 isn't built in, so `=~`, and the regex forms
  of `mod_rewrite`/`mod_redirect`, aren't available.
- **IPv4 only** -- bind `0.0.0.0` or a v4 address.

## What's built in

Modules (`port/lighttpd_port/plugin-static.h`, which must match setup.sh's
`LIGHTTPD_SRCS`): `mod_access`, `mod_accesslog`, `mod_alias`, `mod_auth` +
`mod_authn_file`, `mod_dirlisting`, `mod_evhost`, `mod_expire`,
`mod_extforward`, `mod_fastcgi`, `mod_indexfile`, `mod_proxy`,
`mod_redirect`, `mod_rewrite`, `mod_scgi`, `mod_setenv`,
`mod_simple_vhost`, `mod_staticfile`, `mod_status`.

Left out, and why:

- **`mod_cgi`, and spawned FastCGI/SCGI backends** -- no `fork()`/
  `posix_spawn()`. `mod_fastcgi`/`mod_scgi`/`mod_proxy` to a backend
  that's already running somewhere else (`"host"`/`"port"`) work.
- **TLS (`mod_mbedtls` etc.), `mod_deflate`, `mod_magnet`, `mod_webdav`**
  -- each needs a library this port doesn't build for lighttpd yet.
  Mbed TLS is already vendored for curl, so `mod_mbedtls` is the
  likeliest next addition.

## How it's built

- `scripts/get-lighttpd.sh` fetches the pinned 1.4.85 tarball and checks
  it against the release's published SHA-256.
- `setup.sh` builds lighttpd's bundled `lemon` for the host and uses it
  to generate `configparser.c` from `configparser.y`. It then compiles
  `src/meson.build`'s `common_src` + `main_src` plus the listed modules,
  using `port/lighttpd_port/config.h` in place of meson's generated one.
  That file's comments give the reason for each `HAVE_*`. Functions musl
  has but this port's syscalls don't back (`pread`, `pipe2`, `sendfile`,
  `getrlimit`, file-backed `mmap`, ...) are left undefined, so lighttpd
  takes its own fallback paths.
- `server.c` is compiled with `main` renamed away, and
  `port/lighttpd_port/lighttpd.c` calls its `server_main()` instead.
- The objects go into `build/liblighttpd.a` rather than staying loose
  like the other libraries. lighttpd's globals have generic names, and
  as an archive it's only linked into an app that calls `server_main()`
  (see build-app.sh's `LIGHTTPD_LIB` comment).
- `setup.sh` finishes by building `lighttpd.app` itself.

## Tested

Under QEMU (user-mode networking with a host port forward), with the
default config:

- static files, directory listing, 404s, `HEAD`, `Range`, keep-alive,
  `/server-status`;
- a 3 MB file at about 18 MB/s, and four in parallel interleaved, all
  checksums matching;
- 80 requests in bursts of 8, with 3 idle client connections held open
  throughout;
- 200 sequential requests in 0.86 s;
- 1 MB `POST` bodies;
- `mod_proxy` to an HTTP server on the host.

Not yet run under Firecracker.
