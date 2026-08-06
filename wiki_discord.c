// wiki_discord.c -- fetches a random Wikipedia article's summary and
// posts its title + first paragraph(s) to a Discord channel via an
// incoming webhook. build with build-app.sh.
//
// This is also a valid *nix program of course.
//
// Two HTTPS requests, both through libcurl (see curltest.c for the
// baseline demo this is built on -- same net_shim.c sockets underneath,
// same vendored mbedTLS backend):
//
//   1. GET  https://en.wikipedia.org/api/rest_v1/page/random/summary
//      MediaWiki's REST "page summary" endpoint for a random article;
//      its "extract" field is already the plain-text lead section
//      (roughly the first paragraph(s), before the first heading).
//   2. POST DISCORD_WEBHOOK_URL, a JSON {"content": "..."} body.
//
// DISCORD_WEBHOOK_URL is intentionally blank below -- it's a per-server
// secret (anyone holding it can post to that channel), not something
// to hardcode into a shared/example file. Create one from a Discord
// channel's Settings -> Integrations -> Webhooks, then fill it in
// before building. main() refuses to run with it left empty.
//
// Same certificate-verification stance as curltest.c/tls_shim.c, and
// for the same reason (no CA store is vendored on this port):
// CURLOPT_SSL_VERIFYPEER/VERIFYHOST are both off below. TLS still buys
// confidentiality on the wire, just not server-identity assurance --
// acceptable for hitting two known-good public endpoints like these,
// but note the webhook URL itself is the only thing authenticating the
// POST to Discord, so treat it like a password regardless.
//
// JSON handling is hand-rolled (see json_extract_string()/
// json_escape_append() below), not a real parser/library -- same
// "good enough for this shape of input" tradeoff https_crawler.c makes
// for HTML link scanning. Two known, accepted gaps: \uXXXX escapes in
// the Wikipedia response are dropped rather than decoded (real-world
// MediaWiki responses emit non-ASCII as raw UTF-8 bytes, not \u
// escapes, so this rarely matters), and truncate_utf8() below only
// avoids splitting a *2-4 byte UTF-8 sequence* at the truncation point
// -- it doesn't otherwise validate the input is well-formed UTF-8.
//
// All buffers are static, fixed-size, not malloc'd -- this is a
// memory-constrained microVM (see port/lwip_port/lwipopts.h), same
// reasoning curltest.c's response_buf and https_crawler.c's page_buf
// give.

#include <stdio.h>
#include <string.h>

#include <curl/curl.h>

#define WIKI_SUMMARY_URL   "https://en.wikipedia.org/api/rest_v1/page/random/summary"
#define DISCORD_WEBHOOK_URL "PUT_YOUR_WEBHOOK_HERE"

#define USER_AGENT "BareMetal-wiki-discord/1.0 (https://github.com/ReturnInfinity/BareMetal-App)"

#define WIKI_BUF_SIZE     (32 * 1024)
#define DISCORD_RESP_SIZE (4 * 1024)
#define TITLE_BUF_SIZE    256
#define EXTRACT_BUF_SIZE  (8 * 1024)
#define CONTENT_MAX       1900 // Discord hard-caps message content at 2000 UTF-8 chars; leave headroom
#define PAYLOAD_BUF_SIZE  8192 // JSON-escaping content can expand it up to ~2x

struct membuf {
	char *data;
	size_t cap;
	size_t len;
};

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	struct membuf *mb = userdata;
	size_t n = size * nmemb;

	size_t room = mb->cap - 1 - mb->len;
	size_t copy = n < room ? n : room;
	memcpy(mb->data + mb->len, ptr, copy);
	mb->len += copy;
	mb->data[mb->len] = '\0';

	return n; // report all of n "written" even on overflow -- see curltest.c's write_cb
}

// libcurl setup shared by both requests below.
static void curl_common_opts(CURL *h)
{
	curl_easy_setopt(h, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L); // see file header
	curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L); // matches net_shim.c's own per-call cap
}

// Extracts the value of the first "key":"..." string field from json
// (a flat scan, not a real parser -- see file header) into out.
// Decodes \" \\ \/ \n \r \t; drops \uXXXX escapes and any other
// backslash escape rather than decoding them. Returns 1 if the key was
// found, 0 otherwise (out is left empty).
static int json_extract_string(const char *json, const char *key, char *out, size_t outsz)
{
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\":\"", key);

	out[0] = '\0';
	const char *p = strstr(json, pat);
	if (!p)
		return 0;
	p += strlen(pat);

	size_t n = 0;
	while (*p && *p != '"' && n + 1 < outsz) {
		if (*p == '\\' && p[1]) {
			p++;
			switch (*p) {
			case 'n': out[n++] = '\n'; break;
			case 'r': out[n++] = '\r'; break;
			case 't': out[n++] = '\t'; break;
			case '"': out[n++] = '"'; break;
			case '\\': out[n++] = '\\'; break;
			case '/': out[n++] = '/'; break;
			case 'u':
				for (int i = 0; i < 4 && p[1]; i++)
					p++;
				break;
			default:
				break; // unknown escape -- drop it
			}
			p++;
		} else {
			out[n++] = *p++;
		}
	}
	out[n] = '\0';
	return 1;
}

// If s is longer than maxlen bytes, trims it back to at most maxlen,
// stepping back further if needed so the cut doesn't land inside a
// multi-byte UTF-8 sequence (a continuation byte has its top two bits
// as "10"). Does not otherwise validate that s is well-formed UTF-8.
static void truncate_utf8(char *s, size_t maxlen)
{
	size_t len = strlen(s);
	if (len <= maxlen)
		return;

	size_t cut = maxlen;
	while (cut > 0 && (s[cut] & 0xC0) == 0x80)
		cut--;

	s[cut] = '\0';
}

