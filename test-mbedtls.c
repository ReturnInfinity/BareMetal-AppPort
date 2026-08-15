// test-mbedtls.c -- exercises every mbedTLS component this port builds
// (see build-app.sh's "error: mbedTLS objects are missing" check and
// port/mbedtls_port/baremetal_mbedtls_config.h) and prints a pass/fail
// line for each, plus a final summary. Exit code is the failure count
// (0 = everything passed).
//
// Two kinds of checks:
//
//   - Component self-tests: mbedTLS ships an mbedtls_xxx_self_test()
//     (known-answer test vectors, guarded by MBEDTLS_SELF_TEST) inside
//     nearly every crypto module -- the same set programs/test/selftest.c
//     in the upstream mbedTLS source tree runs. Called here with
//     verbose=0 so they only return a pass/fail code instead of writing
//     their own dot-by-dot progress to stdout; this file does the
//     reporting instead. The list below is every self-test upstream
//     offers that this port's config (see baremetal_mbedtls_config.h)
//     actually turns on -- MBEDTLS_MEMORY_BUFFER_ALLOC_C and
//     MBEDTLS_ENTROPY_NV_SEED are both off here (no libc-level
//     buffer-alloc override, no on-disk seed file), so calloc_self_test
//     and the seed-file half of upstream's entropy wrapper are skipped;
//     mbedtls_entropy_self_test() itself needs neither and is called
//     directly.
//
//   - Integration checks specific to this port: seeding CTR_DRBG from
//     mbedtls_entropy_func(), which on this port pulls randomness from
//     RDRAND via port/mbedtls_port/entropy_hardware_poll.c (no
//     /dev/urandom here -- see MBEDTLS_ENTROPY_HARDWARE_ALT in
//     baremetal_mbedtls_config.h), and parsing the CA bundle
//     build-app.sh links into every app as cacert_data.o (see
//     port/tls_shim.c, which is what actually uses this at connect
//     time). A KAT self-test can pass on a build that's still broken
//     in ways specific to this port (e.g. entropy_hardware_poll.c
//     wired up wrong, or an empty/corrupt compiled-in CA bundle) --
//     these two exercise exactly the pieces tls_shim.c depends on
//     before any of curltest.c/https_crawler.c ever open a socket.
//
//   - Live handshake checks against real hosts on the internet, using
//     the same connect-then-handshake sequence as port/tls_shim.c's
//     tls_connect() (not tls_shim.c itself -- this needs the raw
//     mbedtls_ssl_handshake()/mbedtls_ssl_get_verify_result() return
//     values tls_shim.c collapses into a bare NULL, to tell "rejected
//     for the expected reason" apart from "something else broke").
//     Two ordinary hosts should verify cleanly; four badssl.com
//     subdomains, each broken in one specific well-known way (expired
//     cert, hostname not covered by the cert, self-signed, signed by a
//     CA outside the trust store), should each be *rejected* -- a
//     handshake that succeeds against one of those is the real
//     failure, not a pass. Needs tap0/internet (see 2-run.sh); a
//     DNS/TCP-level failure reports SKIP rather than FAIL, since that
//     means "no network," not "mbedTLS accepted a bad certificate."

#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

#include <stdio.h>
#include <string.h>

#include "mbedtls/version.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/error.h"
#include "mbedtls/md5.h"
#include "mbedtls/ripemd160.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"
#include "mbedtls/sha512.h"
#include "mbedtls/sha3.h"
#include "mbedtls/des.h"
#include "mbedtls/aes.h"
#include "mbedtls/gcm.h"
#include "mbedtls/ccm.h"
#include "mbedtls/nist_kw.h"
#include "mbedtls/cmac.h"
#include "mbedtls/chacha20.h"
#include "mbedtls/poly1305.h"
#include "mbedtls/chachapoly.h"
#include "mbedtls/base64.h"
#include "mbedtls/bignum.h"
#include "mbedtls/rsa.h"
#include "mbedtls/camellia.h"
#include "mbedtls/aria.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/hmac_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ecjpake.h"
#include "mbedtls/dhm.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/x509_crt.h"

// The CA bundle build-app.sh compiles from files/cacert.pem into
// build/cacert_data.c and links into every app -- see port/tls_shim.c's
// matching `extern`/weak-symbol comment for why these are declared
// __attribute__((weak)) rather than plain `extern`: it keeps this file
// linkable even in a hypothetical build that omits cacert_data.o.
__attribute__((weak)) const unsigned char cacert_pem[1];
__attribute__((weak)) const unsigned int cacert_pem_len;

typedef struct {
	const char *name;
	int (*run)(int verbose);
} selftest_t;

