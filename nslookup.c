#include <assert.h>
#include <arpa/inet.h>
#include <resolv.h>
#include <netdb.h>
#include <stdio.h>
#include <poll.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

enum {
	RR_TYPE_A     = 1,
	RR_TYPE_CNAME = 5,
	RR_TYPE_PTR   = 12,
	RR_TYPE_AAAA  = 28,
	POLL_TIMEOUT  = 5000,
	MAXREVLEN     = 73,
};

static void print_address(const char *label, const char *name,
			  short family, const void *addr)
{
	char str[INET6_ADDRSTRLEN];
	printf("%-10s %s\n", label, name);
	printf("%-10s %s\n", "Address 1:",
	       inet_ntop(family, addr, str, sizeof(str)));
}

static struct addrinfo *resolve_server(const char *server)
{
	/* translate the server name to an address */
	struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_DGRAM,
		.ai_flags = AI_PASSIVE|AI_CANONNAME|AI_NUMERICSERV,
	};
	struct addrinfo *res;

	int rv = getaddrinfo(server, "53", &hints, &res);
	if (rv != 0 || res == NULL) {
		fprintf(stderr, "cannot resolve %s\n", server);
		return NULL;
	}

	/* print the address of the server */
	print_address("Server:", server, res->ai_family,
		      &((struct sockaddr_in *)res->ai_addr)->sin_addr);
	fputc('\n', stdout);

	return res;
}

static int res_ssend(struct addrinfo *srv, const unsigned char *msg,
		     int msglen, unsigned char *answer, int anslen)
{
	struct sockaddr_storage src = {
		.ss_family = srv->ai_family,
	};

	int fd = socket(srv->ai_family,
			SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
	if (fd < 0) {
		fprintf(stderr, "cannot create the socket\n");
		goto err;
	}

	if (bind(fd, (void *)&src, srv->ai_addrlen) < 0) {
		fprintf(stderr, "bind failure\n");
		goto err_close;
	}

	/* send the query */
	if (sendto(fd, msg, msglen, MSG_NOSIGNAL,
		   srv->ai_addr, srv->ai_addrlen) < 0)
	{
		fprintf(stderr, "sendto failure\n");
		goto err_close;
	}

	/* wait for the answer */
	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, POLL_TIMEOUT) <= 0) {
		fprintf(stderr, "poll timeout\n");
		goto err_close;
	}

	/* receive the data */
	size_t alen = anslen;
	ssize_t rlen = recvfrom(fd, answer, alen, 0, NULL, NULL);
	if (rlen < 0) {
		fprintf(stderr, "recvfrom error\n");
		goto err_close;
	}

	close(fd);
	return rlen;

err_close:
	close(fd);
err:
	return -1;
}

static int dns_print(void *ctx, int rr, const uint8_t *data, size_t len,
		     const uint8_t *as, const uint8_t *p, size_t plen)
{
	/* expand the name to a FQDN */
	char name[256];
	if (dn_expand(p, p+plen, as, name, sizeof(name)) < 0) {
		fprintf(stderr, "dn_expand() error\n");
		return -1;
	}

	switch (rr) {
	case RR_TYPE_A:
	case RR_TYPE_AAAA:
		print_address("Name:", name, rr == RR_TYPE_A ? AF_INET : AF_INET6, data);
		break;

	case RR_TYPE_CNAME:
	case RR_TYPE_PTR:
		printf(rr == RR_TYPE_CNAME ? "%s\tcanonical name = " : "%s\tname = ", name);

		if (dn_expand(p, p+plen, data, name, sizeof(name)) < 0) {
			fprintf(stderr, "dn_expand() error\n");
			return -1;
		}
		printf("%s.\n", name);
		break;

	default:
		break;
	}

	return 0;
}

/* modified from MUSL libc code */
static int dns_parse(const unsigned char *r, int rlen, void *ctx)
{
	/* return if we didn't even get the header */
	if (rlen < 12)
		return -1;

	/* return in case of errors */
	if ((r[3] & 15))
		return -1;

	int qdcount = r[4]*256 + r[5];
	int ancount = r[6]*256 + r[7];

	if (qdcount + ancount > 64)
		return -1;

	const unsigned char *p = r+12;
	while (qdcount--) {
		while (p-r < rlen && *p-1U < 127)
			p++;

		if (*p>193 || (*p==193 && p[1]>254) || p>r+rlen-6)
			return -1;

		p += 5 + !!*p;
	}

	while (ancount--) {
		const void *as = p;
		while (p-r < rlen && *p-1U < 127)
			p++;

		if (*p>193 || (*p==193 && p[1]>254) || p>r+rlen-6)
			return -1;

		p += 1 + !!*p;
		size_t len = p[8]*256U + p[9];
		if (p+len > r+rlen)
			return -1;

		if (dns_print(ctx, p[1], p+10, len, as, r, rlen) < 0)
			return -1;

		p += 10 + len;
	}
	return 0;
}

