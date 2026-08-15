// sockname_test.c -- exercise getsockname()/getpeername() end to end:
// musl -> posix_shim.c's SYS_getsockname/SYS_getpeername -> net_shim.c's
// net_shim_getsockname()/net_shim_getpeername(). See net_shim.c's
// "getsockname()/getpeername()" section and OPENISSUES.md's Networking
// entry.
// build with build-app.sh
//
// This is also a valid *nix program of course.

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HOST_IP "1.1.1.1"
#define HOST_PORT 80

static int fail;

static void check(int cond, const char *what)
{
	if (cond) {
		printf("ok:   %s\n", what);
	} else {
		printf("FAIL: %s\n", what);
		fail = 1;
	}
}

int main(void)
{
	// 1) A UDP socket bound to INADDR_ANY:0 -- no real link needed,
	// bind() alone assigns a local port through lwIP. getsockname()
	// should report that assigned (nonzero) port back.
	int ufd = socket(AF_INET, SOCK_DGRAM, 0);
	check(ufd >= 0, "socket(SOCK_DGRAM)");

	struct sockaddr_in uaddr;
	memset(&uaddr, 0, sizeof(uaddr));
	uaddr.sin_family = AF_INET;
	uaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	check(bind(ufd, (struct sockaddr *)&uaddr, sizeof(uaddr)) == 0, "bind(udp, ANY:0)");

	struct sockaddr_in got;
	socklen_t gotlen = sizeof(got);
	check(getsockname(ufd, (struct sockaddr *)&got, &gotlen) == 0, "getsockname(udp)");
	check(gotlen == sizeof(got), "getsockname(udp) addrlen");
	check(got.sin_family == AF_INET, "getsockname(udp) family");
	check(ntohs(got.sin_port) != 0, "getsockname(udp) picked a nonzero port");
	printf("  udp local port: %d\n", ntohs(got.sin_port));

	// getpeername() on an unconnected UDP socket: not meaningful,
	// should fail rather than report garbage.
	struct sockaddr_in peer;
	socklen_t peerlen = sizeof(peer);
	errno = 0;
	int rc = getpeername(ufd, (struct sockaddr *)&peer, &peerlen);
	check(rc < 0 && errno == ENOTCONN, "getpeername(unconnected udp) -> ENOTCONN");

	close(ufd);

	// 2) A real TCP connection to 1.1.1.1:80 (same target tcp_test.c
	// uses) -- getsockname() should report our own (ephemeral) local
	// port, getpeername() should report 1.1.1.1:80 back.
	int tfd = socket(AF_INET, SOCK_STREAM, 0);
	check(tfd >= 0, "socket(SOCK_STREAM)");

	struct sockaddr_in taddr;
	memset(&taddr, 0, sizeof(taddr));
	taddr.sin_family = AF_INET;
	taddr.sin_port = htons(HOST_PORT);
	check(inet_pton(AF_INET, HOST_IP, &taddr.sin_addr) == 1, "inet_pton");

	// getpeername() before connect(): not connected yet, should fail.
	peerlen = sizeof(peer);
	errno = 0;
	rc = getpeername(tfd, (struct sockaddr *)&peer, &peerlen);
	check(rc < 0 && errno == ENOTCONN, "getpeername(unconnected tcp) -> ENOTCONN");

	if (connect(tfd, (struct sockaddr *)&taddr, sizeof(taddr)) < 0) {
		printf("connect() to %s:%d failed -- skipping the connected checks "
		       "below (no network reachability in this environment)\n",
		       HOST_IP, HOST_PORT);
		close(tfd);
		goto done;
	}

	gotlen = sizeof(got);
	check(getsockname(tfd, (struct sockaddr *)&got, &gotlen) == 0, "getsockname(connected tcp)");
	check(got.sin_family == AF_INET, "getsockname(connected tcp) family");
	check(ntohs(got.sin_port) != 0, "getsockname(connected tcp) picked a nonzero local port");
	printf("  tcp local:  %s:%d\n", inet_ntoa(got.sin_addr), ntohs(got.sin_port));

	peerlen = sizeof(peer);
	check(getpeername(tfd, (struct sockaddr *)&peer, &peerlen) == 0, "getpeername(connected tcp)");
	check(peer.sin_family == AF_INET, "getpeername(connected tcp) family");
	check(strcmp(inet_ntoa(peer.sin_addr), HOST_IP) == 0, "getpeername(connected tcp) reports remote IP");
	check(ntohs(peer.sin_port) == HOST_PORT, "getpeername(connected tcp) reports remote port");
	printf("  tcp peer:   %s:%d\n", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));

	close(tfd);

done:
	printf(fail ? "FAIL\n" : "PASS\n");
	return fail;
}