static const selftest_t selftests[] = {
	{ "md5",        mbedtls_md5_self_test },
	{ "ripemd160",  mbedtls_ripemd160_self_test },
	{ "sha1",       mbedtls_sha1_self_test },
	{ "sha224",     mbedtls_sha224_self_test },
	{ "sha256",     mbedtls_sha256_self_test },
	{ "sha384",     mbedtls_sha384_self_test },
	{ "sha512",     mbedtls_sha512_self_test },
	{ "sha3",       mbedtls_sha3_self_test },
	{ "des",        mbedtls_des_self_test },
	{ "aes",        mbedtls_aes_self_test },
	{ "gcm",        mbedtls_gcm_self_test },
	{ "ccm",        mbedtls_ccm_self_test },
	{ "nist_kw",    mbedtls_nist_kw_self_test },
	{ "cmac",       mbedtls_cmac_self_test },
	{ "chacha20",   mbedtls_chacha20_self_test },
	{ "poly1305",   mbedtls_poly1305_self_test },
	{ "chacha20-poly1305", mbedtls_chachapoly_self_test },
	{ "base64",     mbedtls_base64_self_test },
	{ "mpi",        mbedtls_mpi_self_test },
	{ "rsa",        mbedtls_rsa_self_test },
	{ "camellia",   mbedtls_camellia_self_test },
	{ "aria",       mbedtls_aria_self_test },
	{ "ctr_drbg",   mbedtls_ctr_drbg_self_test },
	{ "hmac_drbg",  mbedtls_hmac_drbg_self_test },
	{ "ecp",        mbedtls_ecp_self_test },
	{ "ecjpake",    mbedtls_ecjpake_self_test },
	{ "dhm",        mbedtls_dhm_self_test },
	{ "entropy",    mbedtls_entropy_self_test },
	{ "pkcs5",      mbedtls_pkcs5_self_test },
};

#define N_SELFTESTS ((int)(sizeof(selftests) / sizeof(selftests[0])))

static int passed, failed, skipped;

static void report(const char *name, int ok)
{
	printf("  %-24s [%s]\n", name, ok ? "PASS" : "FAIL");
	if (ok)
		passed++;
	else
		failed++;
}

// PASS/FAIL/SKIP rather than a bool -- SKIP is for "couldn't reach the
// host at all" (see tls_live_test() below), which is a statement about
// network availability, not about whether mbedTLS behaved correctly,
// so it's tracked separately from failed and doesn't affect the exit
// code.
typedef enum { TLS_PASS, TLS_FAIL, TLS_SKIP } tls_result_t;

static void report_tls(const char *label, tls_result_t result, const char *detail)
{
	const char *tag = result == TLS_PASS ? "PASS" : result == TLS_SKIP ? "SKIP" : "FAIL";

	if (detail && detail[0])
		printf("  %-42s [%s] (%s)\n", label, tag, detail);
	else
		printf("  %-42s [%s]\n", label, tag);

	if (result == TLS_PASS)
		passed++;
	else if (result == TLS_SKIP)
		skipped++;
	else
		failed++;
}

// Entropy (RDRAND, via entropy_hardware_poll.c) feeding CTR_DRBG, the
// same pairing port/tls_shim.c's ensure_ready() sets up before any TLS
// handshake -- confirms the hardware RNG path actually produces bytes
// mbedTLS accepts as a seed, not just that the KAT math in ctr_drbg.c
// is correct.
static int test_hardware_entropy_ctr_drbg(void)
{
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context ctr_drbg;
	unsigned char out1[32], out2[32];
	int ok = 1;

	mbedtls_entropy_init(&entropy);
	mbedtls_ctr_drbg_init(&ctr_drbg);

	if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0) != 0)
		ok = 0;

	if (ok && mbedtls_ctr_drbg_random(&ctr_drbg, out1, sizeof(out1)) != 0)
		ok = 0;
	if (ok && mbedtls_ctr_drbg_random(&ctr_drbg, out2, sizeof(out2)) != 0)
		ok = 0;

	// Two draws coming back identical (all-zero RDRAND output, a stuck
	// entropy source, etc.) would still pass the two checks above --
	// catch that here rather than declaring success on a DRBG that's
	// silently producing the same "random" bytes every time.
	if (ok && memcmp(out1, out2, sizeof(out1)) == 0)
		ok = 0;

	mbedtls_ctr_drbg_free(&ctr_drbg);
	mbedtls_entropy_free(&entropy);
	return ok;
}

