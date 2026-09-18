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

// appendChild(child) is a real DOM mutation: it validates its argument
// the way querySelector() already validates a selector string (throws
// a real TypeError on misuse rather than silently no-op'ing), unlike
// getAttribute()/setAttribute() below which stay permissive on missing
// args to match this file's existing soft-fail convention for read
// accessors. lxb_dom_node_append_child() is lexbor's spec-shaped
// Node.appendChild() (see its own header comment) -- it validates the
// insertion itself, not just lxb_dom_node_insert_child()'s raw splice.
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

	// Real Node.appendChild() returns the appended node.
	return JS_DupValue(ctx, argv[0]);
}

// setAttribute(name, value) -- lxb_dom_element_set_attribute() creates
// the attribute if absent or replaces its value if present, matching
// real DOM Element.setAttribute() in one call (no separate "does this
// attribute exist" check needed, unlike some other DOM APIs).
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

// remove() -- the modern, argument-less ChildNode.remove(). lexbor's
// lxb_dom_node_remove() already tolerates a node with no parent (same
// as a real detached node's .remove() being a harmless no-op), so no
// extra guard is needed beyond the usual null-opaque check.
// Node.removeChild(child) (the older, two-party, exception-throwing
// form) is deliberately not added in this pass -- it needs real
// DOM-exception-code translation for "child is not actually a child of
// this node", not just a straight lexbor call, unlike the three
// mutations above.
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

static const JSCFunctionListEntry element_proto_funcs[] = {
	JS_CGETSET_DEF("textContent", element_get_textContent, NULL),
	JS_CGETSET_DEF("tagName", element_get_tagName, NULL),
	JS_CGETSET_DEF("className", element_get_className, NULL),
	JS_CFUNC_DEF("getAttribute", 1, element_getAttribute),
	JS_CFUNC_DEF("appendChild", 1, element_appendChild),
	JS_CFUNC_DEF("setAttribute", 2, element_setAttribute),
	JS_CFUNC_DEF("remove", 0, element_remove),
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

// createElement(tagName) -- lxb_html_document_create_element() (not
// the lower-level lxb_dom_document_create_element() directly) so the
// new element goes through the same HTML-tag/interface-table lookup a
// parsed <div>/<p>/etc already goes through, matching tagName/element
// behavior for the tags this scope actually cares about. The created
// element is allocated from g_document's own long-lived memory arena
// (lxb_dom_document_create_struct() -> lexbor_mraw_calloc(document->mraw,
// ...), confirmed by reading lexbor's own source, not assumed) -- the
// exact same arena every parsed node already lives in, so it stays
// safe to leave un-freed (see the top-of-file comment on
// element_finalizer being a no-op) whether or not the script ever
// calls appendChild() on it. It is NOT inserted into the tree by
// itself -- matches real DOM createElement() semantics exactly.
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
	JS_CFUNC_DEF("createElement", 1, document_createElement),
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

// A real DOM's `Element` is a constructor function -- `typeof Element
// === 'function'`, and `x instanceof Element` walks x's prototype
// chain looking for `Element.prototype` by reference. `new Element()`
// itself throws in a real browser too (Element has no public
// constructor -- you get instances via document.createElement()/
// querySelector()/etc, never by calling Element directly), so this
// matches that rather than silently allowing construction of a
// wrapper with no underlying lxb_dom_node_t.
static JSValue element_ctor_call(JSContext *ctx, JSValueConst this_val,
				  int argc, JSValueConst *argv)
{
	(void)this_val;
	(void)argc;
	(void)argv;
	return JS_ThrowTypeError(ctx, "Illegal constructor");
}

// Must run after register_element_class() -- JS_GetClassProto() reads
// back the exact same prototype object every Element wrapper already
// has as its [[Prototype]] (set at creation by JS_NewObjectClass()),
// so `instanceof` finds a match by reference identity, the same
// OrdinaryHasInstance algorithm every real JS engine uses.
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

	// addEventListener/removeEventListener bound directly on `global` --
	// since window IS global, this covers both `window.addEventListener`
	// and a bare `addEventListener(...)` call, matching how a real page
	// can use either form. Same no-op stub for both: a real
	// implementation would need to actually remove a previously-added
	// listener, but since neither is ever invoked (no event loop), there
	// is no listener registry for "remove" to need to touch.
	JS_SetPropertyStr(ctx, global, "addEventListener",
			   JS_NewCFunction(ctx, window_addEventListener, "addEventListener", 2));
	JS_SetPropertyStr(ctx, global, "removeEventListener",
			   JS_NewCFunction(ctx, window_addEventListener, "removeEventListener", 2));

	register_element_global(ctx, global);

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
	// Six <script> tags, in document order, each exercising one thing:
	// 1) an intentional TypeError, to prove a throwing script doesn't
	//    abort the rest of the page (its own diagnostic line, then
	//    execution continues);
	// 2) document.querySelector("#msg").textContent read from JS and
	//    printed by JS itself via console.log -- the actual point of
	//    this phase, not a C-side print of an eval's return value;
	// 3) .tagName and .getAttribute() on the same element;
	// 4) a selector with no match, proving querySelector resolves to
	//    JS `null` rather than throwing or crashing;
	// 5) typeof Element / document.body instanceof Element -- the
	//    isolated check for register_element_global() below, before
	//    trusting it against a real fetched page;
	// 6) the full DOM-mutation round-trip: create a real element,
	//    attribute it, append it into the live tree, then find it again
	//    via a fresh querySelector() call -- proving it's genuinely in
	//    the tree afterward, not just a JS object floating on its own.
	static const lxb_char_t html[] =
		"<html><body>"
		"<p id=\"msg\" class=\"greeting\">Hello</p>"
		"<script>null.foo;</script>"
		"<script>console.log(\"DOM says: \" + document.querySelector(\"#msg\").textContent);</script>"
		"<script>var el = document.querySelector(\"p.greeting\"); "
		"console.log(\"tagName=\" + el.tagName + \" class=\" + el.getAttribute(\"class\"));</script>"
		"<script>var missing = document.querySelector(\"#nope\"); "
		"console.log(\"missing is \" + missing);</script>"
		"<script>console.log(\"typeof Element=\" + typeof Element); "
		"console.log(\"body instanceof Element=\" + (document.body instanceof Element));</script>"
		"<script>var made = document.createElement(\"div\"); "
		"made.setAttribute(\"id\", \"made\"); "
		"document.body.appendChild(made); "
		"console.log(\"created tagName=\" + document.querySelector(\"#made\").tagName);</script>"
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
