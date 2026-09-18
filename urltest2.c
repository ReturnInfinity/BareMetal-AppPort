// urltest2.c -- isolates whether the browser_fetch.c GP-fault crash is
// purely a lexbor `url`-module reuse bug (base URL kept alive across
// lxb_url_parser_clean(), then the SAME parser/mraw reused for two more
// relative-URL resolutions) with NO curl, NO QuickJS, NO real network
// fetch involved at all -- same exact call sequence browser_fetch.c
// uses (see its resolve_script_url()/main() base-URL setup), just with
// hardcoded strings instead of real fetched data.

#include <stdio.h>
#include <string.h>

#include <lexbor/url/url.h>

static lxb_url_parser_t g_url_parser;
static lxb_url_t *g_base_url;

struct url_buf {
	char *data;
	size_t len;
	size_t cap;
};

static lxb_status_t url_serialize_cb(const lxb_char_t *data, size_t len, void *ctx)
{
	struct url_buf *ub = ctx;
	size_t room = ub->cap - 1 - ub->len;
	size_t copy = len < room ? len : room;
	memcpy(ub->data + ub->len, data, copy);
	ub->len += copy;
	return LXB_STATUS_OK;
}

static int resolve(const char *src, char *out, size_t out_cap)
{
	lxb_url_parser_clean(&g_url_parser);
	lxb_url_t *resolved = lxb_url_parse(&g_url_parser, g_base_url,
		(const lxb_char_t *)src, strlen(src));
	if (!resolved) {
		printf("resolve(%s) failed to parse\n", src);
		return 0;
	}

	struct url_buf ub = { out, 0, out_cap };
	lxb_url_serialize(resolved, url_serialize_cb, &ub, false);
	out[ub.len] = '\0';

	lxb_url_destroy(resolved);
	return 1;
}

int main(void)
{
	printf("BareMetal urltest2 -- isolating the lexbor url-module reuse pattern\n\n");

	const char *effective_url = "https://www.iana.org/domains/reserved";

	lxb_url_parser_init(&g_url_parser, NULL);
	g_base_url = lxb_url_parse(&g_url_parser, NULL,
		(const lxb_char_t *)effective_url, strlen(effective_url));
	lxb_url_parser_clean(&g_url_parser);

	if (!g_base_url) {
		printf("failed to parse base URL\n");
		return 1;
	}
	printf("base URL parsed OK\n");

	char out[2048];

	printf("resolving script 1 (jquery)...\n");
	if (resolve("/static/js/jquery.a8e7cabd4d49.js", out, sizeof(out)))
		printf("  -> %s\n", out);

	printf("resolving script 2 (dtable)...\n");
	if (resolve("/static/js/dtable.46ee921d4414.js", out, sizeof(out)))
		printf("  -> %s\n", out);

	printf("resolving script 3 (relative-time)...\n");
	if (resolve("/static/js/relative-time.79f0e30be3b8.js", out, sizeof(out)))
		printf("  -> %s\n", out);

	printf("\ndone, no crash\n");

	lxb_url_parser_destroy(&g_url_parser, false);

	return 0;
}
