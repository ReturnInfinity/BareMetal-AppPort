// browser_fetch_bundletest.c -- the next concrete step after
// browser_fetch_yamltest.c (see BROWSER.md's "isolate the parked
// GC-leak crash" sections): yamltest.c/yamltest2.c/yamltest3.c ruled
// out plain QuickJS + real js-yaml source alone (63 boots, 0 leaks);
// browser_fetch_yamltest.c ruled out DOM presence + the Element/
// document/window bindings + real js-yaml alone too (30 boots, 0
// leaks). Both used js-yaml's own standalone 39KB UMD build, not
// Swagger UI's real ~1.4MB bundle that originally leaked.
//
// This file isolates the one remaining variable: fetches Swagger UI's
// REAL bundle LIVE from https://httpbin.org/ (the exact file that
// leaked in the first place), but wraps it in the same minimal
// <html><body><script>...</script></body></html> document
// browser_fetch_yamltest.c uses, instead of parsing httpbin.org's own
// real page. This isolates the bundle's own real size/content
// (~1.4MB, several vendored libraries beyond just js-yaml) from
// httpbin.org's specific page structure (its other 2 external
// scripts, its own inline script, real page timing).
//
// Only main() differs from browser_fetch_yamltest.c: instead of
// embedding js-yaml's source from a header, this does a real
// fetch_url() of the bundle (same growable_buf/write_cb machinery
// browser_fetch.c already established) and wraps THAT buffer between
// the same DOC_PREFIX/DOC_SUFFIX. Confirmed via a one-off host-side
// check (curl + grep) that the real bundle contains zero "</script"
// occurrences (case-insensitive) -- the only substring that could
// prematurely close the <script> tag it's embedded in -- so it's safe
// to splice in directly. It does contain 3 "<!--" sequences, but those
// only matter to the HTML tokenizer's script-data-escaped state in
// combination with a later "</script", which never occurs here.

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

#define BUNDLE_URL     "https://httpbin.org/flasgger_static/swagger-ui-bundle.js"
#define RESPONSE_BUF_INITIAL_CAP (64 * 1024)
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
// into `out`. Not exercised in this file (no <script src> in the
// minimal document), kept verbatim from browser_fetch.c/
// browser_fetch_yamltest.c for structural fidelity.
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

	// See browser_fetch.c's matching comment for why lxb_url_destroy()
	// (not lxb_url_memory_destroy()) is required here -- the latter
	// tears down the entire shared mraw arena, not just this one URL.
	lxb_url_destroy(resolved);
	return true;
}

// --- DOM<->QuickJS binding layer, identical to browser_fetch.c's ---

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

static JSValue element_appendChild(JSContext *ctx, JSValueConst this_val,
				    int argc, JSValueConst *argv)
{
	lxb_dom_node_t *parent = JS_GetOpaque(this_val, element_class_id);
	if (!parent)
		return JS_ThrowTypeError(ctx, "appendChild called on a null Element");

	if (argc < 1)
		return JS_ThrowTypeError(ctx, "appendChild requires an argument");

	lxb_dom_node_t *child = JS_GetOpaque(argv[0], element_class_id);
	if (!child)
		return JS_ThrowTypeError(ctx, "appendChild argument must be an Element");

	lxb_dom_exception_code_t code = lxb_dom_node_append_child(parent, child);
	if (code != LXB_DOM_EXCEPTION_OK)
		return JS_ThrowInternalError(ctx, "appendChild failed (DOM exception %d)", (int)code);

	return JS_DupValue(ctx, argv[0]);
}

static JSValue element_setAttribute(JSContext *ctx, JSValueConst this_val,
				     int argc, JSValueConst *argv)
{
	lxb_dom_node_t *node = JS_GetOpaque(this_val, element_class_id);
	if (!node || argc < 2)
		return JS_UNDEFINED;

	const char *name = JS_ToCString(ctx, argv[0]);
	if (!name)
		return JS_EXCEPTION;

	const char *value = JS_ToCString(ctx, argv[1]);
	if (!value) {
		JS_FreeCString(ctx, name);
		return JS_EXCEPTION;
	}

	lxb_dom_element_set_attribute(lxb_dom_interface_element(node),
		(const lxb_char_t *)name, strlen(name),
		(const lxb_char_t *)value, strlen(value));

	JS_FreeCString(ctx, name);
	JS_FreeCString(ctx, value);

	return JS_UNDEFINED;
}

