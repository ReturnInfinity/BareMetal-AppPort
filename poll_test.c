// poll_test.c -- exercises real poll()/select() readiness and
// non-blocking sockets: EAGAIN/EINPROGRESS/SO_ERROR/FIONREAD/
// MSG_DONTWAIT on their own, then a single-threaded poll()-driven HTTP
// server on port 80 that keeps serving while other connections sit
// idle. An HTTP request for /quit stops the server.
// build with build-app.sh
//
// The client checks connect to CLIENT_HOST (10.0.2.2 -- the host, under
// QEMU user-mode networking): CLIENT_PORT should have an HTTP server
// listening on it, and CLOSED_PORT nothing.
//
// This is also a valid *nix program of course.

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define CLIENT_HOST "10.0.2.2"
#define CLIENT_PORT 8000
#define CLOSED_PORT 1
#define SERVER_PORT 80
#define MAX_CONNS   12

static int failures;

static void check(int ok, const char *what)
{
	printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		failures++;
}

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static struct sockaddr_in addr_of(const char *ip, int port)
{
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	inet_pton(AF_INET, ip, &a.sin_addr);
	return a;
}

static void client_tests(void)
{
	// Not connected yet: recv() fails rather than waiting.
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	char buf[512];
	check(recv(fd, buf, sizeof(buf), 0) < 0 && errno == ENOTCONN, "recv() on an unconnected socket is ENOTCONN");
	check((fcntl(fd, F_GETFL) & O_NONBLOCK) != 0, "SOCK_NONBLOCK shows up in F_GETFL");

	struct sockaddr_in a = addr_of(CLIENT_HOST, CLIENT_PORT);
	int r = connect(fd, (struct sockaddr *)&a, sizeof(a));
	check(r < 0 && errno == EINPROGRESS, "non-blocking connect() returns EINPROGRESS");
	check(connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0 && (errno == EALREADY || errno == EISCONN),
		"second connect() is EALREADY/EISCONN");

	struct pollfd p = { .fd = fd, .events = POLLOUT };
	r = poll(&p, 1, 5000);
	int err = -1;
	socklen_t len = sizeof(err);
	getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
	check(r == 1 && (p.revents & POLLOUT) && err == 0, "poll() reports the connect finished, SO_ERROR 0");

	check(recv(fd, buf, sizeof(buf), 0) < 0 && errno == EAGAIN, "recv() with nothing queued is EAGAIN");

	static const char req[] = "GET / HTTP/1.0\r\nHost: " CLIENT_HOST "\r\nConnection: close\r\n\r\n";
	check(send(fd, req, sizeof(req) - 1, 0) == (long)sizeof(req) - 1, "non-blocking send()");

	p.events = POLLIN;
	check(poll(&p, 1, 5000) == 1 && (p.revents & POLLIN), "poll() reports the response arriving");
	int avail = 0;
	check(ioctl(fd, FIONREAD, &avail) == 0 && avail > 0, "FIONREAD counts queued bytes");

	long total = 0;
	long deadline = now_ms() + 5000;
	int eof = 0;
	while (!eof && now_ms() < deadline) {
		long n = recv(fd, buf, sizeof(buf), 0);
		if (n > 0) {
			if (total == 0) {
				buf[n < 15 ? n : 15] = '\0';
				printf("      response starts: %s\n", buf);
			}
			total += n;
		} else if (n == 0) {
			eof = 1;
		} else if (errno == EAGAIN) {
			poll(&p, 1, 1000);
		} else {
			break;
		}
	}
	printf("      read %ld bytes\n", total);
	check(eof && total > 0, "read the whole response to EOF with EAGAIN handling");
	close(fd);

	// Refused connection: reported via POLLOUT/POLLERR + SO_ERROR.
	fd = socket(AF_INET, SOCK_STREAM, 0);
	int on = 1;
	check(ioctl(fd, FIONBIO, &on) == 0 && (fcntl(fd, F_GETFL) & O_NONBLOCK), "FIONBIO sets O_NONBLOCK");
	a = addr_of(CLIENT_HOST, CLOSED_PORT);
	r = connect(fd, (struct sockaddr *)&a, sizeof(a));
	if (r < 0 && errno == EINPROGRESS) {
		p.fd = fd;
		p.events = POLLOUT;
		r = poll(&p, 1, 5000);
		err = 0;
		len = sizeof(err);
		getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
		printf("      poll()=%d revents=0x%x SO_ERROR=%d (%s)\n", r, p.revents, err, strerror(err));
		check(r == 1 && (p.revents & POLLERR) && err == ECONNREFUSED, "refused connect() surfaces ECONNREFUSED");
	} else {
		check(r < 0 && errno == ECONNREFUSED, "refused connect() surfaces ECONNREFUSED");
	}
	close(fd);

	// UDP: MSG_DONTWAIT on an empty queue, POLLOUT always.
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	check(recv(fd, buf, sizeof(buf), MSG_DONTWAIT) < 0 && errno == EAGAIN, "UDP recv(MSG_DONTWAIT) is EAGAIN");
	p.fd = fd;
	p.events = POLLIN | POLLOUT;
	check(poll(&p, 1, 0) == 1 && p.revents == POLLOUT, "UDP socket polls writable, not readable");
	close(fd);
}

