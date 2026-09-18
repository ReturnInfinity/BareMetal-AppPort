// browser_fetch.c -- the stretch goal BROWSER.md flagged as not yet
// attempted: combine a *live* network fetch (examples/lexbor/fetch/
// fetch.c's curl+lexbor pipeline) with the DOM<->QuickJS binding layer
// (examples/lexbor/browser/browser.c's console.log/document.
// querySelector()/inline-<script> execution) and run whatever inline
// <script> tags a real fetched page actually has.
//
// Neither fetch.c nor browser.c is modified -- both stay exactly as
// they were (hermetic-or-argv-driven, independently verified). This
// file is new glue combining the two, not a rewrite of either.
//
// The URL comes from argv[1], same mechanism fetch.c just gained
// (crt0.c's fc_parse_args_param() turning a `./baremetal.sh start
// "https://..."` kernel args= boot param into a real argv). Falls back
// to FETCH_URL (https://example.com/) when no arg is given -- chosen
// as the default because it's the same known-good target every other
// example already uses, not because it has any inline <script> (it
// doesn't -- see the "what actually happened" notes in BROWSER.md's
// new section for real URLs that do).
//
// The fetched HTTP body is length-delimited, not a null-terminated C
// string (same finding fetch.c's own header documents) -- passed into
// lxb_html_document_parse() with an explicit length throughout, never
// treated as a C string.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>

#include "quickjs.h"

#include <lexbor/html/html.h>
#include <lexbor/css/css.h>
#include <lexbor/selectors/selectors.h>
#include <lexbor/dom/interfaces/element.h>

#define FETCH_URL      "https://example.com/"
#define RESPONSE_BUF_INITIAL_CAP (16 * 1024)
#define CA_BUNDLE_PATH "/etc/ssl/cacert.pem"

// See curltest.c's matching declaration for why this is `weak` with a
// real (empty) definition rather than a bare `extern`.
__attribute__((weak)) const unsigned char cacert_pem[1];
__attribute__((weak)) const unsigned int cacert_pem_len;

// Growable, not a fixed RESPONSE_BUF_SIZE cap -- see fetch.c's matching
// comment: the old fixed 32KiB buffer silently truncated anything
// bigger, found for real against https://www.wikipedia.org/'s 119KB
// response (see BROWSER.md's "what actually happened" notes).
struct growable_buf {
	char *data;
	size_t len;
	size_t cap;
};

// The DOM root every document.querySelector() call searches -- same
// file-scope-global, single-document design browser.c uses.
static lxb_html_document_t *g_document;

static JSClassID element_class_id;

static void set_ca_bundle(CURL *h)
{
	if (access(CA_BUNDLE_PATH, R_OK) == 0) {
		curl_easy_setopt(h, CURLOPT_CAINFO, CA_BUNDLE_PATH);
	} else {
		struct curl_blob blob = { (void *)cacert_pem, cacert_pem_len, CURL_BLOB_NOCOPY };
		curl_easy_setopt(h, CURLOPT_CAINFO_BLOB, &blob);
	}
}

// Same contract as fetch.c's write_cb(): returning anything other than
// `n` tells libcurl the write failed and aborts the transfer, the
// standard growable-buffer write-callback idiom.
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

// --- DOM<->QuickJS binding layer, identical to browser.c's ---

static void element_finalizer(JSRuntime *rt, JSValueConst val)
{
	(void)rt;
	(void)val;
}

static JSClassDef element_class_def = {
	"Element",
	.finalizer = element_finalizer,
};

static JSValue make_element(JSContext *ctx, lxb_dom_node_t *node)
{
	JSValue obj = JS_NewObjectClass(ctx, element_class_id);
	if (JS_IsException(obj))
		return obj;
	JS_SetOpaque(obj, node);
	return obj;
}

static JSValue element_get_textContent(JSContext *ctx, JSValueConst this_val)
{
	lxb_dom_node_t *node = JS_GetOpaque(this_val, element_class_id);
	if (!node)
		return JS_UNDEFINED;

	size_t len = 0;
	lxb_char_t *text = lxb_dom_node_text_content(node, &len);
	if (!text)
		return JS_NewStringLen(ctx, "", 0);

	return JS_NewStringLen(ctx, (const char *)text, len);
}