static JSValue element_remove(JSContext *ctx, JSValueConst this_val,
			       int argc, JSValueConst *argv)
{
	(void)ctx;
	(void)argc;
	(void)argv;
	lxb_dom_node_t *node = JS_GetOpaque(this_val, element_class_id);
	if (node)
		lxb_dom_node_remove(node);
	return JS_UNDEFINED;
}

static JSValue element_querySelectorAll(JSContext *ctx, JSValueConst this_val,
					 int argc, JSValueConst *argv);

static const JSCFunctionListEntry element_proto_funcs[] = {
	JS_CGETSET_DEF("textContent", element_get_textContent, NULL),
	JS_CGETSET_DEF("tagName", element_get_tagName, NULL),
	JS_CGETSET_DEF("className", element_get_className, NULL),
	JS_CFUNC_DEF("getAttribute", 1, element_getAttribute),
	JS_CFUNC_DEF("appendChild", 1, element_appendChild),
	JS_CFUNC_DEF("setAttribute", 2, element_setAttribute),
	JS_CFUNC_DEF("remove", 0, element_remove),
	JS_CFUNC_DEF("querySelectorAll", 1, element_querySelectorAll),
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

struct qsa_result {
	JSContext *ctx;
	JSValue arr;
	uint32_t count;
};

static lxb_status_t qsa_find_cb(lxb_dom_node_t *node,
				 lxb_css_selector_specificity_t spec, void *ctx_ptr)
{
	(void)spec;
	struct qsa_result *r = ctx_ptr;
	JS_SetPropertyUint32(r->ctx, r->arr, r->count++, make_element(r->ctx, node));
	return LXB_STATUS_OK;
}

static JSValue query_selector_all(JSContext *ctx, lxb_dom_node_t *root, JSValueConst sel_val)
{
	const char *sel = JS_ToCString(ctx, sel_val);
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

	JSValue arr = JS_NewArray(ctx);
	struct qsa_result result = { .ctx = ctx, .arr = arr, .count = 0 };
	status = lxb_selectors_find(selectors, root, list, qsa_find_cb, &result);

	lxb_selectors_destroy(selectors, true);
	lxb_css_parser_destroy(parser, true);
	lxb_css_selector_list_destroy_memory(list);

	if (status != LXB_STATUS_OK) {
		JS_FreeValue(ctx, arr);
		return JS_ThrowInternalError(ctx, "lxb_selectors_find failed");
	}

	return arr;
}

static JSValue document_querySelectorAll(JSContext *ctx, JSValueConst this_val,
					  int argc, JSValueConst *argv)
{
	(void)this_val;
	if (argc < 1)
		return JS_NewArray(ctx);
	return query_selector_all(ctx, lxb_dom_interface_node(g_document), argv[0]);
}

static JSValue element_querySelectorAll(JSContext *ctx, JSValueConst this_val,
					 int argc, JSValueConst *argv)
{
	lxb_dom_node_t *node = JS_GetOpaque(this_val, element_class_id);
	if (!node)
		return JS_ThrowTypeError(ctx, "querySelectorAll called on a null Element");
	if (argc < 1)
		return JS_NewArray(ctx);
	return query_selector_all(ctx, node, argv[0]);
}

static lxb_dom_node_t *find_by_id_recursive(lxb_dom_node_t *node, const char *id, size_t id_len)
{
	for (; node != NULL; node = node->next) {
		if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
			size_t len = 0;
			const lxb_char_t *value = lxb_dom_element_get_attribute(
				lxb_dom_interface_element(node),
				(const lxb_char_t *)"id", 2, &len);
			if (value && len == id_len && memcmp(value, id, id_len) == 0)
				return node;
		}

		lxb_dom_node_t *found = find_by_id_recursive(node->first_child, id, id_len);
		if (found)
			return found;
	}

	return NULL;
}

