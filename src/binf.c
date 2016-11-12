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

#include "bolo.h"
#include <sys/mman.h>

#define RECORD_TYPE_MASK  0x000f
#define RECORD_TYPE_STATE    0x1
#define RECORD_TYPE_COUNTER  0x2
#define RECORD_TYPE_SAMPLE   0x3
#define RECORD_TYPE_EVENT    0x4
#define RECORD_TYPE_RATE     0x5

typedef struct PACKED {
	uint32_t  magic;
	uint16_t  version;
	uint16_t  flags;
	uint32_t  timestamp;
	uint32_t  count;
} binf_header_t;

typedef struct PACKED {
	uint16_t  len;
	uint16_t  flags;
} binf_record_t;

typedef struct PACKED {
	uint32_t  last_seen;
	 uint8_t  status;
	 uint8_t  stale;
	 uint8_t  ignore;
} binf_state_t;

typedef struct PACKED {
	uint32_t  last_seen;
	uint64_t  value;
	 uint8_t  ignore;
} binf_counter_t;

typedef struct PACKED {
	uint32_t  last_seen;
	uint64_t  n;
	double    min;
	double    max;
	double    sum;
	double    mean;
	double    mean_;
	double    var;
	double    var_;
	 uint8_t  ignore;
} binf_sample_t;

typedef struct PACKED {
	uint32_t first_seen;
	uint32_t last_seen;
	uint64_t first;
	uint64_t last;
	 uint8_t ignore;
} binf_rate_t;

typedef struct PACKED {
	uint32_t  timestamp;
} binf_event_t;

#define binf_record_type(b) ((b)->flags & RECORD_TYPE_MASK)

#ifndef htonll
static uint64_t htonll(uint64_t x)
{
	union {
		uint64_t u64;
		uint32_t u32[2];
	} h, n;

	h.u64 = x;
	n.u32[0] = htonl(h.u32[1]);
	n.u32[1] = htonl(h.u32[0]);
	return n.u64;
}
#endif

#ifndef ntohll
static uint64_t ntohll(uint64_t x)
{
	union {
		uint64_t u64;
		uint32_t u32[2];
	} h, n;

	n.u64 = x;
	h.u32[0] = ntohl(n.u32[1]);
	h.u32[1] = ntohl(n.u32[0]);
	return h.u64;
}
#endif

