/*
  Copyright (c) 2016 The Bolo Authors.  All Rights Reserved.

  This file is part of Bolo.

  Bolo is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version.

  Bolo is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
  details.

  You should have received a copy of the GNU General Public License along
  with Bolo.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <getopt.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>

#include <vigor.h>
#include <bolo.h>

#define ME "bolo2influxdb"

/**************************/

typedef struct {
	void *control;    /* SUB:  hooked up to supervisor.command; receives control messages */
	void *monitor;    /* PUSH: hooked up to monitor.input; relays monitoring messages */
	void *tock;       /* SUB:  hooked up to scheduler.tick, for timing interrupts */

	void *subscriber; /* SUB:  subscriber to bolo broadcast port (external) */

	int   udp;        /* UDP socket fd, 'connected' to the remote influxdb */

	reactor_t *reactor;
} dispatcher_t;

int dispatcher_thread(void *zmq, const char *endpoint, const char *influx);

static int _dispatcher_reactor(void *socket, pdu_t *pdu, void *_) /* {{{ */
{
	assert(socket != NULL);
	assert(pdu != NULL);
	assert(_ != NULL);

	dispatcher_t *dispatcher = (dispatcher_t*)_;

	if (socket == dispatcher->control) {
		if (strcmp(pdu_type(pdu), "TERMINATE") == 0) {
			return VIGOR_REACTOR_HALT;
		}

		logger(LOG_ERR, "dispatcher thread received unrecognized [%s] PDU from control socket; ignoring",
			pdu_type(pdu));
		return VIGOR_REACTOR_CONTINUE;
	}

	if (socket == dispatcher->tock) {
		return VIGOR_REACTOR_CONTINUE;
	}

	if (socket == dispatcher->subscriber) {
		stopwatch_t watch;
		uint64_t spent = 0;

		STOPWATCH(&watch, spent) {
			char *name, *ts;
			char *type, *item, *metric;
			char *p, pkt[65536];
			size_t len;

			if ((strcmp(pdu_type(pdu), "COUNTER") == 0 && pdu_size(pdu) == 4)
			 || (strcmp(pdu_type(pdu), "SAMPLE")  == 0 && pdu_size(pdu) == 9)
			 || (strcmp(pdu_type(pdu), "RATE")    == 0 && pdu_size(pdu) == 5)) {
				//logger(LOG_INFO, "received a [%s] PDU", pdu_type(pdu));
				ts   = pdu_string(pdu, 1);
				name = pdu_string(pdu, 2);

			} else {
				return VIGOR_REACTOR_CONTINUE;
			}

			p = strchr(name, ':');
			if (!p) {
				fprintf(stderr, "unrecognized pattern `%s'\n", name);
				return VIGOR_REACTOR_CONTINUE;
			}

			*p++ = '\0';
			type = p;

			p = strchr(type, ':');
			if (!p) {
				fprintf(stderr, "unrecognized pattern `%s:%s'\n", name, type);
				return VIGOR_REACTOR_CONTINUE;
			}

			item = NULL;
			*p++ = '\0';
			if (strrchr(p, ':')) {
				item = p;
				p = strrchr(p, ':');
				*p++ = '\0';
			}
			metric = p;

			if (strcmp(pdu_type(pdu), "SAMPLE") == 0) {
				char *n, *min, *max, *sum, *mean, *var;
				n    = pdu_string(pdu, 3);
				min  = pdu_string(pdu, 4);
				max  = pdu_string(pdu, 5);
				sum  = pdu_string(pdu, 6);
				mean = pdu_string(pdu, 7);
				var  = pdu_string(pdu, 8);

				if (item) {
					len = snprintf(pkt, 65536,
						"%s,host=%s,type=%s,item=%s n=%s,min=%s,max=%s,sum=%s,mean=%s,var=%s %s000000000\n",
						metric, name, type, item, n, min, max, sum, mean, var, ts);
				} else {
					len = snprintf(pkt, 65536,
						"%s,host=%s,type=%s n=%s,min=%s,max=%s,sum=%s,mean=%s,var=%s %s000000000\n",
						metric, name, type,       n, min, max, sum, mean, var, ts);
				}

				free(n);
				free(min);
				free(max);
				free(sum);
				free(mean);
				free(var);

			} else if (strcmp(pdu_type(pdu), "COUNTER") == 0) {
				char *value;
				value = pdu_string(pdu, 3);

				if (item) {
					len = snprintf(pkt, 65536,
						"%s,host=%s,type=%s,item=%s value=%s %s000000000\n",
						metric, name, type, item, value, ts);
				} else {
					len = snprintf(pkt, 65536,
						"%s,host=%s,type=%s value=%s %s000000000\n",
						metric, name, type,       value, ts);
				}

				free(value);

			} else if (strcmp(pdu_type(pdu), "RATE") == 0) {
				char *win, *value;
				win   = pdu_string(pdu, 3);
				value = pdu_string(pdu, 4);

				if (item) {
					len = snprintf(pkt, 65536,
						"%s,host=%s,type=%s,item=%s window=%s,value=%s %s000000000\n",
						metric, name, type, item, win, value, ts);
				} else {
					len = snprintf(pkt, 65536,
						"%s,host=%s,type=%s window=%s,value=%s %s000000000\n",
						metric, name, type,       win, value, ts);
				}

				free(win);
				free(value);

			} else {
				return VIGOR_REACTOR_CONTINUE;
			}

			ssize_t n = send(dispatcher->udp, pkt, len, 0);
			if (n >= 0 && n != len) {
				logger(LOG_ERR, "udp send failed (short write) -- only wrote %d of %d bytes", n, len);
			}
		}

		pdu_t *perf = pdu_make("SAMPLE", 1, "dispatch.time.s");
		pdu_extendf(perf, "%lf", spent / 1000.);
		pdu_send_and_free(perf, dispatcher->monitor);

		return VIGOR_REACTOR_CONTINUE;
	}

	logger(LOG_ERR, "unhandled socket!");
	return VIGOR_REACTOR_HALT;
}
/* }}} */
static void * _dispatcher_thread(void *_) /* {{{ */
{
	assert(_ != NULL);

	dispatcher_t *dispatcher = (dispatcher_t*)_;

	reactor_go(dispatcher->reactor);

	logger(LOG_DEBUG, "dispatcher: shutting down");

	zmq_close(dispatcher->subscriber);
	zmq_close(dispatcher->monitor);
	zmq_close(dispatcher->tock);
	zmq_close(dispatcher->control);

	reactor_free(dispatcher->reactor);
	free(dispatcher);

	logger(LOG_DEBUG, "dispatcher: terminated");
	return NULL;
}
/* }}} */
int dispatcher_thread(void *zmq, const char *endpoint, const char *influx) /* {{{ */
{
	assert(zmq != NULL);
	assert(endpoint != NULL);

	int rc, port = 8089;
	char *host, *p;
	struct sockaddr_in addr;

	logger(LOG_INFO, "initializing dispatcher thread");

	dispatcher_t *dispatcher = vmalloc(sizeof(dispatcher_t));

	logger(LOG_DEBUG, "setting up influxdb udp socket");
	dispatcher->udp = socket(AF_INET, SOCK_DGRAM, 0);
	if (dispatcher->udp < 0) {
		logger(LOG_ERR, "failed to create UDP socket");
		return 1;
	}

	host = strdup(influx);
	if (!host) {
		logger(LOG_ERR, "strdup() failed\n");
		return 1;
	}
	p = strchr(host, ':');
	if (p) {
		*p++ = '\0';
		port = atoi(p);
		if (port <= 0 || port > 65535) {
			logger(LOG_ERR, "invalid port: %s", p);
			return 1;
		}
	}
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	rc = inet_pton(AF_INET, host, &addr.sin_addr);
	if (rc != 1) {
		logger(LOG_ERR, "invalid ip address: %s", host);
		return 1;
	}
	rc = connect(dispatcher->udp, (struct sockaddr *)&addr, sizeof(addr));
	if (rc != 0) {
		logger(LOG_ERR, "failed to connect to %s:%d", host, port);
		return rc;
	}

	logger(LOG_DEBUG, "connecting dispatcher.control -> supervisor");
	rc = bolo_subscriber_connect_supervisor(zmq, &dispatcher->control);
	if (rc != 0)
		return rc;

	logger(LOG_DEBUG, "connecting dispatcher.monitor -> monitor");
	rc = bolo_subscriber_connect_monitor(zmq, &dispatcher->monitor);
	if (rc != 0)
		return rc;

	logger(LOG_DEBUG, "connecting dispatcher.tock -> scheduler");
	rc = bolo_subscriber_connect_scheduler(zmq, &dispatcher->tock);
	if (rc != 0)
		return rc;

	logger(LOG_DEBUG, "connecting dispatcher.subscriber <- bolo at %s", endpoint);
	dispatcher->subscriber = zmq_socket(zmq, ZMQ_SUB);
	if (!dispatcher->subscriber)
		return -1;
	rc = zmq_setsockopt(dispatcher->subscriber, ZMQ_SUBSCRIBE, "", 0);
	if (rc != 0)
		return rc;
	rc = vzmq_connect_af(dispatcher->subscriber, endpoint, AF_UNSPEC);
	if (rc != 0)
		return rc;

	logger(LOG_DEBUG, "setting up dispatcher event reactor");
	dispatcher->reactor = reactor_new();
	if (!dispatcher->reactor)
		return -1;

	logger(LOG_DEBUG, "dispatcher: registering dispatcher.control with event reactor");
	rc = reactor_set(dispatcher->reactor, dispatcher->control, _dispatcher_reactor, dispatcher);
	if (rc != 0)
		return rc;

	logger(LOG_DEBUG, "dispatcher: registering dispatcher.tock with event reactor");
	rc = reactor_set(dispatcher->reactor, dispatcher->tock, _dispatcher_reactor, dispatcher);
	if (rc != 0)
		return rc;

	logger(LOG_DEBUG, "dispatcher: registering dispatcher.subscriber with event reactor");
	rc = reactor_set(dispatcher->reactor, dispatcher->subscriber, _dispatcher_reactor, dispatcher);
	if (rc != 0)
		return rc;

	pthread_t tid;
	rc = pthread_create(&tid, NULL, _dispatcher_thread, dispatcher);
	if (rc != 0)
		return rc;

	return 0;
}
/* }}} */