static JSValue document_getElementById(JSContext *ctx, JSValueConst this_val,
					int argc, JSValueConst *argv)
{
	(void)this_val;
	if (argc < 1)
		return JS_NULL;

	const char *id = JS_ToCString(ctx, argv[0]);
	if (!id)
		return JS_EXCEPTION;

	lxb_dom_node_t *root = lxb_dom_interface_node(g_document);
	lxb_dom_node_t *found = find_by_id_recursive(root->first_child, id, strlen(id));
	JS_FreeCString(ctx, id);

	if (!found)
		return JS_NULL;

	return make_element(ctx, found);
}

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

static JSValue document_createElement(JSContext *ctx, JSValueConst this_val,
				       int argc, JSValueConst *argv)
{
	(void)this_val;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "createElement requires a tag name");

	const char *name = JS_ToCString(ctx, argv[0]);
	if (!name)
		return JS_EXCEPTION;

	lxb_html_element_t *el = lxb_html_document_create_element(g_document,
		(const lxb_char_t *)name, strlen(name), NULL);
	JS_FreeCString(ctx, name);

	if (!el)
		return JS_ThrowInternalError(ctx, "createElement failed");

	return make_element(ctx, lxb_dom_interface_node(el));
}

static const JSCFunctionListEntry document_props[] = {
	JS_CFUNC_DEF("querySelector", 1, document_querySelector),
	JS_CFUNC_DEF("querySelectorAll", 1, document_querySelectorAll),
	JS_CFUNC_DEF("getElementById", 1, document_getElementById),
	JS_CFUNC_DEF("createElement", 1, document_createElement),
	JS_CGETSET_DEF("documentElement", document_get_documentElement, NULL),
	JS_CGETSET_DEF("body", document_get_body, NULL),
	JS_CGETSET_DEF("head", document_get_head, NULL),
};

static JSValue window_addEventListener(JSContext *ctx, JSValueConst this_val,
					int argc, JSValueConst *argv)
{
	(void)ctx;
	(void)this_val;
	(void)argc;
	(void)argv;
	return JS_UNDEFINED;
}

static JSValue element_ctor_call(JSContext *ctx, JSValueConst this_val,
				  int argc, JSValueConst *argv)
{
	(void)this_val;
	(void)argc;
	(void)argv;
	return JS_ThrowTypeError(ctx, "Illegal constructor");
}

static void register_element_global(JSContext *ctx, JSValue global)
{
	JSValue ctor = JS_NewCFunction(ctx, element_ctor_call, "Element", 0);
	JSValue proto = JS_GetClassProto(ctx, element_class_id);
	JS_SetPropertyStr(ctx, ctor, "prototype", proto);
	JS_SetPropertyStr(ctx, global, "Element", ctor);
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

	JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));

	JS_SetPropertyStr(ctx, global, "addEventListener",
			   JS_NewCFunction(ctx, window_addEventListener, "addEventListener", 2));
	JS_SetPropertyStr(ctx, global, "removeEventListener",
			   JS_NewCFunction(ctx, window_addEventListener, "removeEventListener", 2));

	register_element_global(ctx, global);

	JS_FreeValue(ctx, global);
}

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

#define MAX_SCRIPTS_PER_PAGE 256

struct script_list {
	lxb_dom_node_t *nodes[MAX_SCRIPTS_PER_PAGE];
	size_t count;
};

static lxb_status_t script_find_cb(lxb_dom_node_t *node,
				    lxb_css_selector_specificity_t spec, void *ctx_ptr)
{
	(void)spec;
	struct script_list *list = ctx_ptr;
	if (list->count < MAX_SCRIPTS_PER_PAGE)
		list->nodes[list->count++] = node;
	return LXB_STATUS_OK;
}