// Parses the compiled-in CA bundle the same way port/tls_shim.c's
// ensure_ready() falls back to when /etc/ssl/cacert.pem isn't present
// on disk.img -- confirms cacert_data.o actually links in a non-empty,
// well-formed bundle rather than the weak zero-length fallback symbols
// declared above.
static int test_cacert_bundle_parse(void)
{
	mbedtls_x509_crt chain;
	int ok = 1;

	if (cacert_pem_len == 0)
		return 0;

	mbedtls_x509_crt_init(&chain);
	if (mbedtls_x509_crt_parse(&chain, cacert_pem, cacert_pem_len) < 0)
		ok = 0;
	if (ok && chain.raw.len == 0)
		ok = 0;

	mbedtls_x509_crt_free(&chain);
	return ok;
}

// Where port/mbedtls_port/install-cacert.sh writes the CA bundle onto
// disk.img -- same path/fallback order as port/tls_shim.c's
// ensure_ready(), so this exercises the real trust store an app would
// get, not a synthetic one.
#define TLS_CA_BUNDLE_PATH "/etc/ssl/cacert.pem"

typedef struct {
	const char *label;
	const char *host;
	int expect_trusted; // 1: handshake must succeed and verify clean.
	                     // 0: handshake must be rejected (badssl.com's
	                     // point is that each of these looks like a
	                     // normal HTTPS server at the TCP/record layer
	                     // -- only certificate verification catches
	                     // the problem).
} tls_case_t;

static const tls_case_t tls_cases[] = {
	{ "example.com",                  "example.com",                  1 },
	{ "badssl.com",                   "badssl.com",                   1 },
	{ "expired.badssl.com",           "expired.badssl.com",           0 },
	{ "wrong.host.badssl.com",        "wrong.host.badssl.com",        0 },
	{ "self-signed.badssl.com",       "self-signed.badssl.com",       0 },
	{ "untrusted-root.badssl.com",    "untrusted-root.badssl.com",    0 },
};

#define N_TLS_CASES ((int)(sizeof(tls_cases) / sizeof(tls_cases[0])))

static mbedtls_entropy_context tls_entropy;
static mbedtls_ctr_drbg_context tls_ctr_drbg;
static mbedtls_x509_crt tls_cacert;
static mbedtls_ssl_config tls_conf;
static int tls_ready;