static JSValue element_get_tagName(JSContext *ctx, JSValueConst this_val)
{
	lxb_dom_node_t *node = JS_GetOpaque(this_val, element_class_id);
	if (!node)
		return JS_UNDEFINED;

	size_t len = 0;
	const lxb_char_t *name = lxb_dom_element_qualified_name_upper(
		lxb_dom_interface_element(node), &len);
	if (!name)
		return JS_UNDEFINED;

	return JS_NewStringLen(ctx, (const char *)name, len);
}

static JSValue element_getAttribute(JSContext *ctx, JSValueConst this_val,
				     int argc, JSValueConst *argv)
{
	lxb_dom_node_t *node = JS_GetOpaque(this_val, element_class_id);
	if (!node || argc < 1)
		return JS_NULL;

	const char *name = JS_ToCString(ctx, argv[0]);
	if (!name)
		return JS_EXCEPTION;

	size_t len = 0;
	const lxb_char_t *value = lxb_dom_element_get_attribute(
		lxb_dom_interface_element(node),
		(const lxb_char_t *)name, strlen(name), &len);
	JS_FreeCString(ctx, name);

	if (!value)
		return JS_NULL;

	return JS_NewStringLen(ctx, (const char *)value, len);
}

static const JSCFunctionListEntry element_proto_funcs[] = {
	JS_CGETSET_DEF("textContent", element_get_textContent, NULL),
	JS_CGETSET_DEF("tagName", element_get_tagName, NULL),
	JS_CFUNC_DEF("getAttribute", 1, element_getAttribute),
};

static void register_element_class(JSRuntime *rt, JSContext *ctx)
{
	JS_NewClassID(rt, &element_class_id);
	JS_NewClass(rt, element_class_id, &element_class_def);

	JSValue proto = JS_NewObject(ctx);
	JS_SetPropertyFunctionList(ctx, proto, element_proto_funcs,
				    sizeof(element_proto_funcs) / sizeof(element_proto_funcs[0]));
	JS_SetClassProto(ctx, element_class_id, proto);
}

struct qs_result {
	lxb_dom_node_t *found;
};

static lxb_status_t qs_find_cb(lxb_dom_node_t *node,
				lxb_css_selector_specificity_t spec, void *ctx)
{
	(void)spec;
	struct qs_result *r = ctx;
	if (!r->found)
		r->found = node;
	return LXB_STATUS_OK;
}

static JSValue document_querySelector(JSContext *ctx, JSValueConst this_val,
				       int argc, JSValueConst *argv)
{
	(void)this_val;
	if (argc < 1)
		return JS_NULL;

	const char *sel = JS_ToCString(ctx, argv[0]);
	if (!sel)
		return JS_EXCEPTION;

	lxb_css_parser_t *parser = lxb_css_parser_create();
	lxb_status_t status = lxb_css_parser_init(parser, NULL);
	if (status != LXB_STATUS_OK) {
		JS_FreeCString(ctx, sel);
		lxb_css_parser_destroy(parser, true);
		return JS_ThrowInternalError(ctx, "lxb_css_parser_init failed");
	}

	lxb_css_selector_list_t *list =
		lxb_css_selectors_parse(parser, (const lxb_char_t *)sel, strlen(sel));
	JS_FreeCString(ctx, sel);
	if (parser->status != LXB_STATUS_OK) {
		lxb_css_parser_destroy(parser, true);
		return JS_ThrowTypeError(ctx, "invalid selector");
	}

	lxb_selectors_t *selectors = lxb_selectors_create();
	status = lxb_selectors_init(selectors);
	if (status != LXB_STATUS_OK) {
		lxb_selectors_destroy(selectors, true);
		lxb_css_parser_destroy(parser, true);
		lxb_css_selector_list_destroy_memory(list);
		return JS_ThrowInternalError(ctx, "lxb_selectors_init failed");
	}

	struct qs_result result = { .found = NULL };
	lxb_dom_node_t *root = lxb_dom_interface_node(g_document);
	status = lxb_selectors_find(selectors, root, list, qs_find_cb, &result);

	lxb_selectors_destroy(selectors, true);
	lxb_css_parser_destroy(parser, true);
	lxb_css_selector_list_destroy_memory(list);

	if (status != LXB_STATUS_OK || !result.found)
		return JS_NULL;

	return make_element(ctx, result.found);
}