static void run_one_script(JSContext *ctx, lxb_dom_node_t *node)
{
	lxb_dom_element_t *el = lxb_dom_interface_element(node);

	size_t src_len = 0;
	const lxb_char_t *src = lxb_dom_element_get_attribute(el,
		(const lxb_char_t *)"src", 3, &src_len);
	if (src) {
		run_external_script(ctx, (const char *)src, src_len);
		return;
	}

	size_t len = 0;
	lxb_char_t *text = lxb_dom_node_text_content(node, &len);
	if (!text || len == 0)
		return;

	eval_script(ctx, (const char *)text, len, "<script>");
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

	struct script_list scripts = { .count = 0 };
	lxb_dom_node_t *root = lxb_dom_interface_node(g_document);
	lxb_selectors_find(selectors, root, list, script_find_cb, &scripts);

	lxb_selectors_destroy(selectors, true);
	lxb_css_parser_destroy(parser, true);
	lxb_css_selector_list_destroy_memory(list);

	for (size_t i = 0; i < scripts.count; i++)
		run_one_script(ctx, scripts.nodes[i]);
}

// Revised from a first attempt that embedded the fetched bundle
// *inline* into the parsed document -- that tripled real memory use
// versus the original bug's actual mechanism (lexbor's HTML parse
// makes its own copy of inline script text, and
// lxb_dom_node_text_content() in run_one_script() makes a THIRD copy
// to hand to JS_Eval; an *external* <script src> never round-trips
// through lexbor's DOM storage at all -- run_external_script() fetches
// straight into a buffer and JS_Eval()s it directly, then frees it).
// Hit a real, reproducible `posix_shim: out of memory` at 32MiB MEMSIZE
// with the inline version because of exactly that 3x overhead -- not a
// leak, just wasteful. This version instead builds a MINIMAL document
// with one `<script src="...">` pointing at the real bundle URL, and
// lets the existing, already-proven run_scripts()/run_external_script()
// path do the real live fetch -- the exact same mechanism that
// originally leaked on httpbin.org's own page, just isolated from that
// page's other 2 scripts/its own inline script/its own page structure.
static const char MINIMAL_DOC[] =
	"<html><body><script src=\"" BUNDLE_URL "\"></script></body></html>";

int main(void)
{
	printf("BareMetal browser -- lexbor + QuickJS (minimal-DOM real-bundle isolation repro)\n");
	printf("Minimal document with one <script src=\"%s\"> (no httpbin.org page)\n\n", BUNDLE_URL);

	curl_global_init(CURL_GLOBAL_DEFAULT);

	size_t resp_len = sizeof(MINIMAL_DOC) - 1;
	const char *resp_data = MINIMAL_DOC;

	printf("body: %zu byte(s)\n\n", resp_len);

	// g_base_url only matters for resolving a RELATIVE <script src>
	// against it -- this document's <script src> is already absolute,
	// so any base works (WHATWG URL parsing resolves an absolute URL
	// independent of base, same as every other absolute external-script
	// URL already proven to work in browser_fetch.c, e.g. jQuery's
	// ajax.googleapis.com src). Still set up for teardown-path
	// consistency, just against a placeholder rather than a real
	// post-redirect URL.
	lxb_url_parser_init(&g_url_parser, NULL);
	static const char PLACEHOLDER_URL[] = "http://localhost/bundletest";
	g_base_url = lxb_url_parse(&g_url_parser, NULL,
		(const lxb_char_t *)PLACEHOLDER_URL, sizeof(PLACEHOLDER_URL) - 1);
	lxb_url_parser_clean(&g_url_parser);

	// --- parse phase: hand the in-memory bytes straight to lexbor ---
	// resp_data/resp_len is length-delimited, not a C string -- passed
	// through with its explicit length, never strlen()'d.

	g_document = lxb_html_document_create();
	lxb_status_t lstatus = lxb_html_document_parse(g_document,
		(const lxb_char_t *)resp_data, resp_len);
	if (lstatus != LXB_STATUS_OK) {
		printf("lxb_html_document_parse failed: %d\n", lstatus);
		return 1;
	}

	size_t title_len = 0;
	const lxb_char_t *title = lxb_html_document_title(g_document, &title_len);
	printf("title: %.*s\n\n", (int)title_len, title ? (const char *)title : "");

	// --- run phase: bind console/document, run every inline <script> ---

	JSRuntime *rt = JS_NewRuntime();
	// Same diagnostic as browser_fetch_yamltest.c -- see BROWSER.md's
	// "identify the leaked objects" section for what this catches.
	JS_SetDumpFlags(rt, JS_DUMP_LEAKS);
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
	curl_global_cleanup();

	return 0;
}
