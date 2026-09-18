// yamltest.c -- standalone isolation repro for the parked GC-assert
// crash documented in BROWSER.md ("identify the leaked objects behind
// the parked GC-assert crash" / this round's follow-up). No lexbor, no
// DOM, no Element/document/window bindings at all -- just QuickJS
// evaluating the real, unmodified js-yaml 4.1.0 UMD bundle (embedded
// verbatim in js_yaml_src.h, fetched from cdnjs -- the same library
// Swagger UI's real bundle vendors internally, whose module-load-time
// Schema singletons were the objects the leak dump identified by name).
//
// Point: does loading real js-yaml source ALONE reproduce the same
// `Object leaks:` dump / `Assertion failed: list_empty(&rt->gc_obj_list)`
// crash that showed up fetching Swagger UI's bundle through the full
// browser_fetch.c pipeline? If yes, this isolates the bug to quickjs-ng
// itself (or this exact JS pattern), independent of this project's own
// DOM binding layer. If no, the leak needs something else present
// (DOM bindings, multiple sequential JS_Eval calls, curl-buffer-loaded
// content) to trigger.

#include <stdio.h>
#include "quickjs.h"
#include "js_yaml_src.h"

int main(void)
{
	JSRuntime *rt = JS_NewRuntime();

	// Same fix as browser_fetch.c: quickjs-ng's leak-dump
	// infrastructure (ENABLE_DUMPS) is already compiled into this
	// vendored build, just off by default -- turn it on so a leak
	// prints a real object dump instead of silently corrupting state
	// until some later, unrelated instruction faults.
	JS_SetDumpFlags(rt, JS_DUMP_LEAKS);

	JSContext *ctx = JS_NewContext(rt);

	JSValue result = JS_Eval(ctx, (const char *)js_yaml_min_js,
				  js_yaml_min_js_len, "<js-yaml>", JS_EVAL_TYPE_GLOBAL);

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

	// Same shutdown order as browser_fetch.c's main(): free the
	// context, then the runtime -- JS_FreeRuntime() is where the
	// leak-check assertion fires.
	JS_FreeContext(ctx);
	printf("about to JS_FreeRuntime (this is where the leak check runs)\n");
	JS_FreeRuntime(rt);
	printf("JS_FreeRuntime completed cleanly\n");

	return 0;
}
