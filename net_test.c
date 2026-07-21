// net_test.c -- connect to 1.1.1.1:80, issue a plain HTTP GET, and dump
// whatever the server sends back (headers + body) to stdout. Proves the
// musl -> posix_shim -> net_shim -> lwIP TCP path works end to end.
// build with build-app.sh
//
// This is also a valid *nix program of course.
//
// No DNS in this port (see OPENISSUES.md), so the target is a literal
// IPv4 address. 1.1.1.1 is Cloudflare's public DNS server, but it does
// return HTML -- it's here as a reachable IP to exercise the socket
// path. Point HOST_IP at a real HTTP server to see real HTML.

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HOST_IP "1.1.1.1"
#define HOST_PORT 80

int main(void)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		printf("socket() failed\n");
		return 1;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(HOST_PORT);
	if (inet_pton(AF_INET, HOST_IP, &addr.sin_addr) != 1) {
		printf("inet_pton() failed\n");
		close(fd);
		return 1;
	}

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		printf("connect() to %s:%d failed\n", HOST_IP, HOST_PORT);
		close(fd);
		return 1;
	}

	static const char req[] =
		"GET / HTTP/1.0\r\n"
		"Host: " HOST_IP "\r\n"
		"Connection: close\r\n"
		"\r\n";

	if (send(fd, req, sizeof(req) - 1, 0) < 0) {
		printf("send() failed\n");
		close(fd);
		return 1;
	}

	char buf[1024];
	long n;
	while ((n = recv(fd, buf, sizeof(buf) - 1, 0)) > 0) {
		buf[n] = '\0';
		printf("%s", buf);
	}

	close(fd);
	return 0;
}
