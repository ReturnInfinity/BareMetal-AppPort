// msg_test.c -- exercise sendmsg()/recvmsg() through the musl ->
// posix_shim -> net_shim -> lwIP path, over both UDP and TCP, against
// echo servers on the host (HOST_IP). Checks the iovec gather/scatter,
// msg_name in both directions, MSG_TRUNC, and the error cases this port
// defines (ENOTSOCK on a non-socket fd, EOPNOTSUPP for ancillary data).
// build with build-app.sh
//
// This is also a valid *nix program of course. On the host side, any
// UDP and TCP echo server works, e.g.:
//
//   socat UDP-RECVFROM:7000,fork EXEC:cat &
//   socat TCP-LISTEN:7001,reuseaddr,fork EXEC:cat &

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HOST_IP "172.19.0.1"
#define UDP_PORT 7000
#define TCP_PORT 7001

static int failures;

static void check(int ok, const char *what)
{
	printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		failures++;
}

static struct sockaddr_in host_addr(int port)
{
	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	inet_pton(AF_INET, HOST_IP, &sin.sin_addr);
	return sin;
}

static void test_errors(void)
{
	char c = 'x';
	struct iovec iov = { &c, 1 };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };

	errno = 0;
	check(sendmsg(1, &msg, 0) < 0 && errno == ENOTSOCK, "sendmsg() on stdout -> ENOTSOCK");

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in dst = host_addr(UDP_PORT);
	char ctrl[CMSG_SPACE(sizeof(int))];
	memset(ctrl, 0, sizeof(ctrl));
	msg.msg_name = &dst;
	msg.msg_namelen = sizeof(dst);
	msg.msg_control = ctrl;
	msg.msg_controllen = sizeof(ctrl);
	struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
	cm->cmsg_level = IPPROTO_IP;
	cm->cmsg_type = IP_TTL;
	cm->cmsg_len = CMSG_LEN(sizeof(int));
	int ttl = 64;
	memcpy(CMSG_DATA(cm), &ttl, sizeof(ttl));
	// This port rejects any ancillary data with EOPNOTSUPP; Linux sends
	// the datagram with the IP_TTL applied. Either is fine here --
	// anything else isn't.
	errno = 0;
	long r = sendmsg(fd, &msg, 0);
	check(r == 1 || (r < 0 && errno == EOPNOTSUPP),
	      r < 0 ? "sendmsg() with ancillary data -> EOPNOTSUPP" : "sendmsg() with ancillary data -> sent");
	if (r == 1) {
		// Drain the echo before closing.
		char drain;
		struct timeval tv = { 1, 0 };
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		recv(fd, &drain, 1, 0);
	}
	close(fd);
}

static void test_udp(void)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct timeval tv = { 5, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	// Gather: three iovecs, one datagram.
	struct sockaddr_in dst = host_addr(UDP_PORT);
	struct iovec out[3] = { { "hello, ", 7 }, { "", 0 }, { "sendmsg", 7 } };
	struct msghdr smsg = { .msg_name = &dst, .msg_namelen = sizeof(dst), .msg_iov = out, .msg_iovlen = 3 };
	check(sendmsg(fd, &smsg, MSG_NOSIGNAL) == 14, "UDP sendmsg() gathers 3 iovecs into 14 bytes");

	// Scatter into 5+5 bytes: echo is 14, so 10 land and MSG_TRUNC is set.
	char a[5], b[5], ctrl[64];
	struct sockaddr_in src;
	struct iovec in[2] = { { a, sizeof(a) }, { b, sizeof(b) } };
	struct msghdr rmsg = {
		.msg_name = &src, .msg_namelen = sizeof(src),
		.msg_iov = in, .msg_iovlen = 2,
		.msg_control = ctrl, .msg_controllen = sizeof(ctrl),
	};
	long n = recvmsg(fd, &rmsg, 0);
	check(n == 10, "UDP recvmsg() returns the 10 bytes that fit");
	check(n == 10 && memcmp(a, "hello", 5) == 0 && memcmp(b, ", sen", 5) == 0, "UDP recvmsg() scatters across iovecs");
	check(rmsg.msg_flags & MSG_TRUNC, "UDP recvmsg() sets MSG_TRUNC on a short buffer");
	check(rmsg.msg_controllen == 0, "UDP recvmsg() reports no ancillary data");
	check(rmsg.msg_namelen == sizeof(src) && src.sin_port == htons(UDP_PORT) && src.sin_addr.s_addr == dst.sin_addr.s_addr,
	      "UDP recvmsg() fills msg_name with the sender");

	// Again with room to spare: no MSG_TRUNC.
	check(sendmsg(fd, &smsg, 0) == 14, "UDP sendmsg() again");
	char big[64];
	struct iovec in2 = { big, sizeof(big) };
	struct msghdr rmsg2 = { .msg_iov = &in2, .msg_iovlen = 1 };
	n = recvmsg(fd, &rmsg2, 0);
	check(n == 14 && memcmp(big, "hello, sendmsg", 14) == 0 && !(rmsg2.msg_flags & MSG_TRUNC),
	      "UDP recvmsg() whole datagram, no MSG_TRUNC");

	close(fd);
}

static void test_tcp(void)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in dst = host_addr(TCP_PORT);
	if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
		check(0, "TCP connect() to echo server");
		close(fd);
		return;
	}
	struct timeval tv = { 5, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	struct iovec out[3] = { { "ping-", 5 }, { "", 0 }, { "pong", 4 } };
	struct msghdr smsg = { .msg_iov = out, .msg_iovlen = 3 };
	check(sendmsg(fd, &smsg, MSG_NOSIGNAL) == 9, "TCP sendmsg() gathers 3 iovecs");

	// Read until all 9 echoed bytes are in, scattered over 4+64 bytes:
	// the first iovec fills exactly, and recvmsg() must then carry on
	// into the second without blocking for more than the peer sent.
	char a[4], b[64], ctrl[64];
	long got = 0;
	char all[9];
	while (got < 9) {
		struct iovec in[2] = { { a, sizeof(a) }, { b, sizeof(b) } };
		struct msghdr rmsg = { .msg_iov = in, .msg_iovlen = 2, .msg_control = ctrl, .msg_controllen = sizeof(ctrl) };
		long n = recvmsg(fd, &rmsg, 0);
		if (n <= 0)
			break;
		check(rmsg.msg_controllen == 0 && rmsg.msg_namelen == 0, "TCP recvmsg() reports no ancillary data or address");
		long from_a = n < 4 ? n : 4;
		if (got + n <= 9) {
			memcpy(all + got, a, from_a);
			memcpy(all + got + from_a, b, n - from_a);
		}
		got += n;
	}
	check(got == 9 && memcmp(all, "ping-pong", 9) == 0, "TCP recvmsg() returns the echo across iovecs");

	close(fd);
}

int main(void)
{
	test_errors();
	test_udp();
	test_tcp();
	printf("msg_test: %s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
	return failures != 0;
}
