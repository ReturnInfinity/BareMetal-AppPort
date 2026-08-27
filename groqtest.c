// groqtest.c -- sends one chat-completion request to the Groq API
// (https://api.groq.com) via libcurl and prints the model's reply.
//
// This is also a valid *nix program of course.
//
// Same libcurl-through-the-real-thing setup as curltest.c/
// wiki_discord.c: socket()/connect()/send()/recv() end up in
// net_shim.c, TLS ends up in the vendored mbedTLS backend, all reached
// through curl's own vtls/mbedtls.c rather than tls_shim.c's narrower
// wrapper.
//
// GROQ_API_KEY is intentionally blank below -- it's a per-account
// secret (see https://console.groq.com/keys), not something to
// hardcode into a shared/example file. Fill it in before building;
// main() refuses to run with it left empty. GROQ_MODEL/GROQ_PROMPT are
// the two other things you'll likely want to change -- see
// https://console.groq.com/docs/models for the current model list.
//
// Same certificate-verification stance as wiki_discord.c (on, not off
// like curltest.c): CURLOPT_SSL_VERIFYPEER/VERIFYHOST are both on
// below, checked against disk.img's CA bundle if it's there, this
// binary's own compiled-in copy otherwise (see curltest.c's file
// header) -- api.groq.com is a fixed, sensitive (bears your API key)
// endpoint, unlike curltest.c's arbitrary demo URL.
//
// JSON handling is hand-rolled (see json_extract_string()/
// json_escape_append() in wiki_discord.c, reused here as-is), not a
// real parser/library -- good enough for pulling a single top-level
// string field ("content" from the first choice, or "message" from an
// "error" object) out of a response shaped the way Groq's API
// document says it will be. \uXXXX escapes in the reply are dropped
// rather than decoded, same accepted gap wiki_discord.c has.
//
// All buffers are static, fixed-size, not malloc'd -- this is a
// memory-constrained microVM (see port/lwip_port/lwipopts.h), same
// reasoning curltest.c's response_buf gives.

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>

#define GROQ_API_KEY   "" // put your key here -- see https://console.groq.com/keys
#define GROQ_MODEL     "openai/gpt-oss-120b"
#define GROQ_PROMPT    "What is BareMetal OS in one sentence?"
#define GROQ_API_URL   "https://api.groq.com/openai/v1/chat/completions"
#define CA_BUNDLE_PATH "/etc/ssl/cacert.pem" // see curltest.c's file header

// See curltest.c's set_ca_bundle() and its matching comment -- same
// disk-first, compiled-in-fallback CA bundle source, weak for the same
// plain-*nix-program-buildable reason.
__attribute__((weak)) const unsigned char cacert_pem[1];
__attribute__((weak)) const unsigned int cacert_pem_len;

#define USER_AGENT "BareMetal-groqtest/1.0 (https://github.com/ReturnInfinity/BareMetal-App)"

#define RESPONSE_BUF_SIZE (32 * 1024)
#define PAYLOAD_BUF_SIZE  4096 // JSON-escaping the prompt can expand it up to ~2x
#define CONTENT_BUF_SIZE  (16 * 1024)
#define ERROR_BUF_SIZE    512

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

// Extracts the value of the first "key":"..." string field from json
// (a flat scan, not a real parser -- see file header) into out.
// Decodes \" \\ \/ \n \r \t; drops \uXXXX escapes and any other
// backslash escape rather than decoding them. Returns 1 if the key was
// found, 0 otherwise (out is left empty). Lifted from wiki_discord.c.
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

// Appends s to out (a nul-terminated buffer of size outsz, *pos bytes
// already used), JSON-escaping it as it goes. Bare control characters
// below 0x20 (other than \n/\t) are dropped rather than escaped.
// Lifted from wiki_discord.c.
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
	printf("BareMetal groqtest -- one chat completion via the Groq API\n\n");

	if (GROQ_API_KEY[0] == '\0') {
		fprintf(stderr, "error: GROQ_API_KEY is empty -- edit groqtest.c and set it to "
				"your API key before building (https://console.groq.com/keys).\n");
		return 1;
	}

	static char payload[PAYLOAD_BUF_SIZE];
	size_t pos = 0;
	snprintf(payload, sizeof(payload), "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"",
		GROQ_MODEL);
	pos = strlen(payload);
	json_escape_append(payload, sizeof(payload), &pos, GROQ_PROMPT);
	if (pos + 4 < sizeof(payload)) {
		strcpy(payload + pos, "\"}]}");
		pos += 4;
	}

	printf("model: %s\nprompt: %s\n\n", GROQ_MODEL, GROQ_PROMPT);

	curl_global_init(CURL_GLOBAL_DEFAULT);

	CURL *h = curl_easy_init();
	if (!h) {
		fprintf(stderr, "curl_easy_init() failed\n");
		curl_global_cleanup();
		return 1;
	}

	static char auth_header[256];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", GROQ_API_KEY);

	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, auth_header);

	static char response_buf[RESPONSE_BUF_SIZE];
	struct membuf resp_mb = { response_buf, sizeof(response_buf), 0 };

	curl_easy_setopt(h, CURLOPT_URL, GROQ_API_URL);
	curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(h, CURLOPT_POST, 1L);
	curl_easy_setopt(h, CURLOPT_POSTFIELDS, payload);
	curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)pos);
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, &resp_mb);
	curl_easy_setopt(h, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);

	// Same disk-first, compiled-in-fallback CA bundle source as
	// wiki_discord.c's curl_common_opts() -- see curltest.c's file
	// header for why verification is on here (unlike curltest.c
	// itself): this endpoint carries your API key.
	if (access(CA_BUNDLE_PATH, R_OK) == 0) {
		curl_easy_setopt(h, CURLOPT_CAINFO, CA_BUNDLE_PATH);
	} else {
		struct curl_blob blob = { (void *)cacert_pem, cacert_pem_len, CURL_BLOB_NOCOPY };
		curl_easy_setopt(h, CURLOPT_CAINFO_BLOB, &blob);
	}
	curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);

	// net_shim.c's own blocking socket calls already cap at 30s each
	// (see OPENISSUES.md) -- this just makes libcurl's own bookkeeping
	// agree with that instead of using its 0 (never time out) default.
	curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L);

	printf("POST %s (%zu byte payload)\n", GROQ_API_URL, pos);
	CURLcode res = curl_easy_perform(h);

	long status = 0;
	curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(headers);

	int ok = 0;
	if (res != CURLE_OK) {
		fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
	} else {
		printf("status: %ld, %zu byte(s)\n\n", status, resp_mb.len);

		if (status >= 200 && status < 300) {
			static char content[CONTENT_BUF_SIZE];
			if (json_extract_string(response_buf, "content", content, sizeof(content))) {
				printf("%s\n", content);
				ok = 1;
			} else {
				fprintf(stderr, "couldn't find a \"content\" field in the response "
						"(unexpected response shape):\n%s\n", response_buf);
			}
		} else {
			static char errmsg[ERROR_BUF_SIZE];
			if (json_extract_string(response_buf, "message", errmsg, sizeof(errmsg)))
				fprintf(stderr, "API error (HTTP %ld): %s\n", status, errmsg);
			else
				fprintf(stderr, "API error (HTTP %ld):\n%s\n", status, response_buf);
		}
	}

	curl_easy_cleanup(h);
	curl_global_cleanup();

	return ok ? 0 : 1;
}