static int s_write_record(void *addr, size_t *len, uint8_t type, void *_)
{
	binf_record_t record;
	union {
		binf_state_t   state;
		binf_counter_t counter;
		binf_sample_t  sample;
		binf_event_t   event;
		binf_rate_t    rate;
	} body;
	union {
		void      *unknown;
		state_t   *state;
		counter_t *counter;
		sample_t  *sample;
		event_t   *event;
		rate_t    *rate;
	} payload;
	const char *s;

	payload.unknown = _;

	record.len   = sizeof(record);
	record.flags = type;
	switch (type) {
	case RECORD_TYPE_STATE:
		record.len += sizeof(body.state);
		record.len += strlen(payload.state->name)    + 1;
		record.len += strlen(payload.state->summary) + 1;
		break;

	case RECORD_TYPE_COUNTER:
		record.len += sizeof(body.counter);
		record.len += strlen(payload.counter->name) + 1;
		break;

	case RECORD_TYPE_SAMPLE:
		record.len += sizeof(body.sample);
		record.len += strlen(payload.sample->name) + 1;
		break;

	case RECORD_TYPE_EVENT:
		record.len += sizeof(body.event);
		record.len += strlen(payload.event->name)  + 1;
		record.len += strlen(payload.event->extra) + 1;
		break;

	case RECORD_TYPE_RATE:
		record.len += sizeof(body.rate);
		record.len += strlen(payload.rate->name) + 1;
		break;

	default:
		return -1;
	}

	#define _cpybin(addr,obj,idx,size) \
		memcpy(addr + idx, obj, size); \
		idx += size;

	record.len   = htons(record.len);
	record.flags = htons(type);

	memcpy(addr + *len, &record, sizeof(record));
	*len += sizeof(record);

	switch (type) {
	case RECORD_TYPE_STATE:
		body.state.last_seen = htonl(payload.state->last_seen);
		body.state.status    = payload.state->status;
		body.state.stale     = payload.state->stale;
		body.state.ignore    = payload.state->ignore;

		_cpybin(addr, &body.state, *len, sizeof(body.state))

		s = payload.state->name;
		_cpybin(addr, s, *len, strlen(s) + 1)

		s = payload.state->summary;
		_cpybin(addr, s, *len, strlen(s) + 1)

		break;

	case RECORD_TYPE_COUNTER:
		body.counter.last_seen = htonl(payload.counter->last_seen);
		body.counter.value     = htonll(payload.counter->value);
		body.counter.ignore    = payload.counter->ignore;

		_cpybin(addr, &body.counter, *len, sizeof(body.counter))

		s = payload.counter->name;
		_cpybin(addr, s, *len, strlen(s) + 1)

		break;

	case RECORD_TYPE_SAMPLE:
		body.sample.last_seen = htonl(payload.sample->last_seen);
		body.sample.n         = htonll(payload.sample->n);
		body.sample.min       = htonl(payload.sample->min);
		body.sample.max       = htonl(payload.sample->max);
		body.sample.sum       = htonl(payload.sample->sum);
		body.sample.mean      = htonl(payload.sample->mean);
		body.sample.mean_     = htonl(payload.sample->mean_);
		body.sample.var       = htonl(payload.sample->var);
		body.sample.var_      = htonl(payload.sample->var_);
		body.sample.ignore    = payload.sample->ignore;

		_cpybin(addr, &body.sample, *len, sizeof(body.sample))

		s = payload.sample->name;
		_cpybin(addr, s, *len, strlen(s) + 1)

		break;

	case RECORD_TYPE_EVENT:
		body.event.timestamp = htonl(payload.event->timestamp);

		_cpybin(addr, &body.event, *len, sizeof(body.event))

		s = payload.event->name;
		_cpybin(addr, s, *len, strlen(s) + 1)

		s = payload.event->extra;
		_cpybin(addr, s, *len, strlen(s) + 1)

		break;

	case RECORD_TYPE_RATE:
		body.rate.first_seen = htonl(payload.rate->first_seen);
		body.rate.last_seen  = htonl(payload.rate->last_seen);
		body.rate.first      = htonll(payload.rate->first);
		body.rate.last       = htonll(payload.rate->last);
		body.rate.ignore     = payload.rate->ignore;

		_cpybin(addr, &body.rate, *len, sizeof(body.rate))

		s = payload.rate->name;
		_cpybin(addr, s, *len, strlen(s) + 1)

		break;

	default:
		return -1;
	}

	#undef _cpybin
	return 0;
}

