// crawler.c -- a small breadth-first web crawler: starts at SEED_URL,
// fetches it over plain HTTP, scans the response body for href="..."
// links, and queues whatever it finds (any host, not just the seed's --
// this deliberately follows links across the internet, not just one
// site) to be fetched in turn. Prints one progress line per page as it
// goes. Stops once MAX_PAGES have been fetched or the queue drains,
// whichever comes first -- "crawl the internet" has no natural
// endpoint otherwise.
// build with build-app.sh
//
// This is also a valid *nix program of course.
//
// HTTP only: this port has no TLS, so https:// links are recognized
// and skipped (reported, not silently dropped) rather than attempted.
// That includes the very common case of a plain-http page redirecting
// to https -- e.g. many sites' 301/302 Location: header does exactly
// that -- which is treated the same as a https:// link found in the
// page body.
//
// All storage here is static and fixed-size (queue, visited list, the
// per-page read buffer) rather than malloc'd -- this is a memory-
// constrained microVM (see lwip_port/lwipopts.h's buffer-sizing
// comment), and a fixed, known footprint is easier to reason about
// than an open-ended crawl's actual memory use. That's also why
// MAX_PAGES exists at all: a page can queue far more links than it's
// worth fetching, so without a cap this would just run until the
// queue (or the real internet) is exhausted.
//
// Link discovery is a raw byte scan for href="..." (or ='...'),
// case-insensitive on "href", not a real HTML parser -- good enough
// to find links in real-world markup (including the attribute-spans-
// multiple-lines style older/hand-written HTML sometimes uses)
// without needing a parsing library. It doesn't decode HTML entities
// (e.g. "&amp;" in a query string), which is a known, accepted gap.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define SEED_URL   "http://info.cern.ch/"
#define MAX_PAGES  40 // hard cap on pages actually fetched -- see file header

#define MAX_URL       600 // "http://" + host + ":" + port + path, generously
#define MAX_QUEUE     128
#define MAX_VISITED   MAX_PAGES // exactly one mark_visited() call per fetched page
#define PAGE_BUF_SIZE (32 * 1024) // per-page read buffer; oversized pages are truncated, not rejected

struct url {
	char host[128];
	int port;
	char path[420]; // includes leading '/' and any query string
};

// Parses an absolute "http://host[:port][/path]" string. Returns 1 and
// fills *u on success, 0 if s isn't an "http://" URL or has no host.
static int parse_url(const char *s, struct url *u)
{
	if (strncasecmp(s, "http://", 7) != 0)
		return 0;
	s += 7;

	size_t hn = 0;
	while (*s && *s != '/' && *s != ':' && hn + 1 < sizeof(u->host))
		u->host[hn++] = *s++;
	u->host[hn] = '\0';
	if (hn == 0)
		return 0;

	u->port = 80;
	if (*s == ':') {
		s++;
		int port = 0;
		while (*s >= '0' && *s <= '9') {
			port = port * 10 + (*s - '0');
			s++;
		}
		if (port > 0 && port < 65536)
			u->port = port;
	}

	if (*s == '\0')
		strcpy(u->path, "/");
	else
		snprintf(u->path, sizeof(u->path), "%s", s);

	return 1;
}

// Case-insensitive strstr(); musl only exposes the GNU strcasestr()
// under _GNU_SOURCE, so this stays a portable POSIX-only build.
static const char *stristr(const char *hay, const char *needle)
{
	size_t nlen = strlen(needle);
	for (; *hay; hay++)
		if (strncasecmp(hay, needle, nlen) == 0)
			return hay;
	return NULL;
}