// Appends s to out (a nul-terminated buffer of size outsz, *pos bytes
// already used), JSON-escaping it as it goes. Bare '\r' and other
// control characters below 0x20 are dropped rather than escaped --
// none are expected in a Wikipedia extract, and Discord's content
// field doesn't need them.
static void json_escape_append(char *out, size_t outsz, size_t *pos, const char *s)
{
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\') {
			if (*pos + 2 >= outsz)
				break;
			out[(*pos)++] = '\\';
			out[(*pos)++] = c;
		} else if (c == '\n') {
			if (*pos + 2 >= outsz)
				break;
			out[(*pos)++] = '\\';
			out[(*pos)++] = 'n';
		} else if (c == '\t') {
			if (*pos + 2 >= outsz)
				break;
			out[(*pos)++] = '\\';
			out[(*pos)++] = 't';
		} else if (c < 0x20) {
			// drop other control chars, including bare '\r'
		} else {
			if (*pos + 1 >= outsz)
				break;
			out[(*pos)++] = c;
		}
	}
	out[*pos] = '\0';
}

int main(void)
{
	printf("BareMetal wiki_discord -- random Wikipedia article -> Discord webhook\n\n");

	if (DISCORD_WEBHOOK_URL[0] == '\0') {
		fprintf(stderr, "error: DISCORD_WEBHOOK_URL is empty -- edit wiki_discord.c "
				"and set it to your channel's webhook URL before building "
				"(Discord: channel Settings -> Integrations -> Webhooks).\n");
		return 1;
	}

	curl_global_init(CURL_GLOBAL_DEFAULT);

	static char wiki_buf[WIKI_BUF_SIZE];
	struct membuf wiki_mb = { wiki_buf, sizeof(wiki_buf), 0 };

	CURL *h = curl_easy_init();
	if (!h) {
		fprintf(stderr, "curl_easy_init() failed\n");
		curl_global_cleanup();
		return 1;
	}

	printf("GET %s\n", WIKI_SUMMARY_URL);
	curl_common_opts(h);
	curl_easy_setopt(h, CURLOPT_URL, WIKI_SUMMARY_URL);
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, &wiki_mb);

	CURLcode res = curl_easy_perform(h);
	long status = 0;
	curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);

	if (res != CURLE_OK || status != 200) {
		fprintf(stderr, "fetch failed: %s (HTTP %ld)\n",
			curl_easy_strerror(res), status);
		curl_easy_cleanup(h);
		curl_global_cleanup();
		return 1;
	}
	printf("status: %ld, %zu byte(s)\n\n", status, wiki_mb.len);

	static char title[TITLE_BUF_SIZE];
	static char extract[EXTRACT_BUF_SIZE];
	json_extract_string(wiki_buf, "title", title, sizeof(title));
	int have_extract = json_extract_string(wiki_buf, "extract", extract, sizeof(extract));

	if (title[0] == '\0' || !have_extract || extract[0] == '\0') {
		fprintf(stderr, "couldn't find a title/extract in the response "
				"(unexpected page type, or a malformed response)\n");
		curl_easy_cleanup(h);
		curl_global_cleanup();
		return 1;
	}

	printf("article: %s\n\n%s\n\n", title, extract);

	// Plain-text message body, truncated to fit Discord's content
	// limit before JSON-escaping (escaping can only grow it further).
	static char body[TITLE_BUF_SIZE + EXTRACT_BUF_SIZE + 16];
	int blen = snprintf(body, sizeof(body), "**%s**\n\n%s", title, extract);
	if (blen < 0)
		blen = 0;
	if ((size_t)blen >= sizeof(body))
		blen = sizeof(body) - 1;

	int truncated = (size_t)blen > CONTENT_MAX - 3;
	truncate_utf8(body, truncated ? CONTENT_MAX - 3 : CONTENT_MAX);
	if (truncated)
		strcat(body, "...");

	static char payload[PAYLOAD_BUF_SIZE];
	size_t pos = 0;
	snprintf(payload, sizeof(payload), "{\"content\":\"");
	pos = strlen(payload);
	json_escape_append(payload, sizeof(payload), &pos, body);
	if (pos + 2 < sizeof(payload)) {
		payload[pos++] = '"';
		payload[pos++] = '}';
		payload[pos] = '\0';
	}

	static char discord_resp[DISCORD_RESP_SIZE];
	struct membuf discord_mb = { discord_resp, sizeof(discord_resp), 0 };

	// ?wait=true makes Discord respond with the created message (and a
	// real error body on failure) instead of a bare 204 No Content.
	static char post_url[600];
	snprintf(post_url, sizeof(post_url), "%s?wait=true", DISCORD_WEBHOOK_URL);

	curl_easy_reset(h);
	curl_common_opts(h);
	curl_easy_setopt(h, CURLOPT_URL, post_url);
	curl_easy_setopt(h, CURLOPT_POST, 1L);
	curl_easy_setopt(h, CURLOPT_POSTFIELDS, payload);
	curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)pos);
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, &discord_mb);

	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);

	printf("POST %s (%zu byte payload)\n", DISCORD_WEBHOOK_URL, pos);
	res = curl_easy_perform(h);
	status = 0;
	curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);

	int ok = (res == CURLE_OK && status >= 200 && status < 300);
	printf("status: %ld%s\n", status, ok ? " (posted)" : " (failed)");
	if (discord_mb.len)
		printf("response: %.*s\n", (int)discord_mb.len, discord_resp);
	if (res != CURLE_OK)
		fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));

	curl_slist_free_all(headers);
	curl_easy_cleanup(h);
	curl_global_cleanup();

	return ok ? 0 : 1;
}
