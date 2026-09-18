// fetch.c -- the first real fetch -> parse pipeline: libcurl (see
// curltest.c at the repo root for the plain HTTP/HTTPS-only version
// this borrows its CA-bundle/write-callback handling from) GETs
// FETCH_URL into an in-memory buffer, then lexbor (see
// examples/lexbor/hello/hello.c for the standalone parser demo) parses
// that buffer into a real DOM and answers two small real queries
// against it: the document's <title> text, and how many <a> tags it
// contains (via a CSS selector, same lxb_selectors_find() pattern
// hello.c already used).
//
// FETCH_URL is the same https://example.com/ curltest.c already
// fetches, on purpose -- a small, stable, dependency-free page is more
// useful here as a known-good target than picking a new one, and it
// keeps this example's expected output predictable: one <title>
// ("Example Domain") and exactly one <a> tag.

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>

#include <lexbor/html/html.h>
#include <lexbor/css/css.h>
#include <lexbor/selectors/selectors.h>

#define FETCH_URL      "https://example.com/"
#define RESPONSE_BUF_SIZE (32 * 1024)
#define CA_BUNDLE_PATH "/etc/ssl/cacert.pem"

// See curltest.c's matching declaration for why this is `weak` with a
// real (empty) definition rather than a bare `extern`.
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

// Same overflow contract as curltest.c's write_cb(): always report
// every byte "written" (never abort the transfer), silently dropping
// whatever doesn't fit past RESPONSE_BUF_SIZE.
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

static lxb_status_t count_a_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t spec, void *ctx)
{
	(void)node;
	(void)spec;
	unsigned *count = ctx;
	(*count)++;
	return LXB_STATUS_OK;
}

int main(void)
{
	printf("BareMetal fetch+parse -- libcurl %s + lexbor\n", curl_version());
	printf("GET %s\n\n", FETCH_URL);

	curl_global_init(CURL_GLOBAL_DEFAULT);

	CURL *h = curl_easy_init();
	if (!h) {
		printf("curl_easy_init() failed\n");
		curl_global_cleanup();
		return 1;
	}

	curl_easy_setopt(h, CURLOPT_URL, FETCH_URL);
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
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
		return 1;
	}

	long status = 0;
	curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
	printf("status: %ld\n", status);
	printf("body: %zu byte(s) kept (RESPONSE_BUF_SIZE cap)\n\n", response_len);

	curl_easy_cleanup(h);
	curl_global_cleanup();

	// --- parse phase: hand the fetched bytes straight to lexbor ---

	lxb_html_document_t *document = lxb_html_document_create();
	lxb_status_t lstatus = lxb_html_document_parse(document,
		(const lxb_char_t *)response_buf, response_len);
	if (lstatus != LXB_STATUS_OK) {
		printf("lxb_html_document_parse failed: %d\n", lstatus);
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

	return 0;
}
