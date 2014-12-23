#include "test.h"

TESTS {
	alarm(5);
	server_t svr;
	int rc;
	void *z;
	pthread_t tid;

	mkdir("t/tmp", 0755);
	memset(&svr, 0, sizeof(svr));
	svr.config.broadcast = "inproc://bcast";
	svr.config.dumpfiles = "t/tmp/dump.%s";
	unlink(svr.config.savefile = "t/tmp/save");

	CHECK(svr.zmq = zmq_ctx_new(),
		"failed to create a new 0MQ context");
	CHECK(pthread_create(&tid, NULL, kernel, &svr) == 0,
		"failed to spin up kernel thread");
	CHECK(z = zmq_socket(svr.zmq, ZMQ_DEALER),
		"failed to create mock kernel test socket");
	CHECK(zmq_connect(z, KERNEL_ENDPOINT) == 0,
		"failed to connect to kernel socket");
	sleep_ms(50);

	/* ----------------------------- */

	pdu_t *p;
	uint32_t time = time_s();

	/* save state (to /t/tmp/save) */
	p = pdu_make("SAVESTATE", 1, "test");
	rc = pdu_send_and_free(p, z);
	is_int(rc, 0, "sent [SAVESTATE] PDU to kernel");

	p = pdu_recv(z);
	isnt_null(p, "received reply PDU from kernel");
	is_string(pdu_type(p), "OK", "kernel replied with a [SAVESTATE]");
	pdu_free(p);

	char s[16];
	memcpy(s, "BOLO\0\1\0\0....\0\0\0\0", 16);
	*(uint32_t*)(s+4+2+2) = htonl(time);
	binfile_is("t/tmp/save", s, 16, "empty save file");

	/* ----------------------------- */
	pthread_cancel(tid);
	zmq_close(z);
}
