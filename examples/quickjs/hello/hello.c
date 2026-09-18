#include <stdio.h>
#include "quickjs.h"

/* Minimal embedding, no quickjs-libc: this port's own posix_shim.c
   supplies stdout, not js_std_*'s FILE*-based console/print helpers
   (those pull in quickjs-libc.c, which this port doesn't build -- see
   QUICKJS.md). */
int main(void)
{
	JSRuntime *rt = JS_NewRuntime();
	JSContext *ctx = JS_NewContext(rt);

	const char *src = "function greet(name) { return 'Hello, ' + name + '! 2+2=' + (2+2); } greet('BareMetal');";
	JSValue result = JS_Eval(ctx, src, __builtin_strlen(src), "<hello>", JS_EVAL_TYPE_GLOBAL);

	if (JS_IsException(result)) {
		JSValue exc = JS_GetException(ctx);
		const char *msg = JS_ToCString(ctx, exc);
		printf("QuickJS exception: %s\n", msg);
		JS_FreeCString(ctx, msg);
		JS_FreeValue(ctx, exc);
	} else {
		const char *str = JS_ToCString(ctx, result);
		printf("QuickJS says: %s\n", str);
		JS_FreeCString(ctx, str);
	}

	JS_FreeValue(ctx, result);
	JS_FreeContext(ctx);
	JS_FreeRuntime(rt);
	return 0;
}
