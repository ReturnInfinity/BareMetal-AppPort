// webserver.cpp -- C++ counterpart to webserver.c/webserver.py/webserver-rs:
// a single-threaded HTTP server with a hit counter, built directly on
// BSD sockets (there's no std::net in C++). Exercises the musl ->
// posix_shim -> net_shim -> lwIP TCP server-side (bind/listen/accept)
// path, plus std::string/std::vector/std::cout, the same way the other
// languages' webservers already do.
// build with build-cpp-app.sh

#include <iostream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#define LISTEN_PORT 80

// Escapes '<', '>', and '&' so raw request headers can be embedded in
// the HTML response without being interpreted as markup.
static std::string html_escape(const std::string &src)
{
	std::string out;
	out.reserve(src.size());
	for (char c : src) {
		switch (c) {
		case '<': out += "&lt;"; break;
		case '>': out += "&gt;"; break;
		case '&': out += "&amp;"; break;
		default: out += c; break;
		}
	}
	return out;
}

static const char *style =
	"<style>"
	"body{margin:0;padding:2rem;min-height:100vh;box-sizing:border-box;"
	"background:linear-gradient(135deg,#1e293b,#0f172a);"
	"font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
	"display:flex;justify-content:center;align-items:flex-start}"
	".card{max-width:40rem;width:100%;background:#fff;border-radius:12px;"
	"box-shadow:0 10px 30px rgba(0,0,0,.3);padding:2rem 2.5rem;color:#1e293b}"
	"h1{margin-top:0;color:#0f172a}"
	".hits{display:inline-block;background:#e0e7ff;color:#3730a3;"
	"font-weight:600;padding:.25rem .6rem;border-radius:999px;font-size:.9rem}"
	"table{width:100%;border-collapse:collapse;font-size:.9rem;margin:1rem 0}"
	"th,td{text-align:left;padding:.4rem 0;border-bottom:1px solid #e2e8f0}"
	"th{color:#64748b;font-weight:500;width:40%}"
	"pre{background:#0f172a;color:#e2e8f0;padding:1rem;border-radius:8px;"
	"overflow-x:auto;font-size:.85rem;line-height:1.4}"
	"</style>";

static std::string render_page(unsigned long hits, const std::string &escaped_request)
{
	std::vector<std::pair<std::string, std::string>> info_rows = {
		{"g++ version", __VERSION__},
		{"__cplusplus", std::to_string(__cplusplus)},
		{"Platform", "BareMetal unikernel (see BareMetal-AppPort/CPP.md)"},
	};

	std::string rows;
	for (const auto &row : info_rows)
		rows += "<tr><th>" + row.first + "</th><td>" + row.second + "</td></tr>";

	std::string body =
		"<html><head><title>BareMetal Web Server</title>" + std::string(style) +
		"</head><body><div class=\"card\"><h1>Hello from C++!</h1>"
		"<p>This web server is running in a BareMetal microVM.</p>"
		"<p>Hits: <span class=\"hits\">" + std::to_string(hits) + "</span></p>"
		"<table>" + rows + "</table>"
		"<p>Headers received:</p>"
		"<pre>" + escaped_request + "</pre></div></body></html>\n";

	return
		"HTTP/1.0 200 OK\r\n"
		"Content-Type: text/html\r\n"
		"Content-Length: " + std::to_string(body.size()) + "\r\n"
		"Connection: close\r\n"
		"\r\n" + body;
}

int main()
{
	int lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd < 0) {
		std::cout << "socket() failed" << std::endl;
		return 1;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(LISTEN_PORT);
	addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		std::cout << "bind() to port " << LISTEN_PORT << " failed" << std::endl;
		close(lfd);
		return 1;
	}

	if (listen(lfd, 4) < 0) {
		std::cout << "listen() failed" << std::endl;
		close(lfd);
		return 1;
	}

	std::cout << "listening on port " << LISTEN_PORT << std::endl;

	unsigned long hits = 0;

	for (;;) {
		struct sockaddr_in cliaddr;
		socklen_t clilen = sizeof(cliaddr);

		int cfd = accept(lfd, (struct sockaddr *)&cliaddr, &clilen);
		if (cfd < 0) {
			// accept() blocks for at most 30s (see OPENISSUES.md) and
			// returns EAGAIN/EWOULDBLOCK if nobody connected in that
			// window -- just go back and wait for the next one.
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			std::cout << "accept() failed" << std::endl;
			continue;
		}

		hits++;

		std::cout << "connection from " << inet_ntoa(cliaddr.sin_addr)
			  << ":" << ntohs(cliaddr.sin_port) << std::endl;

		// Don't bother parsing the request -- just capture whatever
		// headers the client sent and echo them back on the page.
		char reqbuf[4096];
		int n = recv(cfd, reqbuf, sizeof(reqbuf) - 1, 0);
		reqbuf[n > 0 ? n : 0] = '\0';

		std::string resp = render_page(hits, html_escape(reqbuf));
		send(cfd, resp.data(), resp.size(), 0);
		close(cfd);
	}

	close(lfd);
	return 0;
}
