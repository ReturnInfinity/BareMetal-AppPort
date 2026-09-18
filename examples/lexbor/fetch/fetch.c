// fetch.c -- the first real fetch -> parse pipeline: libcurl (see
// curltest.c at the repo root for the plain HTTP/HTTPS-only version
// this borrows its CA-bundle/write-callback handling from) GETs a URL
// into an in-memory buffer, then lexbor (see examples/lexbor/hello/
// hello.c for the standalone parser demo) parses that buffer into a
// real DOM and answers two small real queries against it: the
// document's <title> text, and how many <a> tags it contains (via a
// CSS selector, same lxb_selectors_find() pattern hello.c already
// used).
//
// The URL comes from argv[1] -- crt0.c's fc_parse_args_param() already
// turns `./baremetal.sh start "https://..."` 's kernel `args=` boot
// param into a real argv, so this app doesn't need to do anything
// special to accept one. FETCH_URL (the same https://example.com/
// curltest.c fetches) is only the fallback when no arg is given, kept
// as a known-good default with predictable output (one <title>,
// "Example Domain", and exactly one <a> tag) -- not a hardcoded target
// anymore.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>

#include <lexbor/html/html.h>
#include <lexbor/css/css.h>
#include <lexbor/selectors/selectors.h>

#define FETCH_URL      "https://example.com/"
#define RESPONSE_BUF_INITIAL_CAP (16 * 1024)
#define CA_BUNDLE_PATH "/etc/ssl/cacert.pem"

// See curltest.c's matching declaration for why this is `weak` with a
// real (empty) definition rather than a bare `extern`.
__attribute__((weak)) const unsigned char cacert_pem[1];
__attribute__((weak)) const unsigned int cacert_pem_len;

// Growable, not a fixed RESPONSE_BUF_SIZE cap -- the original fixed
// 32KiB buffer silently truncated anything bigger (found for real
// against https://www.wikipedia.org/'s 119KB response, see BROWSER.md).
// Doubles on demand starting from RESPONSE_BUF_INITIAL_CAP; freed once
// lexbor is done parsing it.
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

// Returning anything other than `n` tells libcurl the write failed and
// aborts the transfer (CURLE_WRITE_ERROR) -- the standard growable-
// buffer write-callback contract. Unlike the old fixed-buffer version,
// a real allocation failure now surfaces as a real error instead of
// silently handing lexbor a truncated document.
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

static lxb_status_t count_a_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t spec, void *ctx)
{
	(void)node;
	(void)spec;
	unsigned *count = ctx;
	(*count)++;
	return LXB_STATUS_OK;
}

int main(int argc, char **argv)
{
	const char *url = argc > 1 ? argv[1] : FETCH_URL;

	printf("BareMetal fetch+parse -- libcurl %s + lexbor\n", curl_version());
	printf("GET %s\n\n", url);

	curl_global_init(CURL_GLOBAL_DEFAULT);

	CURL *h = curl_easy_init();
	if (!h) {
		printf("curl_easy_init() failed\n");
		curl_global_cleanup();
		return 1;
	}

	struct growable_buf resp = { 0 };

	curl_easy_setopt(h, CURLOPT_URL, url);
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, &resp);
	curl_easy_setopt(h, CURLOPT_USERAGENT, "BareMetal-fetch/1.0");
	curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
	set_ca_bundle(h);
	curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L);

	CURLcode res = curl_easy_perform(h);
	if (res != CURLE_OK) {
		printf("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
		curl_easy_cleanup(h);
		curl_global_cleanup();
		free(resp.data);
		return 1;
	}

	long status = 0;
	curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
	printf("status: %ld\n", status);
	printf("body: %zu byte(s)\n\n", resp.len);

	curl_easy_cleanup(h);
	curl_global_cleanup();

	// --- parse phase: hand the fetched bytes straight to lexbor ---

	lxb_html_document_t *document = lxb_html_document_create();
	lxb_status_t lstatus = lxb_html_document_parse(document,
		(const lxb_char_t *)resp.data, resp.len);
	if (lstatus != LXB_STATUS_OK) {
		printf("lxb_html_document_parse failed: %d\n", lstatus);
		free(resp.data);
		return 1;
	}

	size_t title_len = 0;
	const lxb_char_t *title = lxb_html_document_title(document, &title_len);
	printf("title: %.*s\n", (int)title_len, title ? (const char *)title : "");

	lxb_css_parser_t *parser = lxb_css_parser_create();
	lstatus = lxb_css_parser_init(parser, NULL);
	if (lstatus != LXB_STATUS_OK) {
		printf("lxb_css_parser_init failed: %d\n", lstatus);
		return 1;
	}

	lxb_selectors_t *selectors = lxb_selectors_create();
	lstatus = lxb_selectors_init(selectors);
	if (lstatus != LXB_STATUS_OK) {
		printf("lxb_selectors_init failed: %d\n", lstatus);
		return 1;
	}

	static const lxb_char_t query[] = "a";
	lxb_css_selector_list_t *list = lxb_css_selectors_parse(parser, query, sizeof(query) - 1);
	if (parser->status != LXB_STATUS_OK) {
		printf("lxb_css_selectors_parse failed: %d\n", parser->status);
		return 1;
	}

	unsigned a_count = 0;
	lxb_dom_node_t *root = lxb_dom_interface_node(document);
	lstatus = lxb_selectors_find(selectors, root, list, count_a_cb, &a_count);
	if (lstatus != LXB_STATUS_OK) {
		printf("lxb_selectors_find failed: %d\n", lstatus);
		return 1;
	}

	printf("<a> tag count: %u\n", a_count);

	lxb_selectors_destroy(selectors, true);
	lxb_css_parser_destroy(parser, true);
	lxb_css_selector_list_destroy_memory(list);
	lxb_html_document_destroy(document);
	free(resp.data);

	return 0;
}
