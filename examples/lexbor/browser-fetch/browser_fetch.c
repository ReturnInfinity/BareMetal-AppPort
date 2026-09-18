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

#include <stdbool.h>
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
#include <lexbor/url/url.h>

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

// The page's own URL (after redirects -- see fetch_url()'s
// CURLINFO_EFFECTIVE_URL comment below), parsed once and reused as the
// base every <script src="..."> is resolved against, exactly the way a
// real browser resolves a page's relative URLs against its final
// navigated location, not the URL originally requested. g_url_parser
// is reused (lxb_url_parser_clean() between calls, matching lexbor's
// own examples/lexbor/url/relative.c) rather than re-created per
// script -- one parser, many lxb_url_parse() calls.
static lxb_url_parser_t g_url_parser;
static lxb_url_t *g_base_url;

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

// Common curl setup/teardown for both the main page fetch and each
// external <script src> fetch -- factored out because this file now
// has two real, immediate callers of the exact same ~10-line
// CURLOPT_* block, not because it might be reused someday.
// effective_url_out/effective_url_cap are only used by the page fetch
// in main() (to get the post-redirect URL <script src> resolution
// needs as its base) -- pass NULL/0 for a plain fetch. The string
// CURLINFO_EFFECTIVE_URL points at belongs to the handle and is only
// valid until curl_easy_cleanup(), hence copying it out before that.
static CURLcode fetch_url(const char *url, struct growable_buf *out,
			   long *status_out, char *effective_url_out,
			   size_t effective_url_cap)
{
	CURL *h = curl_easy_init();
	if (!h)
		return CURLE_FAILED_INIT;

	curl_easy_setopt(h, CURLOPT_URL, url);
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, out);
	curl_easy_setopt(h, CURLOPT_USERAGENT, "BareMetal-browser/1.0");
	curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
	set_ca_bundle(h);
	curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L);

	CURLcode res = curl_easy_perform(h);

	if (status_out)
		curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, status_out);

	if (effective_url_out && effective_url_cap > 0) {
		char *eff = NULL;
		curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &eff);
		if (eff) {
			strncpy(effective_url_out, eff, effective_url_cap - 1);
			effective_url_out[effective_url_cap - 1] = '\0';
		} else {
			effective_url_out[0] = '\0';
		}
	}

	curl_easy_cleanup(h);
	return res;
}

// Bounded accumulator for lxb_url_serialize()'s multi-call callback
// (see lexbor's own examples/lexbor/url/parse.c -- it may call back
// once per URL component). A fixed cap is fine here, unlike the fetch
// response buffer: URLs have a real, sane length bound in practice
// (every real browser enforces one), unlike arbitrary page content.
struct url_buf {
	char *data;
	size_t len;
	size_t cap;
};

static lxb_status_t url_serialize_cb(const lxb_char_t *data, size_t len, void *ctx)
{
	struct url_buf *ub = ctx;

	if (ub->len + 1 >= ub->cap)
		return LXB_STATUS_OK;

	size_t room = ub->cap - 1 - ub->len;
	size_t copy = len < room ? len : room;
	memcpy(ub->data + ub->len, data, copy);
	ub->len += copy;

	return LXB_STATUS_OK;
}