// Collapses "." and ".." segments out of an already-joined path (e.g.
// "/a/b/../c" -> "/a/c"), the way a browser resolving a relative link
// would. A leading ".." past the root is just dropped rather than
// erroring -- malformed input, but not worth failing the whole crawl
// over.
//
// tmp/segs are static rather than stack locals: this port's per-app
// stack is a fixed 64KiB (see BareMetal's kernel.asm), shared with
// whatever's nested underneath a call to this (recv() -> net_poll()
// -> lwIP's own, none-too-small-at -O0 TCP input processing), so
// large stack buffers here are a real overflow risk rather than a
// style nicety. Safe since this is single-threaded and not
// reentrant -- same reasoning net_shim.c/dns_shim.c already apply to
// their own buffers.
static void normalize_path(const char *in, char *out, size_t outsz)
{
	static char tmp[820];
	snprintf(tmp, sizeof(tmp), "%s", in);

	static char *segs[64];
	int n = 0;

	char *save;
	char *tok = strtok_r(tmp, "/", &save);
	while (tok && n < 64) {
		if (strcmp(tok, ".") == 0) {
			// drop
		} else if (strcmp(tok, "..") == 0) {
			if (n > 0)
				n--;
		} else {
			segs[n++] = tok;
		}
		tok = strtok_r(NULL, "/", &save);
	}

	size_t len = 0;
	for (int i = 0; i < n; i++) {
		size_t seglen = strlen(segs[i]);
		if (len + 1 + seglen + 1 > outsz)
			break;
		out[len++] = '/';
		memcpy(out + len, segs[i], seglen);
		len += seglen;
	}
	if (len == 0) {
		out[0] = '/';
		len = 1;
	}
	out[len] = '\0';
}

// Resolves href (as found in an <a href="...">, or an HTTP Location:
// header -- both use this) against the page it was found on (base),
// producing an absolute "http://host:port/path" string in out.
// Returns 0 (skip this link) for https:// links, any other URL scheme
// (mailto:, javascript:, tel:, ...), or an empty/fragment-only href.
//
// buf/dir/combined are static -- see normalize_path()'s comment on
// why: this port's 64KiB fixed stack has to also fit whatever's
// nested under a later recv() call, so large per-call buffers belong
// in static storage, not on the stack.
static int resolve_link(const struct url *base, const char *href, char *out, size_t outsz)
{
	static char buf[512];
	size_t n = 0;
	for (const char *p = href; *p && *p != '#' && n + 1 < sizeof(buf); p++)
		buf[n++] = *p;
	buf[n] = '\0';

	if (buf[0] == '\0')
		return 0;

	if (strncasecmp(buf, "http://", 7) == 0) {
		snprintf(out, outsz, "%s", buf);
		return 1;
	}
	if (strncasecmp(buf, "https://", 8) == 0)
		return 0; // http-only crawler

	// Any other "scheme:" (mailto:, javascript:, tel:, ftp:, data:,
	// ...) -- a ':' before the first '/' that isn't one of the two
	// schemes just handled above.
	const char *colon = strchr(buf, ':');
	const char *slash = strchr(buf, '/');
	if (colon && (!slash || colon < slash))
		return 0;

	struct url u = *base;

	if (buf[0] == '/' && buf[1] == '/') {
		static char tmp[520];
		snprintf(tmp, sizeof(tmp), "http:%s", buf);
		if (!parse_url(tmp, &u))
			return 0;
	} else if (buf[0] == '/') {
		snprintf(u.path, sizeof(u.path), "%s", buf);
	} else {
		static char dir[420];
		snprintf(dir, sizeof(dir), "%s", base->path);
		char *slash2 = strrchr(dir, '/');
		if (slash2)
			slash2[1] = '\0';
		else
			strcpy(dir, "/");

		static char combined[820];
		snprintf(combined, sizeof(combined), "%s%s", dir, buf);
		normalize_path(combined, u.path, sizeof(u.path));
	}

	if (u.port == 80)
		snprintf(out, outsz, "http://%s%s", u.host, u.path);
	else
		snprintf(out, outsz, "http://%s:%d%s", u.host, u.port, u.path);
	return 1;
}

// ---- Queue + visited-set (fixed-size, see file header) ----

static char queue[MAX_QUEUE][MAX_URL];
static int queue_head, queue_tail;

static char visited[MAX_VISITED][MAX_URL];
static int visited_count;

static int is_known(const char *url)
{
	for (int i = 0; i < visited_count; i++)
		if (strcmp(visited[i], url) == 0)
			return 1;
	for (int i = queue_head; i < queue_tail; i++)
		if (strcmp(queue[i], url) == 0)
			return 1;
	return 0;
}

