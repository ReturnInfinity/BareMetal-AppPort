// sodium.c -- a demo/test of libsodium running as a BareMetal app: runs
// through a representative spread of the library (secret-key
// authenticated encryption, public-key authenticated encryption,
// signatures, hashing, and password hashing) and prints pass/fail for
// each. build with build-app.sh.
//
// This is also a valid *nix program of course.
//
// Unlike curltest.c/sqltest.c (which reach network sockets/TLS through
// libcurl on top of net_shim.c/tls_shim.c, or EXT2 through SQLite's
// VFS), this exercises the real, unmodified libsodium (see
// BareMetal-AppPort/scripts/get-libsodium.sh) entirely in memory -- no
// network, no filesystem.
//
// libsodium's own randombytes_sysrandom.c (which opens /dev/urandom, or
// falls back to getrandom()/getentropy()) is never compiled into this
// port at all -- none of those exist here (see OPENISSUES.md).
// port/libsodium_port/randombytes_baremetal.c stands in for it instead,
// backed by RDRAND (same technique, same fallback, as
// port/mbedtls_port/entropy_hardware_poll.c uses for mbedTLS), so
// sodium_init() below seeds every key/nonce generated in this file from
// real hardware randomness, same as it would on a normal OS.
//
// This also means every SIMD-accelerated implementation in libsodium
// (SSE2/AVX2/AES-NI/...) compiles down to nothing here -- see
// BareMetal-AppPort/setup.sh's "Building libsodium" comment -- so
// everything below runs through libsodium's portable reference C code.
// Slower, not less correct.
//
// crypto_pwhash() (Argon2) is deliberately run at OPSLIMIT_MIN/
// MEMLIMIT_MIN, not any of libsodium's INTERACTIVE/MODERATE/SENSITIVE
// presets: those assume a multi-core desktop with hundreds of MB to
// spare, and this is a single-core, memory-constrained microVM (see
// BareMetal-AppPort/port/posix_shim.c's heap comment) -- MIN limits are
// still enough to prove the KDF itself works end-to-end, just not
// something to imitate for an actual password store.
//
// sodium_malloc()/sodium_mlock() are deliberately not used anywhere
// below: they need mprotect()/mlock(), and posix_shim.c's
// __bmos_syscall implements neither (see its dispatch table) -- every
// buffer here is a plain stack/global array instead.

#include <stdio.h>
#include <string.h>

#include <sodium.h>

static int failures = 0;

static void report(const char *what, int ok)
{
	printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
	if (!ok)
		failures++;
}

static void print_hex(const char *label, const unsigned char *buf, size_t len)
{
	printf("  %s: ", label);
	for (size_t i = 0; i < len; i++)
		printf("%02x", buf[i]);
	printf("\n");
}

// crypto_secretbox: symmetric authenticated encryption. One key, shared
// out of band; round-trips a message and confirms a flipped ciphertext
// byte is rejected by the Poly1305 tag rather than silently decrypting.
static void test_secretbox(void)
{
	printf("crypto_secretbox (XSalsa20-Poly1305)\n");

	unsigned char key[crypto_secretbox_KEYBYTES];
	unsigned char nonce[crypto_secretbox_NONCEBYTES];
	crypto_secretbox_keygen(key);
	randombytes_buf(nonce, sizeof nonce);

	const char *msg = "the BareMetal microVM has no OS underneath it";
	size_t msg_len = strlen(msg);
	unsigned char ciphertext[crypto_secretbox_MACBYTES + 64];
	unsigned char decrypted[64];

	crypto_secretbox_easy(ciphertext, (const unsigned char *)msg, msg_len, nonce, key);
	int ok = crypto_secretbox_open_easy(decrypted, ciphertext, msg_len + crypto_secretbox_MACBYTES, nonce, key) == 0
		&& memcmp(decrypted, msg, msg_len) == 0;
	report("round-trip decrypts to the original message", ok);

	ciphertext[0] ^= 0x01;
	int tampered_rejected = crypto_secretbox_open_easy(decrypted, ciphertext, msg_len + crypto_secretbox_MACBYTES, nonce, key) != 0;
	report("tampered ciphertext is rejected", tampered_rejected);
}

// crypto_box: public-key authenticated encryption. Two independent
// keypairs (Alice/Bob); Alice encrypts to Bob's public key, Bob decrypts
// with his secret key and Alice's public key.
static void test_box(void)
{
	printf("crypto_box (X25519-XSalsa20-Poly1305)\n");

	unsigned char alice_pk[crypto_box_PUBLICKEYBYTES], alice_sk[crypto_box_SECRETKEYBYTES];
	unsigned char bob_pk[crypto_box_PUBLICKEYBYTES], bob_sk[crypto_box_SECRETKEYBYTES];
	crypto_box_keypair(alice_pk, alice_sk);
	crypto_box_keypair(bob_pk, bob_sk);

	unsigned char nonce[crypto_box_NONCEBYTES];
	randombytes_buf(nonce, sizeof nonce);

	const char *msg = "meet at the usual coordinates";
	size_t msg_len = strlen(msg);
	unsigned char ciphertext[crypto_box_MACBYTES + 64];
	unsigned char decrypted[64];

	int ok = crypto_box_easy(ciphertext, (const unsigned char *)msg, msg_len, nonce, bob_pk, alice_sk) == 0
		&& crypto_box_open_easy(decrypted, ciphertext, msg_len + crypto_box_MACBYTES, nonce, alice_pk, bob_sk) == 0
		&& memcmp(decrypted, msg, msg_len) == 0;
	report("Bob decrypts what Alice encrypted to him", ok);

	unsigned char eve_pk[crypto_box_PUBLICKEYBYTES], eve_sk[crypto_box_SECRETKEYBYTES];
	crypto_box_keypair(eve_pk, eve_sk);
	int wrong_key_rejected = crypto_box_open_easy(decrypted, ciphertext, msg_len + crypto_box_MACBYTES, nonce, alice_pk, eve_sk) != 0;
	report("a third party's key cannot open it", wrong_key_rejected);
}

