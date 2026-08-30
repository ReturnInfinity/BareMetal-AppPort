// telegram_groq_bot.c - long-polls the Telegram Bot API for incoming
// messages, forwards each message's text to the Groq API
// (OpenAI-compatible chat-completions endpoint) as a one-shot prompt,
// and replies in the same chat with the model's answer.
//
// This is also a valid *nix program of course.
//
// Same libcurl-through-the-real-thing setup as curltest.c/groqtest.c/
// wiki_discord.c: socket()/connect()/send()/recv() end up in
// net_shim.c, TLS ends up in the vendored mbedTLS backend, all reached
// through curl's own vtls/mbedtls.c rather than tls_shim.c's narrower
// wrapper.
//
// Three HTTPS requests, all through one reused CURL handle
// (curl_easy_reset() + curl_common_opts() between calls, same pattern
// wiki_discord.c's per-iteration loop uses):
//
//   1. GET https://api.telegram.org/bot<token>/getUpdates
//      Long-polls for new messages. TELEGRAM_POLL_TIMEOUT_SECS is
//      deliberately kept under net_shim.c's fixed 30s blocking-socket
//      default (see OPENISSUES.md, and groqtest.c's matching comment
//      on CURLOPT_TIMEOUT) rather than set equal to it: Telegram holds
//      the connection open for up to that many seconds waiting for a
//      message, and if that raced right up against our own socket's
//      30s recv() cap, a message that happened to land in the last
//      second could show up as a read timeout instead of a response.
//      20s leaves comfortable margin.
//   2. POST https://api.groq.com/openai/v1/chat/completions
//      One user-role message, no conversation history kept.
//   3. POST https://api.telegram.org/bot<token>/sendMessage
//      JSON body, same as the Groq request rather than form-encoded --
//      one fewer code path (json_escape_append() already exists for
//      the Groq payload).
//
// telegram_bot_token/groq_api_key below are variables, not #defines,
// per-account secrets (Telegram: message @BotFather; Groq:
// https://console.groq.com/keys) -- main() refuses to run with either
// left empty. NOTE: this file currently has real, live credentials
// filled in for local testing -- don't commit/publish it as-is.
//
// Same certificate-verification stance as groqtest.c/wiki_discord.c
// (on, not off like curltest.c): CURLOPT_SSL_VERIFYPEER/VERIFYHOST are
// both on below, checked against disk.img's CA bundle if it's there,
// this binary's own compiled-in copy otherwise (see curltest.c's file
// header) -- both endpoints here carry a bearer secret in the request.
//
// JSON handling is hand-rolled (see json_extract_string()/
// json_extract_int()/json_escape_append() below), not a real parser/
// library -- same "good enough for this shape of input" tradeoff
// groqtest.c/wiki_discord.c make. json_extract_string() is lifted
// as-is from those two files; json_extract_int() is this file's own
// addition (needed for update_id/chat/message "id" fields, which are
// bare JSON numbers, not strings). \uXXXX escapes in Groq's reply are
// dropped rather than decoded, same accepted gap those files have.
//
// A single getUpdates response can carry more than one update if the
// bot fell behind; process_updates() splits the response on each
// "update_id": occurrence and handles them in order, copying one
// update's slice at a time into a fixed-size scratch buffer (bounded
// memcpy, not strcpy) rather than operating on the whole response
// in place -- json_extract_string()/json_extract_int() do unbounded
// forward strstr() scans, and without that slicing a lookup for one
// update's "text" could run past its closing brace and pick up a
// field from the next update instead.
//
// All buffers are static, fixed-size, not malloc'd -- this is a
// memory-constrained microVM (see port/lwip_port/lwipopts.h), same
// reasoning groqtest.c's response_buf/wiki_discord.c's wiki_buf give.
// offset (highest update_id seen + 1) lives only in memory -- a
// restart re-delivers whatever Telegram is still holding, same as any
// bot that doesn't persist it to disk.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>

static char telegram_bot_token[256] = ""; // see https://core.telegram.org/bots#botfather
static char groq_api_key[256]       = ""; // see https://console.groq.com/keys
static const char *groq_model       = "openai/gpt-oss-120b"; // see https://console.groq.com/docs/models
// Sent as the "system" message ahead of the user's text on every Groq
// request (see groq_chat()). Plain compile-time constant, not run
// through json_escape_append() -- keep it free of '"'/'\\' if edited.
static const char *groq_system_prompt = "Be brief. Responses under 1000 characters. No markdown.";
#define CA_BUNDLE_PATH               "/etc/ssl/cacert.pem" // see curltest.c's file header

