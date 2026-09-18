// yamltest3.c -- third isolation step for the parked GC-assert crash
// (see yamltest.c/yamltest2.c and BROWSER.md). Neither a single eval
// of embedded js-yaml source (yamltest.c) nor sequential multi-eval on
// a shared context (yamltest2.c) reproduced the leak in 21+21 clean
// boots. This tries the other named candidate difference: the exact
// same js-yaml source, but loaded via a real curl fetch into a
// growable buffer (same struct/write_cb pattern as
// examples/lexbor/fetch/fetch.c) instead of embedded as a string
// constant -- still zero lexbor/DOM involved.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>

#include "quickjs.h"

#define FETCH_URL "https://cdnjs.cloudflare.com/ajax/libs/js-yaml/4.1.0/js-yaml.min.js"
#define RESPONSE_BUF_INITIAL_CAP (16 * 1024)
#define CA_BUNDLE_PATH "/etc/ssl/cacert.pem"

__attribute__((weak)) const unsigned char cacert_pem[1];
__attribute__((weak)) const unsigned int cacert_pem_len;

struct growable_buf {
	char *data;
	size_t len;
	size_t cap;
};

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
	struct growable_buf *buf = userdata;
	size_t n = size * nmemb;

	if (buf->len + n + 1 > buf->cap) {
		size_t new_cap = buf->cap ? buf->cap * 2 : RESPONSE_BUF_INITIAL_CAP;
		while (new_cap < buf->len + n + 1)
			new_cap *= 2;
		char *new_data = realloc(buf->data, new_cap);
		if (!new_data)
			return 0;
		buf->data = new_data;
		buf->cap = new_cap;
	}

	memcpy(buf->data + buf->len, ptr, n);
	buf->len += n;

	return n;
}

int main(void)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
	CURL *h = curl_easy_init();
	struct growable_buf resp = { 0 };

	curl_easy_setopt(h, CURLOPT_URL, FETCH_URL);
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, &resp);
	curl_easy_setopt(h, CURLOPT_USERAGENT, "BareMetal-yamltest3/1.0");
	curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
	set_ca_bundle(h);
	curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L);

	CURLcode res = curl_easy_perform(h);
	long status = 0;
	curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
	printf("GET %s -> status %ld, %zu byte(s), curl result %d\n",
	       FETCH_URL, status, resp.len, res);
	curl_easy_cleanup(h);
	curl_global_cleanup();

	if (res != CURLE_OK || resp.len == 0) {
		printf("fetch failed, aborting before JS_Eval\n");
		free(resp.data);
		return 1;
	}

	JSRuntime *rt = JS_NewRuntime();
	JS_SetDumpFlags(rt, JS_DUMP_LEAKS);
	JSContext *ctx = JS_NewContext(rt);

	JSValue result = JS_Eval(ctx, resp.data, resp.len, "<js-yaml-fetched>", JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(result)) {
		JSValue exc = JS_GetException(ctx);
		const char *msg = JS_ToCString(ctx, exc);
		printf("js-yaml load exception: %s\n", msg);
		JS_FreeCString(ctx, msg);
		JS_FreeValue(ctx, exc);
	} else {
		printf("js-yaml loaded without exception\n");
	}
	JS_FreeValue(ctx, result);

	JS_FreeContext(ctx);
	printf("about to JS_FreeRuntime (this is where the leak check runs)\n");
	JS_FreeRuntime(rt);
	printf("JS_FreeRuntime completed cleanly\n");

	free(resp.data);
	return 0;
}
