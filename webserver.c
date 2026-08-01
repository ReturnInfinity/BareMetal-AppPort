// webserver.c -- minimal HTTP server: binds/listens on LISTEN_PORT,
// accepts one connection at a time, and replies with a page greeting
// the client's IP address. Proves the musl -> posix_shim -> net_shim
// -> lwIP TCP server-side (bind/listen/accept) path works end to end.
// build with build-app.sh

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define LISTEN_PORT 80

int main(void)
{
	int lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd < 0) {
		printf("socket() failed\n");
		return 1;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(LISTEN_PORT);
	addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		printf("bind() to port %d failed\n", LISTEN_PORT);
		close(lfd);
		return 1;
	}

	if (listen(lfd, 4) < 0) {
		printf("listen() failed\n");
		close(lfd);
		return 1;
	}

	printf("listening on port %d\n", LISTEN_PORT);

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
			printf("accept() failed\n");
			continue;
		}

		char ip[INET_ADDRSTRLEN];
		strcpy(ip, inet_ntoa(cliaddr.sin_addr));
		printf("connection from %s:%d\n", ip, ntohs(cliaddr.sin_port));

		// Don't bother parsing the request -- every GET gets the same
		// page back. Just drain whatever the client sent so far.
		char reqbuf[1024];
		recv(cfd, reqbuf, sizeof(reqbuf) - 1, 0);

		char body[256];
		int blen = snprintf(body, sizeof(body),
			"<html><body><h1>Hello, %s!</h1></body></html>\n", ip);

		char resp[512];
		int rlen = snprintf(resp, sizeof(resp),
			"HTTP/1.0 200 OK\r\n"
			"Content-Type: text/html\r\n"
			"Content-Length: %d\r\n"
			"Connection: close\r\n"
			"\r\n"
			"%s", blen, body);

		send(cfd, resp, rlen, 0);
		close(cfd);
	}

	close(lfd);
	return 0;
}