static JSValue js_console_log(JSContext *ctx, JSValueConst this_val,
			       int argc, JSValueConst *argv)
{
	(void)this_val;
	for (int i = 0; i < argc; i++) {
		if (i)
			printf(" ");
		const char *s = JS_ToCString(ctx, argv[i]);
		if (s) {
			printf("%s", s);
			JS_FreeCString(ctx, s);
		}
	}
	printf("\n");
	return JS_UNDEFINED;
}

static void setup_globals(JSContext *ctx)
{
	JSValue global = JS_GetGlobalObject(ctx);

	JSValue console = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, console, "log",
			   JS_NewCFunction(ctx, js_console_log, "log", 0));
	JS_SetPropertyStr(ctx, global, "console", console);

	JSValue document = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, document, "querySelector",
			   JS_NewCFunction(ctx, document_querySelector, "querySelector", 1));
	JS_SetPropertyStr(ctx, global, "document", document);

	JS_FreeValue(ctx, global);
}

static lxb_status_t script_find_cb(lxb_dom_node_t *node,
				    lxb_css_selector_specificity_t spec, void *ctx_ptr)
{
	(void)spec;
	JSContext *ctx = ctx_ptr;
	lxb_dom_element_t *el = lxb_dom_interface_element(node);

	if (lxb_dom_element_has_attribute(el, (const lxb_char_t *)"src", 3)) {
		printf("(skipping <script src=...>, external scripts not fetched)\n");
		return LXB_STATUS_OK;
	}

	size_t len = 0;
	lxb_char_t *text = lxb_dom_node_text_content(node, &len);
	if (!text || len == 0)
		return LXB_STATUS_OK;

	JSValue result = JS_Eval(ctx, (const char *)text, len, "<script>", JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(result)) {
		JSValue exc = JS_GetException(ctx);
		const char *msg = JS_ToCString(ctx, exc);
		printf("Uncaught exception: %s\n", msg ? msg : "(no message)");
		JS_FreeCString(ctx, msg);
		JS_FreeValue(ctx, exc);
	}
	JS_FreeValue(ctx, result);

	return LXB_STATUS_OK;
}

static void run_scripts(JSContext *ctx)
{
	lxb_css_parser_t *parser = lxb_css_parser_create();
	lxb_css_parser_init(parser, NULL);

	lxb_selectors_t *selectors = lxb_selectors_create();
	lxb_selectors_init(selectors);

	static const lxb_char_t query[] = "script";
	lxb_css_selector_list_t *list =
		lxb_css_selectors_parse(parser, query, sizeof(query) - 1);

	lxb_dom_node_t *root = lxb_dom_interface_node(g_document);
	lxb_selectors_find(selectors, root, list, script_find_cb, ctx);

	lxb_selectors_destroy(selectors, true);
	lxb_css_parser_destroy(parser, true);
	lxb_css_selector_list_destroy_memory(list);
}

int main(int argc, char **argv)
{
	const char *url = argc > 1 ? argv[1] : FETCH_URL;

	printf("BareMetal browser -- libcurl %s + lexbor + QuickJS\n", curl_version());
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
	curl_easy_setopt(h, CURLOPT_USERAGENT, "BareMetal-browser/1.0");
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
	// resp.data/resp.len is length-delimited, not a C string -- passed
	// through with its explicit length, never strlen()'d.

	g_document = lxb_html_document_create();
	lxb_status_t lstatus = lxb_html_document_parse(g_document,
		(const lxb_char_t *)resp.data, resp.len);
	if (lstatus != LXB_STATUS_OK) {
		printf("lxb_html_document_parse failed: %d\n", lstatus);
		free(resp.data);
		return 1;
	}

	size_t title_len = 0;
	const lxb_char_t *title = lxb_html_document_title(g_document, &title_len);
	printf("title: %.*s\n\n", (int)title_len, title ? (const char *)title : "");

	// --- run phase: bind console/document, run every inline <script> ---

	JSRuntime *rt = JS_NewRuntime();
	JSContext *ctx = JS_NewContext(rt);

	register_element_class(rt, ctx);
	setup_globals(ctx);
	run_scripts(ctx);

	JS_FreeContext(ctx);
	JS_FreeRuntime(rt);
	lxb_html_document_destroy(g_document);
	free(resp.data);

	return 0;
}
