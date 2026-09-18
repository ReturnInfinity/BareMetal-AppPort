// browser.c -- the DOM<->QuickJS binding layer: the piece that actually
// makes this a "browser" rather than three separately-working libraries
// (see QUICKJS.md/LEXBOR.md for the engine and the parser on their own).
//
// Parses a static in-memory HTML string with lexbor, binds a minimal
// `console.log`/`document.querySelector(...).textContent`-shaped API
// into a QuickJS context, then finds every <script> element (in
// document order, skipping any with a `src` attribute -- external
// script fetching is a follow-up, see BROWSER.md) and JS_Eval()s its
// inline text against that context. No event loop, no timers, no
// fetch()/XHR from JS -- synchronous load-and-run only.
//
// Kept hermetic (a static HTML string, no network) so this example's
// output doesn't depend on a live fetch succeeding -- see
// examples/lexbor/fetch/fetch.c for the curl+lexbor pipeline this
// reuses on the parse side.

#include <stdio.h>
#include <string.h>

#include "quickjs.h"

#include <lexbor/html/html.h>
#include <lexbor/css/css.h>
#include <lexbor/selectors/selectors.h>
#include <lexbor/dom/interfaces/element.h>

// The DOM root every document.querySelector() call searches. A real
// multi-document embedder would stash this on the JSContext's opaque
// pointer instead of a file-scope global -- one document per process
// is all this first pass needs.
static lxb_html_document_t *g_document;

static JSClassID element_class_id;

// Element wrapper objects never own the lxb_dom_node_t they point at
// (the lxb_html_document_t does, and outlives every JS value that can
// reference it in this single-document, no-GC-of-the-DOM design) -- so
// there is nothing for QuickJS's GC to free here.
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

	// _upper matches real DOM Element.tagName semantics (uppercase for
	// HTML elements) rather than lxb_dom_element_qualified_name()'s
	// as-parsed case.
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

// document.querySelector() only needs the *first* match, but
// lxb_selectors_find() has no early-stop return value (every existing
// example in this port -- hello.c/fetch.c -- returns LXB_STATUS_OK
// unconditionally too), so this still walks every match and just keeps
// the first one rather than short-circuiting the walk itself.
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

	// A real browser's `window` *is* the global object (window ===
	// globalThis) -- not a separate object with its own copies of every
	// global. Aliasing it this way means `window.document`/
	// `window.console` and `typeof window !== 'undefined'` checks all
	// work for free, and `window.foo = ...` assignments succeed (as a
	// plain property set -- there's no event loop to ever act on them,
	// see BROWSER.md's non-goals). JS_DupValue because JS_SetPropertyStr
	// takes ownership of the value handed to it, and `global` still
	// needs its own reference freed below.
	JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));

	JS_FreeValue(ctx, global);
}

// Runs every <script> element's inline text found in the parsed
// document, in document order (lxb_selectors_find() walks the tree
// depth-first as it's built, which is document order for a freshly
// parsed HTML document -- there is no separate re-sort step here).
// Scripts with a `src` attribute are skipped outright: fetching
// external scripts is explicitly out of scope for this pass (see
// BROWSER.md). A script that throws prints a diagnostic and execution
// continues with the next <script> tag, matching how a real browser
// keeps going after one failed <script> block.
static lxb_status_t script_find_cb(lxb_dom_node_t *node,
				    lxb_css_selector_specificity_t spec, void *ctx_ptr)
{
	(void)spec;
	JSContext *ctx = ctx_ptr;
	lxb_dom_element_t *el = lxb_dom_interface_element(node);

	if (lxb_dom_element_has_attribute(el, (const lxb_char_t *)"src", 3))
		return LXB_STATUS_OK;

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

int main(void)
{
	// Four <script> tags, in document order, each exercising one thing:
	// 1) an intentional TypeError, to prove a throwing script doesn't
	//    abort the rest of the page (its own diagnostic line, then
	//    execution continues);
	// 2) document.querySelector("#msg").textContent read from JS and
	//    printed by JS itself via console.log -- the actual point of
	//    this phase, not a C-side print of an eval's return value;
	// 3) .tagName and .getAttribute() on the same element;
	// 4) a selector with no match, proving querySelector resolves to
	//    JS `null` rather than throwing or crashing.
	static const lxb_char_t html[] =
		"<html><body>"
		"<p id=\"msg\" class=\"greeting\">Hello</p>"
		"<script>null.foo;</script>"
		"<script>console.log(\"DOM says: \" + document.querySelector(\"#msg\").textContent);</script>"
		"<script>var el = document.querySelector(\"p.greeting\"); "
		"console.log(\"tagName=\" + el.tagName + \" class=\" + el.getAttribute(\"class\"));</script>"
		"<script>var missing = document.querySelector(\"#nope\"); "
		"console.log(\"missing is \" + missing);</script>"
		"</body></html>";

	g_document = lxb_html_document_create();
	lxb_status_t status = lxb_html_document_parse(g_document, html, sizeof(html) - 1);
	if (status != LXB_STATUS_OK) {
		printf("lxb_html_document_parse failed: %d\n", status);
		return 1;
	}

	JSRuntime *rt = JS_NewRuntime();
	JSContext *ctx = JS_NewContext(rt);

	register_element_class(rt, ctx);
	setup_globals(ctx);
	run_scripts(ctx);

	JS_FreeContext(ctx);
	JS_FreeRuntime(rt);
	lxb_html_document_destroy(g_document);

	return 0;
}