// See curltest.c's set_ca_bundle() and its matching comment -- same
// disk-first, compiled-in-fallback CA bundle source, weak for the same
// plain-*nix-program-buildable reason.
__attribute__((weak)) const unsigned char cacert_pem[1];
__attribute__((weak)) const unsigned int cacert_pem_len;

#define USER_AGENT "BareMetal-telegram-groq-bot/1.0 (https://github.com/ReturnInfinity/BareMetal-App)"

#define TELEGRAM_POLL_TIMEOUT_SECS 20 // see file header -- kept under net_shim.c's 30s recv cap
#define RETRY_INTERVAL_SECS        5  // backoff after a failed getUpdates/send/API error

#define UPDATES_BUF_SIZE (32 * 1024) // getUpdates response, can hold several backlogged updates
#define UPDATE_BUF_SIZE  (8 * 1024)  // one update's JSON slice
#define TEXT_BUF_SIZE    4096        // one inbound message's "text" field
#define GROQ_PAYLOAD_BUF_SIZE (TEXT_BUF_SIZE * 2 + 256) // JSON-escaping can expand text up to ~2x
#define GROQ_RESP_BUF_SIZE    (32 * 1024)
#define CONTENT_BUF_SIZE      (16 * 1024) // Groq's reply text
#define SEND_TEXT_MAX         4000        // Telegram hard-caps messages at 4096 chars; leave headroom
#define SEND_PAYLOAD_BUF_SIZE (SEND_TEXT_MAX * 2 + 64)
#define SEND_RESP_BUF_SIZE    4096
#define URL_BUF_SIZE          384 // "https://api.telegram.org/bot" + up to a 256-byte token + query

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

// libcurl setup shared by all three requests below.
static void curl_common_opts(CURL *h)
{
	curl_easy_setopt(h, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
	if (access(CA_BUNDLE_PATH, R_OK) == 0) {
		curl_easy_setopt(h, CURLOPT_CAINFO, CA_BUNDLE_PATH);
	} else {
		struct curl_blob blob = { (void *)cacert_pem, cacert_pem_len, CURL_BLOB_NOCOPY };
		curl_easy_setopt(h, CURLOPT_CAINFO_BLOB, &blob);
	}
	curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L); // matches net_shim.c's own per-call cap
}

// Extracts the value of the first "key":"..." string field from json
// (a flat scan, not a real parser -- see file header) into out.
// Decodes \" \\ \/ \n \r \t; drops \uXXXX escapes and any other
// backslash escape rather than decoding them. Returns 1 if the key was
// found, 0 otherwise (out is left empty). Lifted from wiki_discord.c/
// groqtest.c.
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

// Extracts the value of the first "key":<number> field from json (bare
// JSON integer, not a string -- update_id/message_id/chat.id/date are
// all this shape in Telegram's API). Returns 1 and sets *out if found,
// 0 otherwise (*out left untouched). Rejects a quoted value at the
// match point defensively, so a stray same-named string field doesn't
// get silently parsed as 0 by strtol().
static int json_extract_int(const char *json, const char *key, long *out)
{
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\":", key);

	const char *p = strstr(json, pat);
	if (!p)
		return 0;
	p += strlen(pat);

	while (*p == ' ')
		p++;
	if (*p == '"')
		return 0;

	char *end;
	long v = strtol(p, &end, 10);
	if (end == p)
		return 0;

	*out = v;
	return 1;
}

// If s is longer than maxlen bytes, trims it back to at most maxlen,
// stepping back further if needed so the cut doesn't land inside a
// multi-byte UTF-8 sequence (a continuation byte has its top two bits
// as "10"). Does not otherwise validate that s is well-formed UTF-8.
// Lifted from wiki_discord.c.
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
// already used), JSON-escaping it as it goes. Bare control characters
// below 0x20 (other than \n/\t) are dropped rather than escaped.
// Lifted from wiki_discord.c/groqtest.c.
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

