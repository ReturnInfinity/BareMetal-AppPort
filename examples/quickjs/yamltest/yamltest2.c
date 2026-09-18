// yamltest2.c -- second isolation step for the parked GC-assert crash
// (see yamltest.c and BROWSER.md). A single JS_Eval() of real js-yaml
// source alone (yamltest.c) did not reproduce the leak in 21 boots.
// This tries the next-closest structural match to browser_fetch.c's
// real run_scripts(): several SEQUENTIAL JS_Eval() calls against ONE
// shared JSContext (mimicking multiple <script> tags on a page), still
// with no lexbor/DOM/curl involved at all. If this alone doesn't
// reproduce it either, the leak likely needs the DOM binding layer
// itself (Element/document/window) or curl-buffer-loaded content
// present, not just multi-eval structure.

#include <stdio.h>
#include "quickjs.h"
#include "js_yaml_src.h"

static void run_one(JSContext *ctx, const char *name, const char *src, size_t len)
{
	JSValue result = JS_Eval(ctx, src, len, name, JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(result)) {
		JSValue exc = JS_GetException(ctx);
		const char *msg = JS_ToCString(ctx, exc);
		printf("%s exception: %s\n", name, msg);
		JS_FreeCString(ctx, msg);
		JS_FreeValue(ctx, exc);
	} else {
		printf("%s: ok\n", name);
	}
	JS_FreeValue(ctx, result);
}

int main(void)
{
	JSRuntime *rt = JS_NewRuntime();
	JS_SetDumpFlags(rt, JS_DUMP_LEAKS);
	JSContext *ctx = JS_NewContext(rt);

	// Script 1: trivial, like a page's first inline <script> (mirrors
	// browser.c's own hermetic fixture style).
	const char *s1 = "var greeting = 'hi'; console_marker = 1;";
	run_one(ctx, "<script1>", s1, __builtin_strlen(s1));

	// Script 2: the real js-yaml UMD bundle, same as yamltest.c, but
	// now as the SECOND JS_Eval on an already-used context instead of
	// the first and only one.
	run_one(ctx, "<script2-jsyaml>", (const char *)js_yaml_min_js, js_yaml_min_js_len);

	// Script 3: another trivial eval afterward, like a page continuing
	// to run more <script> tags after the one that loaded js-yaml --
	// matches run_scripts() continuing to the next script node.
	const char *s3 = "var after = jsyaml ? 'jsyaml global present' : 'no jsyaml global';";
	run_one(ctx, "<script3>", s3, __builtin_strlen(s3));

	JS_FreeContext(ctx);
	printf("about to JS_FreeRuntime (this is where the leak check runs)\n");
	JS_FreeRuntime(rt);
	printf("JS_FreeRuntime completed cleanly\n");

	return 0;
}