// Resolves `src` (absolute or relative -- lxb_url_parse() detects
// which by whether it has its own scheme, same WHATWG algorithm every
// real browser uses) against g_base_url, and serializes the result
// into `out`. Returns false if g_base_url is unset (should not happen
// in practice -- it's always parsed from a URL curl just used
// successfully) or the src itself doesn't parse as a URL at all.
static bool resolve_script_url(const char *src, size_t src_len, char *out, size_t out_cap)
{
	if (!g_base_url)
		return false;

	lxb_url_parser_clean(&g_url_parser);
	lxb_url_t *resolved = lxb_url_parse(&g_url_parser, g_base_url,
		(const lxb_char_t *)src, src_len);
	if (!resolved)
		return false;

	struct url_buf ub = { out, 0, out_cap };
	lxb_url_serialize(resolved, url_serialize_cb, &ub, false);
	out[ub.len] = '\0';

	// NOT lxb_url_memory_destroy() -- that calls lexbor_mraw_destroy(),
	// tearing down the ENTIRE mraw arena (every chunk, and the mraw
	// struct itself), not just this one URL's own allocation. g_url_parser
	// (and g_base_url, allocated from the same arena) is reused across
	// every <script src> on the page, so destroying the whole arena after
	// the FIRST resolution leaves g_url_parser.mraw a dangling pointer --
	// exactly the documented gotcha in url.h's own comment on
	// lxb_url_memory_destroy(): "if you have a live lxb_url_parser_t
	// parsing object, you will have a pointer to garbage after calling
	// this function". The SECOND call to resolve_script_url() then reads
	// that garbage pointer inside lxb_url_parse() -> lexbor_mraw_alloc(),
	// producing a deterministic GP fault on exactly the second external
	// script on any page, regardless of its content or size -- root-
	// caused via a minimal repro (urltest2.c, no curl/QuickJS at all)
	// that reproduced the identical crash from pure lexbor `url`-module
	// reuse alone. lxb_url_destroy() is the correct one-URL-at-a-time
	// equivalent -- lexbor_mraw_free(), returning just this object's own
	// memory to the arena's free list, leaving the arena itself intact
	// for the next resolve_script_url() call.
	lxb_url_destroy(resolved);
	return true;
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

// Real DOM Element.className is a live view of the `class` attribute,
// but unlike getAttribute("class") it returns "" (not null) when the
// attribute is absent -- a page doing `el.className.split(' ')` would
// otherwise crash on a perfectly normal, class-less element.
static JSValue element_get_className(JSContext *ctx, JSValueConst this_val)
{
	lxb_dom_node_t *node = JS_GetOpaque(this_val, element_class_id);
	if (!node)
		return JS_UNDEFINED;

	size_t len = 0;
	const lxb_char_t *value = lxb_dom_element_get_attribute(
		lxb_dom_interface_element(node),
		(const lxb_char_t *)"class", 5, &len);
	if (!value)
		return JS_NewStringLen(ctx, "", 0);

	return JS_NewStringLen(ctx, (const char *)value, len);
}

static const JSCFunctionListEntry element_proto_funcs[] = {
	JS_CGETSET_DEF("textContent", element_get_textContent, NULL),
	JS_CGETSET_DEF("tagName", element_get_tagName, NULL),
	JS_CGETSET_DEF("className", element_get_className, NULL),
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

// document.documentElement/.body/.head -- direct lexbor accessors, not
// a CSS-selector query like querySelector() uses: lexbor already tracks
// these three as plain struct fields/inline accessors
// (lxb_dom_document_element()/lxb_html_document_body_element()/
// lxb_html_document_head_element()), so there's no need to run
// "html"/"body"/"head" through the selector engine. Each can
// legitimately be NULL for a malformed/incomplete document -- resolves
// to JS null in that case, same convention querySelector() uses for
// "no match".
static JSValue document_get_documentElement(JSContext *ctx, JSValueConst this_val)
{
	(void)ctx;
	(void)this_val;
	lxb_dom_element_t *el = lxb_dom_document_element(&g_document->dom_document);
	if (!el)
		return JS_NULL;
	return make_element(ctx, lxb_dom_interface_node(el));
}

static JSValue document_get_body(JSContext *ctx, JSValueConst this_val)
{
	(void)ctx;
	(void)this_val;
	lxb_html_body_element_t *el = lxb_html_document_body_element(g_document);
	if (!el)
		return JS_NULL;
	return make_element(ctx, lxb_dom_interface_node(el));
}

static JSValue document_get_head(JSContext *ctx, JSValueConst this_val)
{
	(void)ctx;
	(void)this_val;
	lxb_html_head_element_t *el = lxb_html_document_head_element(g_document);
	if (!el)
		return JS_NULL;
	return make_element(ctx, lxb_dom_interface_node(el));
}

static const JSCFunctionListEntry document_props[] = {
	JS_CFUNC_DEF("querySelector", 1, document_querySelector),
	JS_CGETSET_DEF("documentElement", document_get_documentElement, NULL),
	JS_CGETSET_DEF("body", document_get_body, NULL),
	JS_CGETSET_DEF("head", document_get_head, NULL),
};

// Real pages very commonly register listeners during initial script
// execution (window.addEventListener('DOMContentLoaded', ...) is
// close to universal). These are honest no-ops, not a faked event
// system: there is genuinely no DOMContentLoaded/load/click event ever
// fired here (see BROWSER.md's non-goals -- no event loop at all), so
// a registered listener is accepted and silently never called, rather
// than the call itself throwing TypeError: not a function just because
// the method didn't exist.
static JSValue window_addEventListener(JSContext *ctx, JSValueConst this_val,
					int argc, JSValueConst *argv)
{
	(void)ctx;
	(void)this_val;
	(void)argc;
	(void)argv;
	return JS_UNDEFINED;
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
	JS_SetPropertyFunctionList(ctx, document, document_props,
				    sizeof(document_props) / sizeof(document_props[0]));
	JS_SetPropertyStr(ctx, global, "document", document);

	// window is an alias for the global object, same as browser.c --
	// see its matching comment for why (window === globalThis in a real
	// browser, not a separate object).
	JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));

	// addEventListener/removeEventListener -- see browser.c's matching
	// comment: honest no-op stubs, not a faked event system.
	JS_SetPropertyStr(ctx, global, "addEventListener",
			   JS_NewCFunction(ctx, window_addEventListener, "addEventListener", 2));
	JS_SetPropertyStr(ctx, global, "removeEventListener",
			   JS_NewCFunction(ctx, window_addEventListener, "removeEventListener", 2));

	JS_FreeValue(ctx, global);
}

