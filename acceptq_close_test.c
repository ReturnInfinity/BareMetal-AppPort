// acceptq_close_test.c -- regression test for net_shim_close()'s
// accept-queue drain (see OPENISSUES.md's "Unaccepted connections are
// leaked on listener close()" entry, and net_shim.c's
// close_queued_conn()). A host-side script (not part of this app)
// connects into LISTEN_PORT several times per round while this app
// deliberately never calls accept(), then closes the listener -- each
// such connection occupies one of the fixed SOCK_MAX bsock slots
// (net_shim.c) via on_accept(). If net_shim_close() didn't drain the
// queue on a listening socket's close(), those slots would never come
// back, and enough rounds would eventually exhaust the table -- the
// PROBE_SOCKETS check at the end would then start failing.
// build with build-app.sh
//
// This is also a valid *nix program of course (the /dev/tcp-based
// probing this test expects from the host works on any real Linux
// box too).

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PROBE_IP "1.1.1.1"
#define PROBE_PORT 80
#define LISTEN_PORT 9000
#define ROUNDS 2
#define ROUND_WAIT_SEC 5
#define PROBE_SOCKETS 12

int main(void)
{
	// Learn our own real (DHCP-assigned) IP the same trick
	// sockname_test.c uses -- connect out, then getsockname() -- so a
	// host-side script watching this console output knows where to
	// connect back in.
	int pfd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in paddr;
	memset(&paddr, 0, sizeof(paddr));
	paddr.sin_family = AF_INET;
	paddr.sin_port = htons(PROBE_PORT);
	inet_pton(AF_INET, PROBE_IP, &paddr.sin_addr);

	if (pfd >= 0 && connect(pfd, (struct sockaddr *)&paddr, sizeof(paddr)) == 0) {
		struct sockaddr_in me;
		socklen_t melen = sizeof(me);
		getsockname(pfd, (struct sockaddr *)&me, &melen);
		printf("MY_IP %s\n", inet_ntoa(me.sin_addr));
	} else {
		printf("MY_IP unknown (no network reachability -- host-side probing will fail)\n");
	}
	if (pfd >= 0)
		close(pfd);

	for (int r = 1; r <= ROUNDS; r++) {
		int lfd = socket(AF_INET, SOCK_STREAM, 0);
		if (lfd < 0) {
			printf("round %d: socket() failed: %d\n", r, lfd);
			return 1;
		}

		struct sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(LISTEN_PORT);
		addr.sin_addr.s_addr = INADDR_ANY;
		if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			printf("round %d: bind() failed\n", r);
			return 1;
		}
		if (listen(lfd, 8) < 0) {
			printf("round %d: listen() failed\n", r);
			return 1;
		}

		printf("ROUND %d LISTENING port=%d wait=%ds (deliberately not calling accept())\n",
		       r, LISTEN_PORT, ROUND_WAIT_SEC);
		sleep(ROUND_WAIT_SEC);

		close(lfd); // exercises the acceptq drain
		printf("round %d: listener closed\n", r);
	}

	// If queued-but-unaccepted connections leaked a bsock slot each,
	// enough of them would exhaust SOCK_MAX (16, net_shim.c) well
	// before all PROBE_SOCKETS plain socket() calls succeed.
	int fds[PROBE_SOCKETS];
	int ok = 0;
	for (int i = 0; i < PROBE_SOCKETS; i++) {
		fds[i] = socket(AF_INET, SOCK_STREAM, 0);
		if (fds[i] >= 0)
			ok++;
		else
			printf("probe socket %d failed: %d\n", i, fds[i]);
	}
	for (int i = 0; i < PROBE_SOCKETS; i++)
		if (fds[i] >= 0)
			close(fds[i]);

	printf("probe sockets: %d/%d succeeded\n", ok, PROBE_SOCKETS);
	printf(ok == PROBE_SOCKETS ? "PASS\n" : "FAIL\n");
	return ok == PROBE_SOCKETS ? 0 : 1;
}