int main(int argc, char **argv) /* {{{ */
{
	struct {
		char *endpoint;
		char *influxdb;

		int   verbose;
		int   daemonize;

		char *pidfile;
		char *user;
		char *group;

		char *submit_to;
		char *prefix;
		int   schedule;
	} OPTIONS = {
		.verbose   = 0,
		.influxdb  = NULL,
		.endpoint  = strdup("tcp://127.0.0.1:2997"),
		.submit_to = strdup("tcp://127.0.0.1:2999"),
		.daemonize = 1,
		.pidfile   = strdup("/var/run/" ME ".pid"),
		.user      = strdup("root"),
		.group     = strdup("root"),
		.prefix    = NULL, /* will be set later, if needed */
	};

	struct option long_opts[] = {
		{ "help",             no_argument, NULL, 'h' },
		{ "version",          no_argument, NULL, 'V' },
		{ "verbose",          no_argument, NULL, 'v' },
		{ "endpoint",   required_argument, NULL, 'e' },
		{ "foreground",       no_argument, NULL, 'F' },
		{ "pidfile",    required_argument, NULL, 'p' },
		{ "user",       required_argument, NULL, 'u' },
		{ "group",      required_argument, NULL, 'g' },
		{ "influxdb",   required_argument, NULL, 'H' },
		{ "submit",     required_argument, NULL, 'S' },
		{ "prefix",     required_argument, NULL, 'P' },
		{ 0, 0, 0, 0 },
	};
	for (;;) {
		int idx = 1;
		int c = getopt_long(argc, argv, "h?Vv+e:Fp:u:g:H:S:P:", long_opts, &idx);
		if (c == -1) break;

		switch (c) {
		case 'h':
		case '?':
			printf(ME " v%s\n", BOLO_VERSION);
			printf("Usage: " ME " [-h?FVv] [-e tcp://host:port] [-P PREFIX] [-S tcp://host:port]\n"
			       "                [-u user] [-g group] [-p /path/to/pidfile]\n\n");
			printf("Options:\n");
			printf("  -?, -h               show this help screen\n");
			printf("  -F, --foreground     don't daemonize, stay in the foreground\n");
			printf("  -V, --version        show version information and exit\n");
			printf("  -v, --verbose        turn on debugging, to standard error\n");
			printf("  -e, --endpoint       bolo broadcast endpoint to connect to\n");
			printf("  -P, --prefix         prefix for submitting perf metrics back\n");
			printf("  -S, --submit         bolo listener endpoint for perf metrics\n");
			printf("  -H, --influxdb       host:port of the influxdb to submit to\n");
			printf("  -u, --user           user to run as (if daemonized)\n");
			printf("  -g, --group          group to run as (if daemonized)\n");
			printf("  -p, --pidfile        where to store the pidfile (if daemonized)\n");
			exit(0);

		case 'V':
			printf(ME " v%s\n"
			       "Copyright (c) 2016 The Bolo Authors.  All Rights Reserved.\n",
			       BOLO_VERSION);
			exit(0);

		case 'v':
			OPTIONS.verbose++;
			break;

		case 'e':
			free(OPTIONS.endpoint);
			OPTIONS.endpoint = strdup(optarg);
			break;

		case 'F':
			OPTIONS.daemonize = 0;
			break;

		case 'p':
			free(OPTIONS.pidfile);
			OPTIONS.pidfile = strdup(optarg);
			break;

		case 'u':
			free(OPTIONS.user);
			OPTIONS.user = strdup(optarg);
			break;

		case 'g':
			free(OPTIONS.group);
			OPTIONS.group = strdup(optarg);
			break;

		case 'H':
			free(OPTIONS.influxdb);
			OPTIONS.influxdb = strdup(optarg);
			break;

		case 'S':
			free(OPTIONS.submit_to);
			OPTIONS.submit_to = strdup(optarg);
			break;

		case 'P':
			free(OPTIONS.prefix);
			OPTIONS.prefix = strdup(optarg);
			break;

		default:
			fprintf(stderr, "unhandled option flag %#02x\n", c);
			exit(1);
		}
	}

	if (!OPTIONS.influxdb) {
		fprintf(stderr, "Missing required --influxdb parameter.\n");
		exit(1);
	}

	if (!OPTIONS.prefix) {
		char *s = fqdn();
		OPTIONS.prefix = string("%s:sys:" ME, s);
		free(s);
	}

	if (OPTIONS.daemonize) {
		log_open(ME, "daemon");
		log_level(LOG_ERR + OPTIONS.verbose, NULL);

		mode_t um = umask(0);
		if (daemonize(OPTIONS.pidfile, OPTIONS.user, OPTIONS.group) != 0) {
			fprintf(stderr, "daemonization failed: (%i) %s\n", errno, strerror(errno));
			exit(3);
		}
		umask(um);
	} else {
		log_open(ME, "console");
		log_level(LOG_INFO + OPTIONS.verbose, NULL);
	}
	logger(LOG_NOTICE, "starting up");


	void *zmq = zmq_ctx_new();
	if (!zmq) {
		logger(LOG_ERR, "failed to initialize 0MQ context");
		exit(1);
	}

	int rc;
	rc = bolo_subscriber_init();
	if (rc != 0) {
		logger(LOG_ERR, "failed to initialize subscriber architecture");
		exit(2);
	}

	rc = bolo_subscriber_monitor_thread(zmq, OPTIONS.prefix, OPTIONS.submit_to);
	if (rc != 0) {
		logger(LOG_ERR, "failed to spin up monitor thread");
		exit(2);
	}

	rc = bolo_subscriber_scheduler_thread(zmq, 60 * 1000);
	if (rc != 0) {
		logger(LOG_ERR, "failed to spin up scheduler thread");
		exit(2);
	}

	rc = bolo_subscriber_metrics(zmq,
		"SAMPLE", "dispatch.time.s",
		NULL);
	if (rc != 0) {
		logger(LOG_WARNING, "failed to provision metrics");
		/* non-fatal, but still a problem... */
	}

	rc = dispatcher_thread(zmq, OPTIONS.endpoint, OPTIONS.influxdb);
	if (rc != 0) {
		logger(LOG_ERR, "failed to initialize dispatcher: %s", strerror(errno));
		exit(2);
	}

	bolo_subscriber_supervisor(zmq);

	free(OPTIONS.endpoint);

	free(OPTIONS.pidfile);
	free(OPTIONS.user);
	free(OPTIONS.group);

	free(OPTIONS.influxdb);
	free(OPTIONS.submit_to);
	free(OPTIONS.prefix);

	return 0;
}
/* }}} */
