// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// tls_shim.c -- a small blocking TLS client wrapper over mbedTLS, for
// apps (crawler.c) that want to fetch an https:// URL. Connects a plain
// TCP socket the same way crawler.c's own plain-HTTP fetch() does
// (gethostbyname() -- see dns_shim.c -- then socket()/connect()), then
// hands that fd to mbedTLS via net_sockets.c's mbedtls_net_send/recv
// (vendored unmodified, see scripts/get-mbedtls.sh) for the handshake
// and record layer.
//
// No certificate verification: MBEDTLS_SSL_VERIFY_NONE below, and no
// trusted CA store is configured at all. This port has no clock (see
// OPENISSUES.md -- no clock_gettime/gettimeofday/time), so there's no
// way to check a certificate's validity period even if a CA store were
// wired up; skipping verification entirely, rather than half-implementing
// it, is the honest reflection of that limitation. Good enough to get
// TLS's confidentiality (traffic isn't plaintext on the wire) for a
// crawler; not good enough to trust the server's identity -- don't
// reuse this for anything where that distinction matters.
//
// One shared mbedtls_ssl_config (handshake/cipher settings, RNG) across
// all connections, per mbedTLS's own recommended usage -- only the much
// smaller per-connection mbedtls_ssl_context varies. TLS_MAX_CONN is 2
// rather than 1 solely so a failed/lingering connection can't wedge the
// only slot; nothing in this port actually opens TLS connections
// concurrently (see net_shim.c's SOCK_MAX/ext4_shim.c's
// EXT4_SHIM_MAX_OPEN for the same small-fixed-table style elsewhere
// in this port).
// =============================================================================

#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

#include "mbedtls/net_sockets.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"

#include "tls_shim.h"

#define TLS_MAX_CONN 2

struct tls_conn {
	int in_use;
	mbedtls_net_context net;
	mbedtls_ssl_context ssl;
};

static struct tls_conn conns[TLS_MAX_CONN];

static mbedtls_entropy_context s_entropy;
static mbedtls_ctr_drbg_context s_ctr_drbg;
static mbedtls_ssl_config s_conf;
static int s_ready;

// Seeds the DRBG (via mbedtls_hardware_poll() -- see
// port/mbedtls_port/entropy_hardware_poll.c) and builds the shared TLS
// 1.2 client config, once. Idempotent/safe to call from every
// tls_connect(), like net_glue.h's net_ensure_ready().
static int ensure_ready(void)
{
	if (s_ready)
		return 1;

	mbedtls_entropy_init(&s_entropy);
	mbedtls_ctr_drbg_init(&s_ctr_drbg);
	if (mbedtls_ctr_drbg_seed(&s_ctr_drbg, mbedtls_entropy_func, &s_entropy, NULL, 0) != 0)
		return 0;

	mbedtls_ssl_config_init(&s_conf);
	if (mbedtls_ssl_config_defaults(&s_conf, MBEDTLS_SSL_IS_CLIENT,
					 MBEDTLS_SSL_TRANSPORT_STREAM,
					 MBEDTLS_SSL_PRESET_DEFAULT) != 0)
		return 0;

	mbedtls_ssl_conf_authmode(&s_conf, MBEDTLS_SSL_VERIFY_NONE);
	mbedtls_ssl_conf_rng(&s_conf, mbedtls_ctr_drbg_random, &s_ctr_drbg);

	s_ready = 1;
	return 1;
}

static struct tls_conn *alloc_slot(void)
{
	for (int i = 0; i < TLS_MAX_CONN; i++)
		if (!conns[i].in_use)
			return &conns[i];
	return NULL;
}

tls_conn *tls_connect(const char *host, int port)
{
	if (!ensure_ready())
		return NULL;

	struct tls_conn *c = alloc_slot();
	if (!c)
		return NULL;

	struct hostent *he = gethostbyname(host);
	if (!he)
		return NULL;

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return NULL;

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	memcpy(&addr.sin_addr, he->h_addr, sizeof(addr.sin_addr));

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return NULL;
	}

	c->in_use = 1;
	mbedtls_net_init(&c->net);
	c->net.fd = fd;
	mbedtls_ssl_init(&c->ssl);

	// SNI -- required by most real-world HTTPS hosting (virtual hosts /
	// CDNs picking which certificate to present based on this).
	if (mbedtls_ssl_setup(&c->ssl, &s_conf) != 0 ||
	    mbedtls_ssl_set_hostname(&c->ssl, host) != 0) {
		tls_close(c);
		return NULL;
	}

	mbedtls_ssl_set_bio(&c->ssl, &c->net, mbedtls_net_send, mbedtls_net_recv, NULL);

	int ret;
	while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
		if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
			tls_close(c);
			return NULL;
		}
	}

	return c;
}

long tls_write(tls_conn *c, const void *buf, size_t len)
{
	if (!c)
		return -EINVAL;

	int ret;
	do {
		ret = mbedtls_ssl_write(&c->ssl, buf, len);
	} while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);

	return ret < 0 ? -EIO : ret;
}

long tls_read(tls_conn *c, void *buf, size_t len)
{
	if (!c)
		return -EINVAL;

	int ret;
	do {
		ret = mbedtls_ssl_read(&c->ssl, buf, len);
	} while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);

	if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
		return 0;
	return ret < 0 ? -EIO : ret;
}

void tls_close(tls_conn *c)
{
	if (!c)
		return;

	// Best-effort -- ignore errors/retry-needed, we're tearing the
	// connection down regardless.
	mbedtls_ssl_close_notify(&c->ssl);
	mbedtls_ssl_free(&c->ssl);
	mbedtls_net_free(&c->net); // closes the fd

	c->in_use = 0;
}

// =============================================================================
// EOF