// Sends user_text to Groq's chat completions endpoint. On success,
// copies the reply into out (outsz) and returns 1. On failure (network
// error or non-2xx status), logs why and returns 0.
static int groq_chat(CURL *h, const char *user_text, char *out, size_t outsz)
{
	static char payload[GROQ_PAYLOAD_BUF_SIZE];
	static char resp_buf[GROQ_RESP_BUF_SIZE];

	size_t pos = 0;
	snprintf(payload, sizeof(payload),
		"{\"model\":\"%s\",\"messages\":[{\"role\":\"system\",\"content\":\"%s\"},"
		"{\"role\":\"user\",\"content\":\"",
		groq_model, groq_system_prompt);
	pos = strlen(payload);
	json_escape_append(payload, sizeof(payload), &pos, user_text);
	if (pos + 4 < sizeof(payload)) {
		strcpy(payload + pos, "\"}]}");
		pos += 4;
	}

	static char auth_header[288];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", groq_api_key);

	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, auth_header);

	struct membuf resp_mb = { resp_buf, sizeof(resp_buf), 0 };

	curl_easy_reset(h);
	curl_common_opts(h);
	curl_easy_setopt(h, CURLOPT_URL, "https://api.groq.com/openai/v1/chat/completions");
	curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(h, CURLOPT_POST, 1L);
	curl_easy_setopt(h, CURLOPT_POSTFIELDS, payload);
	curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)pos);
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, &resp_mb);

	printf("POST groq chat/completions (%zu byte payload)\n", pos);
	CURLcode res = curl_easy_perform(h);

	long status = 0;
	curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(headers);

	if (res != CURLE_OK) {
		fprintf(stderr, "groq: curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
		return 0;
	}

	printf("groq status: %ld, %zu byte(s)\n", status, resp_mb.len);

	if (status < 200 || status >= 300) {
		static char errmsg[512];
		if (json_extract_string(resp_buf, "message", errmsg, sizeof(errmsg)))
			fprintf(stderr, "groq: API error (HTTP %ld): %s\n", status, errmsg);
		else
			fprintf(stderr, "groq: API error (HTTP %ld):\n%s\n", status, resp_buf);
		return 0;
	}

	if (!json_extract_string(resp_buf, "content", out, outsz)) {
		fprintf(stderr, "groq: couldn't find a \"content\" field in the response "
				"(unexpected response shape):\n%s\n", resp_buf);
		return 0;
	}

	return 1;
}

// Sends text to the given Telegram chat. Logs failures; nothing else
// to do about them for a bot running unattended.
static void telegram_send_message(CURL *h, long chat_id, const char *text)
{
	static char body[SEND_TEXT_MAX + 8];
	static char payload[SEND_PAYLOAD_BUF_SIZE];
	static char resp_buf[SEND_RESP_BUF_SIZE];
	static char url[URL_BUF_SIZE];

	strncpy(body, text, sizeof(body) - 1);
	body[sizeof(body) - 1] = '\0';
	truncate_utf8(body, SEND_TEXT_MAX);

	size_t pos = 0;
	snprintf(payload, sizeof(payload), "{\"chat_id\":%ld,\"text\":\"", chat_id);
	pos = strlen(payload);
	json_escape_append(payload, sizeof(payload), &pos, body);
	if (pos + 2 < sizeof(payload)) {
		payload[pos++] = '"';
		payload[pos++] = '}';
		payload[pos] = '\0';
	}

	snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", telegram_bot_token);

	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Content-Type: application/json");

	struct membuf resp_mb = { resp_buf, sizeof(resp_buf), 0 };

	curl_easy_reset(h);
	curl_common_opts(h);
	curl_easy_setopt(h, CURLOPT_URL, url);
	curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(h, CURLOPT_POST, 1L);
	curl_easy_setopt(h, CURLOPT_POSTFIELDS, payload);
	curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)pos);
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, &resp_mb);

	CURLcode res = curl_easy_perform(h);
	long status = 0;
	curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(headers);

	if (res != CURLE_OK) {
		fprintf(stderr, "telegram sendMessage: curl_easy_perform() failed: %s\n",
			curl_easy_strerror(res));
		return;
	}
	if (status < 200 || status >= 300) {
		fprintf(stderr, "telegram sendMessage: API error (HTTP %ld): %.*s\n",
			status, (int)resp_mb.len, resp_buf);
	}
}