struct conn {
	int fd;
	char req[1024];
	int len;
};

static int set_nonblock(int fd)
{
	return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static void server(void)
{
	int lfd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr_of("0.0.0.0", SERVER_PORT);
	if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(lfd, 8) < 0) {
		printf("FAIL: bind/listen\n");
		failures++;
		return;
	}
	set_nonblock(lfd);

	char buf[64];
	check(accept(lfd, NULL, NULL) < 0 && errno == EAGAIN, "accept() with no client is EAGAIN");

	// Idle wait: poll() really sleeps for its timeout, and so does select().
	struct pollfd p = { .fd = lfd, .events = POLLIN };
	long t0 = now_ms();
	int r = poll(&p, 1, 300);
	long dt = now_ms() - t0;
	printf("      poll() idle wait took %ld ms\n", dt);
	check(r == 0 && dt >= 290 && dt < 600, "poll() times out after ~300ms with nothing ready");

	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(lfd, &rfds);
	struct timeval tv = { 0, 200000 };
	t0 = now_ms();
	r = select(lfd + 1, &rfds, NULL, NULL, &tv);
	dt = now_ms() - t0;
	check(r == 0 && !FD_ISSET(lfd, &rfds) && dt >= 190 && dt < 500, "select() times out after ~200ms, set cleared");
	(void)buf;

	printf("SERVER: listening on port %d (GET /quit to stop)\n", SERVER_PORT);

	struct conn conns[MAX_CONNS];
	for (int i = 0; i < MAX_CONNS; i++)
		conns[i].fd = -1;

	int served = 0, quit = 0;
	while (!quit) {
		struct pollfd fds[MAX_CONNS + 1];
		int map[MAX_CONNS + 1];
		int n = 0;
		fds[n].fd = lfd;
		fds[n].events = POLLIN;
		map[n++] = -1;
		for (int i = 0; i < MAX_CONNS; i++) {
			if (conns[i].fd >= 0) {
				fds[n].fd = conns[i].fd;
				fds[n].events = POLLIN;
				map[n++] = i;
			}
		}

		if (poll(fds, n, -1) < 0)
			continue;

		if (fds[0].revents & POLLIN) {
			int cfd;
			while ((cfd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK)) >= 0) {
				int slot = -1;
				for (int i = 0; i < MAX_CONNS; i++)
					if (conns[i].fd < 0) {
						slot = i;
						break;
					}
				if (slot < 0) {
					close(cfd);
					continue;
				}
				conns[slot].fd = cfd;
				conns[slot].len = 0;
			}
		}

		for (int k = 1; k < n; k++) {
			if (!fds[k].revents)
				continue;
			struct conn *c = &conns[map[k]];
			long got = recv(c->fd, c->req + c->len, sizeof(c->req) - 1 - c->len, 0);
			if (got < 0 && errno == EAGAIN)
				continue;
			if (got <= 0) {
				close(c->fd);
				c->fd = -1;
				continue;
			}
			c->len += got;
			c->req[c->len] = '\0';
			if (!strstr(c->req, "\r\n\r\n"))
				continue;

			if (!strncmp(c->req, "GET /quit", 9))
				quit = 1;
			char body[96], resp[256];
			int blen = snprintf(body, sizeof(body), "hello from BareMetal, request #%d\n", ++served);
			int rlen = snprintf(resp, sizeof(resp),
				"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n%s", blen, body);
			send(c->fd, resp, rlen, 0);
			close(c->fd);
			c->fd = -1;
		}
	}

	printf("SERVER: served %d requests\n", served);
	for (int i = 0; i < MAX_CONNS; i++)
		if (conns[i].fd >= 0)
			close(conns[i].fd);
	close(lfd);
}

int main(void)
{
	client_tests();
	server();
	printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
	return failures != 0;
}
