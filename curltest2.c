// curltest2.c -- minimal repro for a GP-fault crash found in
// examples/lexbor/browser-fetch/browser_fetch.c: two sequential real
// curl_easy_init()/perform()/cleanup() fetches in one process. No
// lexbor, no QuickJS -- isolates whether the crash is networking-only
// (curl/mbedTLS/lwIP/kernel) or requires the full DOM+JS stack to be
// alive too. See BROWSER.md's "External script fetching" section for
// the original symptom.

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>

#define RESPONSE_BUF_SIZE (32 * 1024)
#define CA_BUNDLE_PATH "/etc/ssl/cacert.pem"

__attribute__((weak)) const unsigned char cacert_pem[1];
__attribute__((weak)) const unsigned int cacert_pem_len;

static char response_buf[RESPONSE_BUF_SIZE];
static size_t response_len;

static void set_ca_bundle(CURL *h)
{
	if (access(CA_BUNDLE_PATH, R_OK) == 0) {
		curl_easy_setopt(h, CURLOPT_CAINFO, CA_BUNDLE_PATH);
	} else {
		struct curl_blob blob = { (void *)cacert_pem, cacert_pem_len, CURL_BLOB_NOCOPY };
		curl_easy_setopt(h, CURLOPT_CAINFO_BLOB, &blob);
	}
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	(void)userdata;
	size_t n = size * nmemb;

	size_t room = sizeof(response_buf) - 1 - response_len;
	size_t copy = n < room ? n : room;
	memcpy(response_buf + response_len, ptr, copy);
	response_len += copy;

	return n;
}

static int fetch_once(const char *url)
{
	response_len = 0;

	printf("GET %s\n", url);

	CURL *h = curl_easy_init();
	if (!h) {
		printf("curl_easy_init() failed\n");
		return 1;
	}

	curl_easy_setopt(h, CURLOPT_URL, url);
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(h, CURLOPT_USERAGENT, "BareMetal-curltest2/1.0");
	curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
	set_ca_bundle(h);
	curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L);

	CURLcode res = curl_easy_perform(h);
	if (res != CURLE_OK) {
		printf("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
		curl_easy_cleanup(h);
		return 1;
	}

	long status = 0;
	curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
	printf("status: %ld, body: %zu byte(s)\n\n", status, response_len);

	curl_easy_cleanup(h);
	return 0;
}

int main(void)
{
	printf("BareMetal curltest2 -- libcurl %s (two sequential fetches, no lexbor/QuickJS)\n\n",
		curl_version());

	curl_global_init(CURL_GLOBAL_DEFAULT);

	int rc1 = fetch_once("https://example.com/");
	printf("--- first fetch done, doing second fetch ---\n\n");
	int rc2 = fetch_once("https://www.iana.org/domains/reserved");

	curl_global_cleanup();

	printf("done: rc1=%d rc2=%d\n", rc1, rc2);

	return (rc1 || rc2) ? 1 : 0;
}