// Returns 1 if url was newly added, 0 if it was already known or the
// queue is full (silently capped -- a page with far more links than
// MAX_QUEUE has room for just loses the overflow).
static int enqueue(const char *url)
{
	if (is_known(url))
		return 0;
	if (queue_tail >= MAX_QUEUE)
		return 0;
	snprintf(queue[queue_tail], MAX_URL, "%s", url);
	queue_tail++;
	return 1;
}

static void mark_visited(const char *url)
{
	if (visited_count < MAX_VISITED)
		snprintf(visited[visited_count++], MAX_URL, "%s", url);
}

// ---- HTTP fetch ----

struct fetch_result {
	int status;       // parsed HTTP status code, or -1 if the response didn't start with "HTTP/"
	int truncated;     // page_buf filled up before the connection closed
	const char *body;
	size_t body_len;
	char location[MAX_URL]; // Location: header value, if any (redirects)
};

static char page_buf[PAGE_BUF_SIZE];

// Copies the value of the first "name:" header line (case-insensitive)
// out of headers (a nul-terminated block of "\r\n"-separated lines,
// with or without a trailing "\r\n" on the last one), trimmed of
// leading whitespace. *out is left empty if the header isn't present.
static void get_header(const char *headers, const char *name, char *out, size_t outsz)
{
	out[0] = '\0';
	size_t nlen = strlen(name);

	for (const char *line = headers; line && *line; ) {
		const char *eol = strstr(line, "\r\n");
		size_t linelen = eol ? (size_t)(eol - line) : strlen(line);

		if (linelen > nlen && line[nlen] == ':' && strncasecmp(line, name, nlen) == 0) {
			const char *v = line + nlen + 1;
			while (*v == ' ' || *v == '\t')
				v++;
			size_t vlen = (size_t)((line + linelen) - v);
			if (vlen >= outsz)
				vlen = outsz - 1;
			memcpy(out, v, vlen);
			out[vlen] = '\0';
			return;
		}

		line = eol ? eol + 2 : NULL;
	}
}

// Connects, sends a GET, and reads the response into page_buf (until
// the connection closes, the buffer fills, or net_shim's usual 30s
// blocking-call timeout hits). Returns 0 on success (even for a non-
// 200 status -- that's not a transport failure) or -1 if the socket
// couldn't be opened/connected at all.
//
// req/hdrbuf are static -- see normalize_path()'s comment on why.
static int fetch(const struct url *u, struct fetch_result *r)
{
	memset(r, 0, sizeof(*r));
	r->status = -1;

	struct hostent *he = gethostbyname(u->host);
	if (!he)
		return -1;

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(u->port);
	memcpy(&addr.sin_addr, he->h_addr, sizeof(addr.sin_addr));

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	static char req[700];
	int reqlen = snprintf(req, sizeof(req),
		"GET %s HTTP/1.0\r\n"
		"Host: %s\r\n"
		"User-Agent: BareMetal-webcrawler/1.0\r\n"
		"Connection: close\r\n"
		"\r\n",
		u->path, u->host);
	// snprintf() returns the length it *would* have written -- can
	// exceed sizeof(req) if path+host are long, in which case req
	// itself was already truncated (safely) but reqlen wasn't; clamp
	// it too, or send() below would read past the end of req.
	if (reqlen < 0) {
		close(fd);
		return -1;
	}
	if ((size_t)reqlen >= sizeof(req))
		reqlen = sizeof(req) - 1;

	if (send(fd, req, reqlen, 0) < 0) {
		close(fd);
		return -1;
	}

	size_t total = 0;
	long n;
	while (total < sizeof(page_buf) - 1 &&
	       (n = recv(fd, page_buf + total, sizeof(page_buf) - 1 - total, 0)) > 0)
		total += (size_t)n;
	page_buf[total] = '\0';
	close(fd);

	r->truncated = (total >= sizeof(page_buf) - 1);

	char *hdr_end = strstr(page_buf, "\r\n\r\n");
	size_t hdr_len = hdr_end ? (size_t)(hdr_end - page_buf) : total;
	r->body = hdr_end ? hdr_end + 4 : page_buf + total;
	r->body_len = hdr_end ? total - hdr_len - 4 : 0;

	if (strncmp(page_buf, "HTTP/", 5) == 0) {
		const char *sp = strchr(page_buf, ' ');
		if (sp)
			r->status = atoi(sp + 1);
	}

	static char hdrbuf[4096];
	size_t hn = hdr_len < sizeof(hdrbuf) - 1 ? hdr_len : sizeof(hdrbuf) - 1;
	memcpy(hdrbuf, page_buf, hn);
	hdrbuf[hn] = '\0';
	get_header(hdrbuf, "Location", r->location, sizeof(r->location));

	return 0;
}

