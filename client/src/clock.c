/* The host's clock, measured from here: an NTP (RFC 5905) client that takes the fastest of a burst of exchanges, and
 * a minimal NTP server (clock-serve) for the machine the host runs on. Nothing sets a clock: the client works out
 * how far the server's clock is from this machine's, so its times can be given in the host's. */
#include <arpa/inet.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "test_client.h"

/* Seconds from 1900 (NTP's epoch) to 1970. */
#define NTP_UNIX_OFFSET 2208988800ULL
#define NTP_PACKET 48

int64_t clock_realtime_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static uint32_t get_be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

void clock_put_timestamp(uint8_t *p, int64_t unix_us)
{
	put_be32(p, (uint32_t)(unix_us / 1000000 + NTP_UNIX_OFFSET));
	put_be32(p + 4, (uint32_t)(((uint64_t)(unix_us % 1000000) << 32) / 1000000));
}

int64_t clock_get_timestamp(const uint8_t *p)
{
	int64_t seconds = (int64_t)get_be32(p) - (int64_t)NTP_UNIX_OFFSET;
	return seconds * 1000000 + (int64_t)(((uint64_t)get_be32(p + 4) * 1000000) >> 32);
}

/* "host", "host:port" or "[v6]:port"; port 123 when none. */
static int open_server(const char *server)
{
	char host[256], port[16] = "123";
	const char *colon = strrchr(server, ':');
	if (server[0] == '[') {
		const char *end = strchr(server, ']');
		if (end == NULL || end - server - 1 >= (long)sizeof host)
			return -1;
		snprintf(host, sizeof host, "%.*s", (int)(end - server - 1), server + 1);
		if (end[1] == ':')
			snprintf(port, sizeof port, "%s", end + 2);
	} else if (colon != NULL && strchr(server, ':') == colon) {
		snprintf(host, sizeof host, "%.*s", (int)(colon - server), server);
		snprintf(port, sizeof port, "%s", colon + 1);
	} else {
		snprintf(host, sizeof host, "%s", server);
	}
	struct addrinfo hints = {.ai_socktype = SOCK_DGRAM}, *res;
	if (getaddrinfo(host, port, &hints, &res) != 0)
		return -1;
	int fd = -1;
	for (struct addrinfo *a = res; a != NULL && fd < 0; a = a->ai_next) {
		fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) != 0) {
			close(fd);
			fd = -1;
		}
	}
	freeaddrinfo(res);
	return fd;
}

bool clock_measure(const char *server, int tries, struct clock_result *out)
{
	memset(out, 0, sizeof *out);
	int fd = open_server(server);
	if (fd < 0)
		return false;
	for (int i = 0; i < tries; i++) {
		uint8_t req[NTP_PACKET] = {0}, rep[NTP_PACKET + 16];
		req[0] = 4 << 3 | 3; /* version 4, client */
		int64_t t1 = clock_realtime_us();
		clock_put_timestamp(req + 40, t1);
		if (send(fd, req, sizeof req, 0) != (ssize_t)sizeof req)
			break;
		struct pollfd p = {.fd = fd, .events = POLLIN};
		while (poll(&p, 1, 300) == 1) {
			ssize_t n = recv(fd, rep, sizeof rep, 0);
			int64_t t4 = clock_realtime_us();
			/* A reply from a server (mode 4), not a kiss-of-death (stratum 0), to this request. */
			if (n < NTP_PACKET || (rep[0] & 7) != 4 || rep[1] == 0 || memcmp(rep + 24, req + 40, 8) != 0)
				continue;
			int64_t t2 = clock_get_timestamp(rep + 32), t3 = clock_get_timestamp(rep + 40);
			int64_t delay = (t4 - t1) - (t3 - t2);
			if (delay < 0)
				delay = 0;
			if (out->samples == 0 || delay < 2 * out->uncertainty_us) {
				out->offset_us = ((t2 - t1) + (t3 - t4)) / 2;
				/* The true offset is within half the round trip of this estimate. */
				out->uncertainty_us = delay / 2;
			}
			out->samples++;
			break;
		}
		usleep(20 * 1000);
	}
	close(fd);
	return out->samples > 0;
}

int clock_serve_main(int argc, char **argv)
{
	static const struct option options[] = {{"port", required_argument, NULL, 'p'}, {0}};
	int port = 123, opt;
	optind = 1;
	while ((opt = getopt_long(argc, argv, "", options, NULL)) != -1) {
		if (opt != 'p') {
			fprintf(stderr, "usage: gamestream-test-client clock-serve [--port N]\n");
			return 2;
		}
		port = atoi(optarg);
	}
	int fd = socket(AF_INET6, SOCK_DGRAM, 0);
	int off = 0;
	setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
	struct sockaddr_in6 addr = {.sin6_family = AF_INET6, .sin6_port = htons(port), .sin6_addr = in6addr_any};
	if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
		perror("clock-serve");
		return 1;
	}
	fprintf(stderr, "clock-serve: answering NTP on port %d with this machine's clock\n", port);
	for (;;) {
		uint8_t req[NTP_PACKET + 16], rep[NTP_PACKET] = {0};
		struct sockaddr_storage from;
		socklen_t fromlen = sizeof from;
		ssize_t n = recvfrom(fd, req, sizeof req, 0, (struct sockaddr *)&from, &fromlen);
		int64_t t2 = clock_realtime_us();
		if (n < NTP_PACKET || (req[0] & 7) != 3)
			continue;
		rep[0] = 4 << 3 | 4; /* no leap warning, version 4, server */
		rep[1] = 2;
		rep[2] = req[2];
		rep[3] = (uint8_t)-20; /* precision: about a microsecond */
		memcpy(rep + 12, "HOST", 4);
		clock_put_timestamp(rep + 16, t2);
		memcpy(rep + 24, req + 40, 8);
		clock_put_timestamp(rep + 32, t2);
		clock_put_timestamp(rep + 40, clock_realtime_us());
		sendto(fd, rep, sizeof rep, 0, (struct sockaddr *)&from, fromlen);
	}
}