// crypto_sign: Ed25519 detached signatures -- verify a genuine signature,
// then confirm a corrupted one is rejected.
static void test_sign(void)
{
	printf("crypto_sign (Ed25519)\n");

	unsigned char pk[crypto_sign_PUBLICKEYBYTES], sk[crypto_sign_SECRETKEYBYTES];
	crypto_sign_keypair(pk, sk);

	const char *msg = "signed, sealed, delivered";
	size_t msg_len = strlen(msg);
	unsigned char sig[crypto_sign_BYTES];

	crypto_sign_detached(sig, NULL, (const unsigned char *)msg, msg_len, sk);
	int ok = crypto_sign_verify_detached(sig, (const unsigned char *)msg, msg_len, pk) == 0;
	report("genuine signature verifies", ok);

	sig[0] ^= 0x01;
	int corrupted_rejected = crypto_sign_verify_detached(sig, (const unsigned char *)msg, msg_len, pk) != 0;
	report("corrupted signature is rejected", corrupted_rejected);
}

// crypto_generichash (BLAKE2b) and crypto_hash_sha256: both are pure
// functions of their input, so their digests here are exact, checkable
// constants -- not just "did it run".
static void test_hashing(void)
{
	printf("crypto_generichash (BLAKE2b) / crypto_hash_sha256\n");

	const char *msg = "abc";
	size_t msg_len = strlen(msg);

	unsigned char blake2b[crypto_generichash_BYTES];
	crypto_generichash(blake2b, sizeof blake2b, (const unsigned char *)msg, msg_len, NULL, 0);
	print_hex("BLAKE2b-256(\"abc\")", blake2b, sizeof blake2b);

	unsigned char sha256[crypto_hash_sha256_BYTES];
	crypto_hash_sha256(sha256, (const unsigned char *)msg, msg_len);
	print_hex("SHA-256(\"abc\")   ", sha256, sizeof sha256);

	// SHA-256("abc") is a well-known test vector (FIPS 180-2) -- checking
	// it byte-for-byte catches a wrong implementation getting picked,
	// not just a crash.
	static const unsigned char expected_sha256[crypto_hash_sha256_BYTES] = {
		0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
		0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
		0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
		0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
	};
	report("SHA-256(\"abc\") matches the FIPS 180-2 test vector",
		memcmp(sha256, expected_sha256, sizeof sha256) == 0);
}

// crypto_pwhash (Argon2id): derives a symmetric key from a password and
// a random salt. Run at the library's minimum ops/mem limits (see this
// file's header) so it finishes quickly on this microVM's single core
// and modest RAM.
static void test_pwhash(void)
{
	printf("crypto_pwhash (Argon2id, OPSLIMIT_MIN/MEMLIMIT_MIN)\n");

	unsigned char salt[crypto_pwhash_SALTBYTES];
	randombytes_buf(salt, sizeof salt);

	const char *password = "hunter2";
	unsigned char key1[crypto_secretbox_KEYBYTES];
	unsigned char key2[crypto_secretbox_KEYBYTES];

	int rc1 = crypto_pwhash(key1, sizeof key1, password, strlen(password), salt,
		crypto_pwhash_OPSLIMIT_MIN, crypto_pwhash_MEMLIMIT_MIN, crypto_pwhash_ALG_ARGON2ID13);
	int rc2 = crypto_pwhash(key2, sizeof key2, password, strlen(password), salt,
		crypto_pwhash_OPSLIMIT_MIN, crypto_pwhash_MEMLIMIT_MIN, crypto_pwhash_ALG_ARGON2ID13);

	report("derivation succeeds", rc1 == 0 && rc2 == 0);
	report("same password + salt derives the same key", memcmp(key1, key2, sizeof key1) == 0);
}

int main(void)
{
	if (sodium_init() < 0) {
		printf("sodium_init() failed\n");
		return 1;
	}

	printf("BareMetal sodium -- libsodium %s\n\n", sodium_version_string());

	test_secretbox();
	printf("\n");
	test_box();
	printf("\n");
	test_sign();
	printf("\n");
	test_hashing();
	printf("\n");
	test_pwhash();
	printf("\n");

	if (failures == 0) {
		printf("all checks passed\n");
		return 0;
	}

	printf("%d check(s) FAILED\n", failures);
	return 1;
}