// Scans body for href="..." attributes and enqueues whatever
// resolve_link() accepts. Reports how many href attributes it found
// in total vs. how many were newly queued (already-known/skipped-
// scheme links account for the rest).
//
// value/abs_url are static -- see normalize_path()'s comment on why.
static void extract_links(const struct url *base, const char *body, int *out_total, int *out_new)
{
	int total = 0, newq = 0;
	const char *p = body;

	while ((p = stristr(p, "href")) != NULL) {
		p += 4;
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
			p++;
		if (*p != '=')
			continue;
		p++;
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
			p++;

		static char value[MAX_URL];
		size_t vn = 0;
		if (*p == '"' || *p == '\'') {
			char quote = *p++;
			while (*p && *p != quote && vn + 1 < sizeof(value))
				value[vn++] = *p++;
			if (*p == quote)
				p++;
		} else {
			while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '>' && vn + 1 < sizeof(value))
				value[vn++] = *p++;
		}
		value[vn] = '\0';
		if (vn == 0)
			continue;

		total++;

		static char abs_url[MAX_URL];
		if (resolve_link(base, value, abs_url, sizeof(abs_url)) && enqueue(abs_url))
			newq++;
	}

	*out_total = total;
	*out_new = newq;
}

int main(void)
{
	printf("BareMetal webcrawler -- HTTP only\n");
	printf("seed: %s   max pages: %d   max queue: %d\n\n", SEED_URL, MAX_PAGES, MAX_QUEUE);

	enqueue(SEED_URL);

	// Static, not stack locals, including the "small" struct
	// fetch_result (its embedded char location[MAX_URL] dominates
	// its size) -- see normalize_path()'s comment on why: this
	// port's 64KiB fixed per-app stack has to also fit lwIP's own
	// nested TCP input processing under every fetch()'s recv() call.
	static char url[MAX_URL];
	static char abs_url[MAX_URL];
	static struct fetch_result r;

	int fetched = 0;
	while (fetched < MAX_PAGES && queue_head < queue_tail) {
		snprintf(url, MAX_URL, "%s", queue[queue_head]);
		queue_head++;

		struct url u;
		if (!parse_url(url, &u)) {
			printf("[skip] %s (unparseable)\n", url);
			continue;
		}

		mark_visited(url);
		fetched++;

		printf("[%d/%d] queue=%d visited=%d GET %s -> ",
			fetched, MAX_PAGES, queue_tail - queue_head, visited_count, url);

		if (fetch(&u, &r) < 0) {
			printf("FAILED (connect/DNS error)\n");
			continue;
		}

		if (r.status < 0) {
			printf("FAILED (not a valid HTTP response)\n");
			continue;
		}

		if (r.status >= 300 && r.status < 400 && r.location[0]) {
			printf("%d redirect -> %s", r.status, r.location);
			if (!resolve_link(&u, r.location, abs_url, MAX_URL))
				printf(" (skipping -- not http)\n");
			else if (enqueue(abs_url))
				printf(" (queued)\n");
			else
				printf(" (already known)\n");
			continue;
		}

		if (r.status != 200) {
			printf("status %d, not following\n", r.status);
			continue;
		}

		int total_links, new_links;
		extract_links(&u, r.body, &total_links, &new_links);

		printf("200 OK (%zu bytes%s, %d links found, %d new)\n",
			r.body_len, r.truncated ? ", truncated" : "", total_links, new_links);
	}

	printf("\ndone: %d page(s) fetched, %d queued but not reached (cap or exhausted)\n",
		fetched, queue_tail - queue_head);

	return 0;
}