static int s_read_record(void *addr, size_t *len, uint8_t *type, void **r)
{
	binf_record_t record;
	union {
		binf_state_t   state;
		binf_counter_t counter;
		binf_sample_t  sample;
		binf_event_t   event;
		binf_rate_t    rate;
	} body;
	union {
		void      *unknown;
		state_t   *state;
		counter_t *counter;
		sample_t  *sample;
		event_t   *event;
		rate_t    *rate;
	} payload;
	ssize_t want;
	char *buf, *p;

	memcpy(&record, addr + *len, sizeof(record));
	*len += sizeof(record);

	record.len   = ntohs(record.len);
	record.flags = ntohs(record.flags);
	if (type)
		*type = binf_record_type(&record);

	switch (binf_record_type(&record)) {
	case RECORD_TYPE_STATE:
		payload.state = calloc(1, sizeof(state_t));
		if (!payload.state)
			return 1;

		memcpy(&body.state, addr + *len, sizeof(body.state));
		*len += sizeof(body.state);

		payload.state->last_seen = ntohl(body.state.last_seen);
		payload.state->status    = body.state.status;
		payload.state->stale     = body.state.stale;
		payload.state->ignore    = body.state.ignore;

		want = record.len - sizeof(record) - sizeof(body.state);
		buf = vmalloc(want+1);

		memcpy(buf, addr + *len, want);
		*len += want;
		buf[want] = '\0';
		for (p = buf; *p++; );
		if (p - buf + 1 == want) {
			free(payload.state);
			return 1;
		}
		payload.state->name    = strdup(buf);
		payload.state->summary = strdup(p);
		free(buf);

		*r = payload.state;
		return 0;

	case RECORD_TYPE_COUNTER:
		payload.counter = calloc(1, sizeof(counter_t));
		if (!payload.counter)
			return 1;

		memcpy(&body.counter, addr + *len,  sizeof(body.counter));
		*len += sizeof(body.counter);

		payload.counter->last_seen = ntohl(body.counter.last_seen);
		payload.counter->value     = ntohll(body.counter.value);
		payload.counter->ignore    = body.counter.ignore;

		want = record.len - sizeof(record) - sizeof(body.counter);
		buf = vmalloc(want+1);

		memcpy(buf, addr + *len, want);
		*len += want;
		buf[want] = '\0';
		payload.counter->name = strdup(buf);
		free(buf);

		*r = payload.counter;
		return 0;

	case RECORD_TYPE_SAMPLE:
		payload.sample = calloc(1, sizeof(sample_t));

		memcpy(&body.sample, addr + *len, sizeof(body.sample));
		*len += sizeof(body.sample);

		payload.sample->last_seen = ntohl(body.sample.last_seen);
		payload.sample->n         = ntohll(body.sample.n);
		payload.sample->min       = ntohl(body.sample.min);
		payload.sample->max       = ntohl(body.sample.max);
		payload.sample->sum       = ntohl(body.sample.sum);
		payload.sample->mean      = ntohl(body.sample.mean);
		payload.sample->mean_     = ntohl(body.sample.mean_);
		payload.sample->var       = ntohl(body.sample.var);
		payload.sample->var_      = ntohl(body.sample.var_);
		payload.sample->ignore    = body.sample.ignore;

		want = record.len - sizeof(record) - sizeof(body.sample);
		buf = vmalloc(want+1);

		memcpy(buf, addr + *len, want);
		*len += want;
		buf[want] = '\0';
		payload.sample->name = strdup(buf);
		free(buf);

		*r = payload.sample;
		return 0;

	case RECORD_TYPE_EVENT:
		payload.event = calloc(1, sizeof(event_t));
		if (!payload.event)
			return 1;

		memcpy(&body.event, addr + *len, sizeof(body.event));
		*len += sizeof(body.event);

		payload.event->timestamp = ntohl(body.event.timestamp);
		want = record.len - sizeof(record) - sizeof(body.event);
		buf = vmalloc(want+1);

		memcpy(buf, addr + *len, want);
		*len += want;
		buf[want] = '\0';
		for (p = buf; *p++; );
		if (p - buf + 1 == want) {
			free(payload.event);
			free(buf);
			return 1;
		}
		payload.event->name  = strdup(buf);
		payload.event->extra = strdup(p);
		free(buf);

		*r = payload.event;
		return 0;

	case RECORD_TYPE_RATE:
		payload.rate = calloc(1, sizeof(rate_t));
		if (!payload.rate)
			return 1;

		memcpy(&body.rate, addr + *len, sizeof(body.rate));
		*len += sizeof(body.rate);

		payload.rate->first_seen = ntohl(body.rate.first_seen);
		payload.rate->last_seen  = ntohl(body.rate.last_seen);
		payload.rate->first      = ntohll(body.rate.first);
		payload.rate->last       = ntohll(body.rate.last);
		payload.rate->ignore     = body.rate.ignore;

		want = record.len - sizeof(record) - sizeof(body.rate);
		buf = vmalloc(want+1);

		memcpy(buf, addr + *len, want);
		*len += want;
		buf[want] = '\0';
		for (p = buf; *p++; );
		if (p - buf + 1 == want) {
			free(payload.rate);
			free(buf);
			return 1;
		}
		payload.rate->name = strdup(buf);
		free(buf);

		*r = payload.rate;
		return 0;

	default:
		return 1;
	}

	return 1;
}