// Runs already-fetched script source against ctx, reporting exceptions
// the same way inline scripts do (script_find_cb below) -- `name` is
// the resolved URL for external scripts (so an exception's stack trace
// names the real source) or "<script>" for inline ones.
static void eval_script(JSContext *ctx, const char *src, size_t len, const char *name)
{
	JSValue result = JS_Eval(ctx, src, len, name, JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(result)) {
		JSValue exc = JS_GetException(ctx);
		const char *msg = JS_ToCString(ctx, exc);
		printf("Uncaught exception (%s): %s\n", name, msg ? msg : "(no message)");
		JS_FreeCString(ctx, msg);
		JS_FreeValue(ctx, exc);
	}
	JS_FreeValue(ctx, result);
}

#define SCRIPT_URL_BUF_SIZE 2048

// Fetches and runs one <script src="...">. A network/HTTP failure or
// an unresolvable URL is reported and skipped, same "don't abort the
// rest of the page" contract a throwing inline script already has --
// this is not a special case, it's the same policy applied one layer
// earlier (before the script even gets to run, instead of while it
// runs).
static void run_external_script(JSContext *ctx, const char *src, size_t src_len)
{
	char resolved[SCRIPT_URL_BUF_SIZE];
	if (!resolve_script_url(src, src_len, resolved, sizeof(resolved))) {
		printf("(could not resolve <script src=\"%.*s\">, skipping)\n",
		       (int)src_len, src);
		return;
	}

	struct growable_buf buf = { 0 };
	long status = 0;
	CURLcode res = fetch_url(resolved, &buf, &status, NULL, 0);

	if (res != CURLE_OK) {
		printf("(failed to fetch <script src=\"%s\">: %s)\n",
		       resolved, curl_easy_strerror(res));
		free(buf.data);
		return;
	}
	if (status < 200 || status >= 300) {
		printf("(failed to fetch <script src=\"%s\">: HTTP %ld)\n",
		       resolved, status);
		free(buf.data);
		return;
	}

	eval_script(ctx, buf.data, buf.len, resolved);
	free(buf.data);
}

static lxb_status_t script_find_cb(lxb_dom_node_t *node,
				    lxb_css_selector_specificity_t spec, void *ctx_ptr)
{
	(void)spec;
	JSContext *ctx = ctx_ptr;
	lxb_dom_element_t *el = lxb_dom_interface_element(node);

	size_t src_len = 0;
	const lxb_char_t *src = lxb_dom_element_get_attribute(el,
		(const lxb_char_t *)"src", 3, &src_len);
	if (src) {
		run_external_script(ctx, (const char *)src, src_len);
		return LXB_STATUS_OK;
	}

	size_t len = 0;
	lxb_char_t *text = lxb_dom_node_text_content(node, &len);
	if (!text || len == 0)
		return LXB_STATUS_OK;

	eval_script(ctx, (const char *)text, len, "<script>");

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

	struct growable_buf resp = { 0 };
	long status = 0;
	char effective_url[SCRIPT_URL_BUF_SIZE];

	CURLcode res = fetch_url(url, &resp, &status, effective_url, sizeof(effective_url));
	if (res != CURLE_OK) {
		printf("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
		curl_global_cleanup();
		free(resp.data);
		return 1;
	}

	printf("status: %ld\n", status);
	printf("body: %zu byte(s)\n\n", resp.len);

	// curl_global_cleanup() moved to the very end of main() -- run_scripts()
	// below calls fetch_url() again for every external <script src>, and
	// calling curl_easy_init() after curl_global_cleanup() without a fresh
	// curl_global_init() is undefined behavior per libcurl's own contract.
	// This exact bug produced a real Exception 0x13 (#GP) boot crash the
	// first time this was tried against a page with an external script
	// (iana.org's jQuery) -- global cleanup must bracket every curl call
	// in the process, not just the first one.

	// External <script src="..."> is resolved against the page's final,
	// post-redirect URL -- the correct base per the HTML/URL specs, not
	// necessarily the URL originally requested. lxb_url_parser_clean()
	// resets the reusable parser before this first real parse the same
	// way it's reset before each later script-src parse.
	lxb_url_parser_init(&g_url_parser, NULL);
	g_base_url = lxb_url_parse(&g_url_parser, NULL,
		(const lxb_char_t *)effective_url, strlen(effective_url));
	lxb_url_parser_clean(&g_url_parser);

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
	if (g_base_url)
		lxb_url_memory_destroy(g_base_url);
	lxb_url_parser_destroy(&g_url_parser, false);
	free(resp.data);
	curl_global_cleanup();

	return 0;
}