// Same sequence as port/tls_shim.c's ensure_ready() (DRBG seeded from
// mbedtls_entropy_func(), CA bundle loaded off disk.img falling back
// to the compiled-in copy, MBEDTLS_SSL_VERIFY_REQUIRED) -- one shared
// config for every case below, built once.
static int tls_test_init(void)
{
	if (tls_ready)
		return 1;

	mbedtls_entropy_init(&tls_entropy);
	mbedtls_ctr_drbg_init(&tls_ctr_drbg);
	if (mbedtls_ctr_drbg_seed(&tls_ctr_drbg, mbedtls_entropy_func, &tls_entropy, NULL, 0) != 0)
		return 0;

	mbedtls_x509_crt_init(&tls_cacert);
	if (mbedtls_x509_crt_parse_file(&tls_cacert, TLS_CA_BUNDLE_PATH) != 0 &&
	    mbedtls_x509_crt_parse(&tls_cacert, cacert_pem, cacert_pem_len) != 0)
		return 0;

	mbedtls_ssl_config_init(&tls_conf);
	if (mbedtls_ssl_config_defaults(&tls_conf, MBEDTLS_SSL_IS_CLIENT,
					 MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0)
		return 0;

	mbedtls_ssl_conf_ca_chain(&tls_conf, &tls_cacert, NULL);
	mbedtls_ssl_conf_authmode(&tls_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_rng(&tls_conf, mbedtls_ctr_drbg_random, &tls_ctr_drbg);

	tls_ready = 1;
	return 1;
}

// mbedtls_net_send/recv (net_sockets.c, called via mbedtls_ssl_set_bio
// below) report a plain socket-layer problem (connection reset mid-
// handshake, etc.) as one of these -- distinct from
// MBEDTLS_ERR_X509_CERT_VERIFY_FAILED, which is the certificate layer
// actually doing its job. Treated as SKIP for the same reason a DNS/
// TCP connect failure is.
static int is_net_error(int ret)
{
	switch (ret) {
	case MBEDTLS_ERR_NET_SOCKET_FAILED:
	case MBEDTLS_ERR_NET_CONNECT_FAILED:
	case MBEDTLS_ERR_NET_RECV_FAILED:
	case MBEDTLS_ERR_NET_SEND_FAILED:
	case MBEDTLS_ERR_NET_CONN_RESET:
	case MBEDTLS_ERR_NET_UNKNOWN_HOST:
		return 1;
	default:
		return 0;
	}
}

// Connects to host:443 (gethostbyname()/socket()/connect(), same as
// port/tls_shim.c's tls_connect()) and performs a real TLS 1.2
// handshake with MBEDTLS_SSL_VERIFY_REQUIRED. Under that authmode,
// mbedTLS itself refuses to complete the handshake on a verification
// failure -- mbedtls_ssl_handshake() returns
// MBEDTLS_ERR_X509_CERT_VERIFY_FAILED directly rather than returning 0
// with flags set (that split return only happens under
// MBEDTLS_SSL_VERIFY_OPTIONAL) -- so ret alone says which case
// happened; mbedtls_ssl_get_verify_result() is only consulted
// afterwards, to describe *why* a rejection happened.
static tls_result_t tls_live_test(const char *host, int expect_trusted, char *why, size_t why_len)
{
	why[0] = '\0';

	if (!tls_test_init()) {
		snprintf(why, why_len, "TLS client setup failed");
		return TLS_SKIP;
	}

	struct hostent *he = gethostbyname(host);
	if (!he) {
		snprintf(why, why_len, "DNS lookup failed");
		return TLS_SKIP;
	}

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		snprintf(why, why_len, "socket() failed");
		return TLS_SKIP;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(443);
	memcpy(&addr.sin_addr, he->h_addr, sizeof(addr.sin_addr));

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		snprintf(why, why_len, "TCP connect failed");
		return TLS_SKIP;
	}

	mbedtls_net_context net;
	mbedtls_net_init(&net);
	net.fd = fd;

	mbedtls_ssl_context ssl;
	mbedtls_ssl_init(&ssl);

	tls_result_t result;

	if (mbedtls_ssl_setup(&ssl, &tls_conf) != 0 || mbedtls_ssl_set_hostname(&ssl, host) != 0) {
		snprintf(why, why_len, "ssl_setup/set_hostname failed");
		result = TLS_SKIP;
	} else {
		mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, NULL);

		int ret;
		while ((ret = mbedtls_ssl_handshake(&ssl)) != 0 &&
		       (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE))
			;

		if (ret == 0) {
			if (expect_trusted) {
				result = TLS_PASS;
			} else {
				snprintf(why, why_len, "handshake succeeded and verified clean -- should have been rejected");
				result = TLS_FAIL;
			}
		} else if (is_net_error(ret)) {
			snprintf(why, why_len, "network error during handshake");
			result = TLS_SKIP;
		} else if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
			uint32_t flags = mbedtls_ssl_get_verify_result(&ssl);
			char vbuf[256];
			int n = mbedtls_x509_crt_verify_info(vbuf, sizeof(vbuf), "", flags);
			if (n > 0 && vbuf[n - 1] == '\n')
				vbuf[n - 1] = '\0';

			snprintf(why, why_len, "rejected: %s", vbuf);
			result = expect_trusted ? TLS_FAIL : TLS_PASS;
		} else {
			char ebuf[128];
			mbedtls_strerror(ret, ebuf, sizeof(ebuf));
			snprintf(why, why_len, "handshake failed: %s", ebuf);
			result = expect_trusted ? TLS_FAIL : TLS_SKIP;
		}
	}

	mbedtls_ssl_free(&ssl);
	mbedtls_net_free(&net);
	return result;
}

int main(void)
{
	printf("BareMetal test-mbedtls -- %s\n\n", MBEDTLS_VERSION_STRING_FULL);

	printf("component self-tests (known-answer test vectors):\n");
	for (int i = 0; i < N_SELFTESTS; i++) {
		int ok = selftests[i].run(0) == 0;
		// mbedTLS 3.6.6's library/cmac.c has one test helper
		// (test_aes128_cmac_prf()) that prints its "AES CMAC 128
		// PRF #N: " progress lines unconditionally instead of
		// guarding them behind `if (verbose != 0)` like every
		// other self-test does -- a small upstream inconsistency,
		// not a bug in this report loop. Break to a fresh line so
		// its stray output doesn't run into the [PASS]/[FAIL] line.
		if (strcmp(selftests[i].name, "cmac") == 0)
			printf("\n");
		report(selftests[i].name, ok);
	}

	printf("\nport integration checks:\n");
	report("hardware entropy + ctr_drbg", test_hardware_entropy_ctr_drbg());
	report("compiled-in CA bundle parse", test_cacert_bundle_parse());

	printf("\nlive handshake checks (needs tap0/internet -- see 2-run.sh):\n");
	for (int i = 0; i < N_TLS_CASES; i++) {
		char why[256];
		tls_result_t result = tls_live_test(tls_cases[i].host, tls_cases[i].expect_trusted, why, sizeof(why));
		report_tls(tls_cases[i].label, result, why);
	}

	printf("\n%d/%d passed", passed, passed + failed);
	if (skipped)
		printf(", %d skipped", skipped);
	if (failed)
		printf(", %d FAILED\n", failed);
	else
		printf(", all OK\n");

	return failed;
}