int binf_write(db_t *db, const char *file, int db_size)
{
	binf_header_t header;

	state_t   *state;
	counter_t *counter;
	sample_t  *sample;
	event_t   *event;
	rate_t   *rate;

	char *name;
	void *addr;
	int i;
	size_t so_far = 0;

	int fd = open(file, O_RDWR|O_CREAT|O_TRUNC, 0640);
	if (fd < 0) {
		logger(LOG_ERR, "kernel failed to open save file %s for writing: %s",
				file, strerror(errno));
		return -1;
	}

	logger(LOG_NOTICE, "saving state db to %s", file);

	memset(&header, 0, sizeof(header));
	memcpy(&header.magic, "BOLO", 4);
	header.version   = htons(1);
	header.flags     = 0;
	header.timestamp = htonl((uint32_t)time_s());

	header.count = 0;
	for_each_key_value(&db->states,   name, state)   header.count++;
	for_each_key_value(&db->counters, name, counter) header.count++;
	for_each_key_value(&db->samples,  name, sample)  header.count++;
	for_each_object(event, &db->events, l)           header.count++;
	for_each_key_value(&db->rates,    name, rate)    header.count++;
	header.count = htonl(header.count);

	if((lseek(fd, db_size * 1024 * 1024, SEEK_SET)) == -1) {
		logger(LOG_ERR, "failed to seek to the end of the savedb: %s, error: %s", file, strerror(errno));
		close(fd);
		return -1;
	}
	if((ftruncate(fd, db_size * 1024 * 1024)) == -1) {
		logger(LOG_ERR, "failed to write to save file %s end: %s", file, strerror(errno));
		close(fd);
		return -1;
	}
	addr = mmap(NULL, db_size * 1024 * 1024, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED) {
		logger(LOG_ERR, "failed to allocate mmap %s, for writing: %s", file, strerror(errno));
		close(fd);
		return -1;
	}

	logger(LOG_INFO, "writing %i bytes for the binary file header", sizeof(header));
	memcpy(addr + so_far, &header, sizeof(header));
	so_far += sizeof(header);

	i = 1;
	for_each_key_value(&db->states, name, state) {
		s_write_record(addr, &so_far, RECORD_TYPE_STATE, state);
		logger(LOG_INFO, "wrote bytes for state record #%i (%s), index = %i", i, name, so_far);
		i++;
	}
	for_each_key_value(&db->counters, name, counter) {
		s_write_record(addr, &so_far, RECORD_TYPE_COUNTER, counter);
		logger(LOG_INFO, "wrote bytes for counter record #%i (%s), index = %i", i, name, so_far);
		i++;
	}
	for_each_key_value(&db->samples, name, sample) {
		s_write_record(addr, &so_far, RECORD_TYPE_SAMPLE, sample);
		logger(LOG_INFO, "wrote bytes for sample record #%i (%s), index = %i", i, name, so_far);
		i++;
	}
	event_t *ev;
	for_each_object(ev, &db->events, l) {
		s_write_record(addr, &so_far, RECORD_TYPE_EVENT, ev);
		logger(LOG_INFO, "wrote bytes for event record #%i (%s), index = %i", i, name, so_far);
		i++;
	}
	for_each_key_value(&db->rates, name, rate) {
		s_write_record(addr, &so_far, RECORD_TYPE_RATE, rate);
		logger(LOG_INFO, "wrote bytes for rate record #%i (%s), index = %i", i, name, so_far);
		i++;
	}
	memcpy(addr + so_far, "\0\0", 2);

	logger(LOG_INFO, "done writing savefile %s", file);
	close(fd);
	munmap(addr, db_size * 1024 * 1024);
	return 0;
}