// Handles one update: extracts chat id + message text, forwards text
// to Groq, replies on Telegram. Falls back to an apology reply if Groq
// fails so the user isn't left hanging.
static void handle_update(CURL *h, const char *update_json)
{
	static char text[TEXT_BUF_SIZE];
	static char reply[CONTENT_BUF_SIZE];

	long chat_id;
	const char *chat = strstr(update_json, "\"chat\":");
	if (!chat || !json_extract_int(chat, "id", &chat_id))
		return; // no chat to reply to (e.g. a non-message update) -- skip

	if (!json_extract_string(update_json, "text", text, sizeof(text)) || text[0] == '\0')
		return; // no text (e.g. a photo/sticker/edited-message-only update) -- skip

	printf("[chat %ld] %s\n", chat_id, text);

	if (groq_chat(h, text, reply, sizeof(reply)))
		telegram_send_message(h, chat_id, reply);
	else
		telegram_send_message(h, chat_id, "Sorry, I couldn't get a response from the AI.");
}

// Splits raw getUpdates JSON on each "update_id": occurrence (see file
// header for why this slicing matters) and hands each update to
// handle_update() in order. Advances *offset past the highest
// update_id seen, regardless of whether handle_update() found
// something to act on, so a message type we skip doesn't get
// re-delivered forever.
static void process_updates(CURL *h, const char *json, long *offset)
{
	static char block[UPDATE_BUF_SIZE];

	const char *marker = "\"update_id\":";
	const char *p = strstr(json, marker);

	while (p) {
		const char *next = strstr(p + strlen(marker), marker);
		size_t len = next ? (size_t)(next - p) : strlen(p);
		if (len >= sizeof(block))
			len = sizeof(block) - 1;

		memcpy(block, p, len);
		block[len] = '\0';

		long update_id;
		if (json_extract_int(block, "update_id", &update_id) && update_id >= *offset)
			*offset = update_id + 1;

		handle_update(h, block);

		p = next;
	}
}

int main(void)
{
	printf("BareMetal telegram_groq_bot -- Telegram <-> Groq (%s) bridge\n\n", groq_model);

	if (telegram_bot_token[0] == '\0' || groq_api_key[0] == '\0') {
		fprintf(stderr, "error: telegram_bot_token and/or groq_api_key is empty -- edit "
				"telegram_groq_bot.c and set both before building "
				"(Telegram: message @BotFather; Groq: https://console.groq.com/keys).\n");
		return 1;
	}

	curl_global_init(CURL_GLOBAL_DEFAULT);

	CURL *h = curl_easy_init();
	if (!h) {
		fprintf(stderr, "curl_easy_init() failed\n");
		curl_global_cleanup();
		return 1;
	}

	static char updates_buf[UPDATES_BUF_SIZE];
	static char url[URL_BUF_SIZE];

	long offset = 0;
	printf("polling for messages...\n\n");

	for (;;) {
		snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/getUpdates?offset=%ld&timeout=%d",
			telegram_bot_token, offset, TELEGRAM_POLL_TIMEOUT_SECS);

		struct membuf mb = { updates_buf, sizeof(updates_buf), 0 };

		curl_easy_reset(h);
		curl_common_opts(h);
		curl_easy_setopt(h, CURLOPT_URL, url);
		curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
		curl_easy_setopt(h, CURLOPT_WRITEDATA, &mb);

		printf("GET getUpdates (offset=%ld)\n", offset);
		CURLcode res = curl_easy_perform(h);
		long status = 0;
		curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
		printf("getUpdates status: %ld, %zu byte(s)\n", status, mb.len);

		if (res != CURLE_OK) {
			fprintf(stderr, "getUpdates: curl_easy_perform() failed: %s, retrying in %ds\n",
				curl_easy_strerror(res), RETRY_INTERVAL_SECS);
			sleep(RETRY_INTERVAL_SECS);
			continue;
		}
		if (status < 200 || status >= 300) {
			static char errmsg[512];
			if (json_extract_string(updates_buf, "description", errmsg, sizeof(errmsg)))
				fprintf(stderr, "getUpdates: API error (HTTP %ld): %s, retrying in %ds\n",
					status, errmsg, RETRY_INTERVAL_SECS);
			else
				fprintf(stderr, "getUpdates: API error (HTTP %ld), retrying in %ds\n",
					status, RETRY_INTERVAL_SECS);
			sleep(RETRY_INTERVAL_SECS);
			continue;
		}

		process_updates(h, updates_buf, &offset);
	}

	curl_easy_cleanup(h);
	curl_global_cleanup();

	return 0;
}
