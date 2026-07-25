#ifndef _TLS_SHIM_H
#define _TLS_SHIM_H

#include <stddef.h>

// A blocking HTTPS-shaped TLS client connection: connect, then read/write
// the plaintext application data (an HTTP request/response, typically),
// then close. See tls_shim.c's file header for what this deliberately
// does *not* do (certificate verification).
typedef struct tls_conn tls_conn;

// Resolves host (via gethostbyname() -- see dns_shim.c), opens a TCP
// connection to host:port, and performs a TLS handshake over it (SNI is
// set from host). Returns NULL on any failure (DNS, TCP connect, or TLS
// handshake).
tls_conn *tls_connect(const char *host, int port);

// Write/read plaintext application data over an established connection.
// Same return convention as send()/recv(): >=0 is a byte count, <0 is
// -errno. Blocks (bounded by net_shim.c's usual 30s timeout) like the
// plain TCP path fetch() in crawler.c uses.
long tls_write(tls_conn *c, const void *buf, size_t len);
long tls_read(tls_conn *c, void *buf, size_t len);

// Closes the TLS session and the underlying TCP connection, and frees c's
// slot. Safe to call with c == NULL (no-op).
void tls_close(tls_conn *c);

#endif