int binf_read(db_t *db, const char *file, int db_size)
{
	binf_header_t header;
	union {
		void      *unknown;
		state_t   *state;
		counter_t *counter;
		sample_t  *sample;
		event_t   *event;
		rate_t    *rate;
	} payload, found;
	unsigned int i;
	uint8_t type;
	void *addr;
	size_t so_far = 0;

	int fd = open(file, O_RDONLY);
	if (fd < 0) {
		logger(LOG_ERR, "kernel failed to open %s for reading: %s",
			file, strerror(errno));
		return -1;
	}
	addr = mmap(NULL,
		    db_size * 1024 * 1024,
		    PROT_READ, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED) {
		logger(LOG_ERR, "failed to allocate mmap %s, for reading: %s", file, strerror(errno));
		close(fd);
		return -1;
	}

	logger(LOG_NOTICE, "reading state db from savefile %s", file);
	memcpy(&header, addr, sizeof(header));
	so_far += sizeof(header);

	if (memcmp(&header.magic, "BOLO", 4) != 0) {
		logger(LOG_ERR, "%s does not seem to be a bolo savefile", file);
		close(fd);
		munmap(addr, db_size * 1024 * 1024);
		return -1;
	}

	header.version   = ntohs(header.version);
	header.flags     = ntohs(header.flags);
	header.timestamp = ntohl(header.timestamp);
	header.count     = ntohl(header.count);

	logger(LOG_NOTICE, "%s is a v%i database, dated %lu, and contains %u records",
			file, header.version, header.timestamp, header.count);

	if (header.version != 1) {
		logger(LOG_ERR, "%s is a v%u savefile; this version of bolo only supports v1 files",
			file, header.version);
		close(fd);
		munmap(addr, db_size * 1024 * 1024);
		return -1;
	}

	for (i = 1; i <= header.count; i++) {
		logger(LOG_INFO, "reading record #%i from savefile", i);

		if (s_read_record(addr, &so_far, &type, &payload.unknown) != 0) {
			logger(LOG_ERR, "%s: failed to read all of record #%i", file, i);
			close(fd);
			munmap(addr, db_size * 1024 * 1024);
			return -1;
		}

		switch (type) {
		case RECORD_TYPE_STATE:
			if ((found.state = find_state(db, payload.state->name)) != NULL) {
				free(found.state->summary);
				found.state->summary   = payload.state->summary;
				found.state->last_seen = payload.state->last_seen;
				found.state->status    = payload.state->status;
				found.state->stale     = payload.state->stale;
				found.state->ignore    = payload.state->ignore;

			} else {
				logger(LOG_INFO, "state %s not found in configuration, skipping", payload.state->name);
				free(payload.state->summary);
			}

			free(payload.state->name);
			free(payload.state);
			payload.state = NULL;
			break;

		case RECORD_TYPE_COUNTER:
			if ((found.counter = find_counter(db, payload.counter->name)) != NULL) {
				found.counter->last_seen = payload.counter->last_seen;
				found.counter->value     = payload.counter->value;
				found.counter->ignore    = payload.counter->ignore;

			} else {
				logger(LOG_INFO, "counter %s not found in configuration, skipping", payload.counter->name);
			}

			free(payload.counter->name);
			free(payload.counter);
			payload.counter = NULL;
			break;

		case RECORD_TYPE_SAMPLE:
			if ((found.sample = find_sample(db, payload.sample->name)) != NULL) {
				found.sample->last_seen = payload.sample->last_seen;
				found.sample->n         = payload.sample->n;
				found.sample->min       = payload.sample->min;
				found.sample->max       = payload.sample->max;
				found.sample->sum       = payload.sample->sum;
				found.sample->mean      = payload.sample->mean;
				found.sample->mean_     = payload.sample->mean_;
				found.sample->var       = payload.sample->var;
				found.sample->var_      = payload.sample->var_;
				found.sample->ignore    = payload.sample->ignore;

			} else {
				logger(LOG_INFO, "sample %s not found in configuration, skipping", payload.sample->name);
			}

			free(payload.sample->name);
			free(payload.sample);
			payload.sample = NULL;
			break;

		case RECORD_TYPE_EVENT:
			list_push(&db->events, &payload.event->l);
			break;

		case RECORD_TYPE_RATE:
			if ((found.rate = find_rate(db, payload.rate->name)) != NULL) {
				found.rate->first_seen = payload.rate->first_seen;
				found.rate->last_seen  = payload.rate->last_seen;
				found.rate->first      = payload.rate->first;
				found.rate->last       = payload.rate->last;
				found.rate->ignore     = payload.rate->ignore;
			} else {
				logger(LOG_INFO, "rate %s not found in configuration, skipping", payload.rate->name);
			}
			free(payload.rate->name);
			free(payload.rate);
			payload.rate = NULL;
			break;

		default:
			logger(LOG_ERR, "unknown record type %02x found!", type);
			close(fd);
			munmap(addr, db_size * 1024 * 1024);
			return 1;
		}
	}
	char trailer[2] = { 1 };
	memcpy(trailer, addr + so_far, 2);
	if (trailer[0] || trailer[1]) {
		logger(LOG_ERR, "no savefile trailer found!");
		close(fd);
		munmap(addr, db_size * 1024 * 1024);
		return 1;
	}

	logger(LOG_INFO, "done reading savefile %s", file);
	close(fd);
	munmap(addr, db_size * 1024 * 1024);
	return 0;
}

int binf_sync(const char *file, int db_size)
{
	int fd = open(file, O_RDWR);
	if (fd < 0) {
		logger(LOG_ERR, "kernel failed to open %s for reading: %s",
			file, strerror(errno));
		return -1;
	}

	void *addr = mmap(NULL, db_size * 1024 * 1024, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED) {
		logger(LOG_ERR, "failed to allocate mmap %s for syncing: %s", file, strerror(errno));
		close(fd);
		return -1;
	}
	if((msync(addr, db_size * 1024 * 1024, MS_ASYNC)) == -1) {
		logger(LOG_ERR, "failed to sync mmap %s: %s", file, strerror(errno));
		close(fd);
		munmap(addr, db_size * 1024 * 1024);
		return -1;
	}
	logger(LOG_NOTICE, "successfully synced state %s to disk", file);
	close(fd);
	munmap(addr, db_size * 1024 * 1024);
	return 0;
}
