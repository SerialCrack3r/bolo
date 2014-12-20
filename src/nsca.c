#include "bolo.h"
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/ip.h>

#define EPOLL_MAX_FD 64

#define CLIENT_MAX    16384
#define CLIENT_EXPIRE 600

#define NSCA_MAX_HOSTNAME 64
#define NSCA_MAX_SERVICE  128
#define NSCA_MAX_OUTPUT   4096
#define NSCA_PACKET_LEN   12 + 64 + 128 + 4096

typedef struct {
	int    fd;
	size_t bytes;
	struct PACKED {
		uint16_t  version;
		uint32_t  crc32;
		uint32_t  timestamp;
		uint16_t  status;
		char      host[NSCA_MAX_HOSTNAME];
		char      service[NSCA_MAX_SERVICE];
		char      output[NSCA_MAX_OUTPUT];
	} packet;
} client_t;

static client_t* client_new(int fd)
{
	client_t *c = calloc(1, sizeof(client_t));
	c->fd = fd;
	return c;
}

static void client_free(void *_c)
{
	client_t *c = (client_t*)_c;
	close(c->fd);
	free(c);
}

static void inline nonblocking(int fd)
{
	int orig = fcntl(fd, F_GETFL, 0);
	if (orig < 0 || fcntl(fd, F_SETFL, orig|O_NONBLOCK) != 0)
		logger(LOG_CRIT, "nsca listener failed to set fd %i to non-blocking (O_NONBLOCK): %s",
			fd, strerror(errno));
}

void* nsca_listener(void *u)
{
	struct epoll_event ev, events[EPOLL_MAX_FD];
	int n, nfds, epfd, connfd;
	server_t *svr;
	void *db;
	char *s;

	svr = (server_t*)u;
	if (!svr) {
		logger(LOG_CRIT, "nsca listener failed: server context was NULL");
		return NULL;
	}

	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		logger(LOG_CRIT, "nsca listener failed to get a socket descriptor: %s",
				strerror(errno));
		return NULL;
	}

	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(svr->config.nsca_port);
	addr.sin_addr.s_addr = INADDR_ANY;

	n = 1;
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof(n)) != 0) {
		logger(LOG_WARNING, "nsca listener failed to set SO_REUSEADDR on listening socket");
		return NULL;
	}
	if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
		logger(LOG_CRIT, "nsca listener failed to bind socket to port %u\n", svr->config.nsca_port);
		return NULL;
	}
	if (listen(sockfd, 64) != 0) {
		logger(LOG_CRIT, "nsca listener failed to listen on port %u\n", svr->config.nsca_port);
		return NULL;
	}

	nonblocking(sockfd);

	db = zmq_socket(svr->zmq, ZMQ_DEALER);
	if (!db) {
		logger(LOG_CRIT, "nsca listener failed to get a DEALER socket");
		return NULL;
	}
	if (zmq_connect(db, DB_MANAGER_ENDPOINT) != 0) {
		logger(LOG_CRIT, "nsca listener failed to connect to db manager at " DB_MANAGER_ENDPOINT);
		return NULL;
	}

	epfd = epoll_create1(0);
	if (epfd < 0) {
		logger(LOG_CRIT, "nsca listener failed to get an epoll file descriptor: %s",
				strerror(errno));
		return NULL;
	}

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.fd = sockfd;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev) != 0)
		return NULL;

	cache_t *clients = cache_new(CLIENT_MAX, CLIENT_EXPIRE);
	cache_setopt(clients, VIGOR_CACHE_DESTRUCTOR, client_free);

	for (;;) {
		nfds = epoll_wait(epfd, events, EPOLL_MAX_FD, -1);
		if (nfds == -1)
			return NULL;

		for (n = 0; n < nfds; n++) {
			if (events[n].data.fd == sockfd) {
				/* new inbound connection */
				connfd = accept(sockfd, NULL, NULL);
				if (connfd < 0) {
					logger(LOG_ERR, "nsca listener: inbound connect could not be accepted");
					continue;
				}
				nonblocking(connfd);

				ev.events = EPOLLIN | EPOLLET;
				ev.data.fd = connfd;
				if (epoll_ctl(epfd, EPOLL_CTL_ADD, connfd, &ev) != 0)
					return NULL;

				/* push new client state into cache */
				char *id = string("%04x", connfd);
				client_t *c = client_new(connfd);

				if (!cache_set(clients, id, c))
					client_free(c);
				free(id);

			} else {
				char *id = string("%04x", events[n].data.fd);
				client_t *c = cache_get(clients, id);
				if (!c) {
					logger(LOG_CRIT, "nsca listener: inbound data for unknown client %s; ignoring", id);
					free(id);
					continue;
				}

				/* read what's left from the client */
				ssize_t n = read(c->fd, &c->packet + c->bytes,
						NSCA_PACKET_LEN - c->bytes);
				if (n > 0) {
					c->bytes += n;

					if (c->bytes == NSCA_PACKET_LEN) {
						pdu_t *q = pdu_make("UPDATE", 0);
						pdu_extendf(q, "%u", ntohl(c->packet.timestamp));
						pdu_extendf(q, "%s:%s", c->packet.host, c->packet.service);
						pdu_extendf(q, "%u", ntohs(c->packet.status) & 0xff);
						pdu_extendf(q, "%s", c->packet.output);
						pdu_send(q, db);

						pdu_t *a = pdu_recv(db);
						if (strcmp(pdu_type(a), "OK") != 0) {
							logger(LOG_ERR, "nsca listener received an ERROR (in response to an UPDATE) from the db manager: %s",
								s = pdu_string(a, 1)); free(s);
						}

						client_free(c);
					}
				} else {
					client_free(cache_unset(clients, id));
				}
			}
		}
	}

	return NULL;
}