static const char *reverse_lookup(const char *addr, char *buf, size_t len)
{
	assert(len >= MAXREVLEN);

	struct in_addr v4;
	if (inet_pton(AF_INET, addr, &v4) > 0) {
		uint8_t *arr = (uint8_t *)&v4.s_addr + 3;
		char *p = buf;
		for (int i = 0; i < 4; i++) {
			int l = snprintf(p, len, "%u.", *arr);
			if (l < 0 || l >= len)
				return addr;

			arr--;
			p += l;
			len -= l;
		}

		if (len <= 12)
			return addr;

		strncpy(p, "in-addr.arpa", len);
		return buf;
	}

	struct in6_addr v6;
	if (inet_pton(AF_INET6, addr, &v6) > 0) {
		char *p = buf;
		uint8_t *arr = (uint8_t *)v6.s6_addr + 15;
		for (int i = 0; i < 16; i++) {
			int l = snprintf(p, len, "%x.%x.",
					 *arr & 15, (*arr >> 4) & 15);
			if (l < 0 || l >= len)
				return addr;

			arr--;
			p += l;
			len -= l;
		}

		if (len <= 8)
			return addr;

		strncpy(p, "ip6.arpa", len);
		return buf;
	}

	return addr;
}

int main(int argc, char *argv[])
{
	/* check the args */
	if (argc < 2 || argc > 3) {
		fprintf(stderr, "Usage: %s [HOST] [SERVER]\n", argv[0]);
		return EXIT_FAILURE;
	}

	/* change the lookup name if we need a PTR */
	char ptr[MAXREVLEN];
	const char *name = reverse_lookup(argv[1], ptr, sizeof(ptr));

	/* build the query */
	unsigned char query[2][280];
	unsigned char *current_query;
	int qlen, qlen2;
	int qcount;
	if (strcmp(name, argv[1]) == 0){
		qlen = res_mkquery(0, name, ns_c_in, ns_t_a, 0, 0, 0,
						query[0], sizeof(query[0]));
		qlen2 = res_mkquery(0, name, ns_c_in, ns_t_aaaa, 0, 0, 0,
						query[1], sizeof(query[1]));
		qcount = 2;
	} else {
		qlen = res_mkquery(0, name, ns_c_in, ns_t_ptr, 0, 0, 0,
			       query[0], sizeof(query[0]));
		qcount = 1;
	}

	for (int i = 0; i < qcount; i++) {
		switch (i)
		{
		case 0:
			current_query = (unsigned char*) &query[0];
			break;
		case 1:
			current_query = (unsigned char*) &query[1];
			qlen = qlen2;
		default:
			break;
		}

		if (qlen < 0) {
			fprintf(stderr, "cannot build the query\n");
			return EXIT_FAILURE;
		}

		/* send the query to the server */
		struct addrinfo *srv;
		unsigned char response[1024];
		int rlen;

		// Only resolve the server on the first iteration
		if (argc == 2) {
			if (i == 0) {
				srv = resolve_server("127.0.0.1");
				if (srv == NULL)
					return EXIT_FAILURE;
			}

			rlen = res_send(current_query, qlen, response, sizeof(response));
		} else if (argc == 3) {
			if (i == 0) {
				srv = resolve_server(argv[2]);
				if (srv == NULL)
					return EXIT_FAILURE;
			}

			rlen = res_ssend(srv, current_query, qlen,
					response, sizeof(response));
		} else {
			abort();
		}

		// Free on last iteration
		if (i == qcount-1) {
			freeaddrinfo(srv);
		}

		if (rlen < 0) {
			fprintf(stderr, "cannot send the query\n");
			return EXIT_FAILURE;
		}

		/* check if query and response id match */
		if (memcmp(current_query, response, 2)) {
			fprintf(stderr, "qsections don't match\n");
			return EXIT_FAILURE;
		}

		/* decode the response */
		if (dns_parse(response, rlen, NULL) < 0) {
			fprintf(stderr, "decode failure\n");
			return EXIT_FAILURE;
		}
	}

	return EXIT_SUCCESS;
}
