// udp_test.c -- send a raw DNS query (for "example.com" A record) to
// 1.1.1.1:53 over UDP and dump whatever comes back. Proves the musl ->
// posix_shim -> net_shim -> lwIP UDP path works end to end: sendto()
// with an explicit destination, recvfrom() reporting the sender.
// build with build-app.sh
//
// This is also a valid *nix program of course.
//
// No DNS resolver in this port (see OPENISSUES.md) -- this just
// speaks raw DNS-over-UDP by hand to a literal IP, it doesn't use
// getaddrinfo()/gethostbyname().

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HOST_IP "1.1.1.1"
#define HOST_PORT 53

// Decodes a DNS name starting at buf[pos] into out (dotted, NUL-terminated),
// following RFC 1035 compression pointers. Returns the offset in buf just
// past the name as it appears at the call site (i.e. past the 2-byte
// pointer, if the name started with one -- not past whatever it points to),
// or -1 if the name runs off the end of the buffer or nests pointers too
// deep to plausibly be real.
static long parse_name(const unsigned char *buf, long len, long pos, char *out, size_t outsz)
{
	size_t outn = 0;
	long ret = -1;
	int hops = 0;

	if (outsz)
		out[0] = '\0';

	for (;;) {
		if (pos < 0 || pos >= len || hops++ > 128)
			return -1;

		unsigned char c = buf[pos];
		if (c == 0) {
			pos++;
			if (ret < 0)
				ret = pos;
			break;
		}
		if ((c & 0xC0) == 0xC0) {
			if (pos + 1 >= len)
				return -1;
			if (ret < 0)
				ret = pos + 2;
			pos = ((c & 0x3F) << 8) | buf[pos + 1];
			continue;
		}
		if (pos + 1 + c > len)
			return -1;
		if (outn != 0 && outn + 1 < outsz)
			out[outn++] = '.';
		for (int i = 0; i < c && outn + 1 < outsz; i++)
			out[outn++] = (char)buf[pos + 1 + i];
		out[outn] = '\0';
		pos += 1 + c;
	}

	return ret;
}

static uint16_t get16(const unsigned char *buf, long pos)
{
	return (uint16_t)((buf[pos] << 8) | buf[pos + 1]);
}

static uint32_t get32(const unsigned char *buf, long pos)
{
	return ((uint32_t)buf[pos] << 24) | ((uint32_t)buf[pos + 1] << 16) |
	       ((uint32_t)buf[pos + 2] << 8) | (uint32_t)buf[pos + 3];
}

// Prints the DNS header, question, and answer sections in human-readable
// form. Deliberately doesn't walk the authority/additional sections --
// this is a test program proving the socket path works, not a resolver.
static void dump_dns_message(const unsigned char *buf, long n)
{
	if (n < 12) {
		printf("  (message too short to be a DNS header)\n");
		return;
	}

	uint16_t id = get16(buf, 0);
	uint16_t flags = get16(buf, 2);
	uint16_t qdcount = get16(buf, 4);
	uint16_t ancount = get16(buf, 6);
	uint16_t nscount = get16(buf, 8);
	uint16_t arcount = get16(buf, 10);

	printf("  id=0x%04x qr=%d opcode=%d aa=%d tc=%d rd=%d ra=%d rcode=%d\n",
		id, (flags >> 15) & 1, (flags >> 11) & 0xF, (flags >> 10) & 1,
		(flags >> 9) & 1, (flags >> 8) & 1, (flags >> 7) & 1, flags & 0xF);
	printf("  qdcount=%u ancount=%u nscount=%u arcount=%u\n",
		qdcount, ancount, nscount, arcount);

	long pos = 12;
	char name[256];

	for (uint16_t i = 0; i < qdcount; i++) {
		pos = parse_name(buf, n, pos, name, sizeof(name));
		if (pos < 0 || pos + 4 > n) {
			printf("  question section truncated\n");
			return;
		}
		printf("  question: %s type=%u class=%u\n", name, get16(buf, pos), get16(buf, pos + 2));
		pos += 4;
	}

	for (uint16_t i = 0; i < ancount; i++) {
		pos = parse_name(buf, n, pos, name, sizeof(name));
		if (pos < 0 || pos + 10 > n) {
			printf("  answer section truncated\n");
			return;
		}

		uint16_t type = get16(buf, pos);
		uint16_t class = get16(buf, pos + 2);
		uint32_t ttl = get32(buf, pos + 4);
		uint16_t rdlen = get16(buf, pos + 8);
		pos += 10;

		if (pos + rdlen > n) {
			printf("  answer rdata truncated\n");
			return;
		}

		printf("  answer: %s type=%u class=%u ttl=%u rdlen=%u", name, type, class, ttl, rdlen);
		if (type == 1 && rdlen == 4) // A record
			printf(" addr=%u.%u.%u.%u", buf[pos], buf[pos + 1], buf[pos + 2], buf[pos + 3]);
		else if (type == 5) { // CNAME
			char cname[256];
			if (parse_name(buf, n, pos, cname, sizeof(cname)) >= 0)
				printf(" cname=%s", cname);
		}
		printf("\n");

		pos += rdlen;
	}
}

int main(void)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
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

	// Minimal hand-built DNS query: header + QNAME "example.com" + QTYPE A + QCLASS IN.
	static const unsigned char query[] = {
		0x12, 0x34,             // ID
		0x01, 0x00,             // flags: recursion desired
		0x00, 0x01,             // QDCOUNT = 1
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // AN/NS/AR COUNT = 0
		7, 'e','x','a','m','p','l','e',
		3, 'c','o','m',
		0,                       // end of QNAME
		0x00, 0x01,             // QTYPE = A
		0x00, 0x01,             // QCLASS = IN
	};

	if (sendto(fd, query, sizeof(query), 0, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		printf("sendto() failed\n");
		close(fd);
		return 1;
	}

	unsigned char buf[512];
	struct sockaddr_in from;
	socklen_t fromlen = sizeof(from);
	long n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
	if (n < 0) {
		printf("recvfrom() failed\n");
		close(fd);
		return 1;
	}

	char from_ip[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &from.sin_addr, from_ip, sizeof(from_ip));
	printf("got %ld bytes from %s:%d\n", n, from_ip, ntohs(from.sin_port));

	// Raw DNS wire format, not text -- hex dump it, 16 bytes per line.
	for (long i = 0; i < n; i += 16) {
		for (long j = i; j < i + 16 && j < n; j++)
			printf("%02x ", buf[j]);
		printf("\n");
	}

	printf("\nbreakdown:\n");
	dump_dns_message(buf, n);

	close(fd);
	return 0;
}
