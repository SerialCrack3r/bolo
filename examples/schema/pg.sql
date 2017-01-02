/*
	########   #######  ##        #######           #######          ########   ######
	##     ## ##     ## ##       ##     ##         ##     ##         ##     ## ##    ##
	##     ## ##     ## ##       ##     ##                ##         ##     ## ##
	########  ##     ## ##       ##     ## #######  #######  ####### ########  ##   ####
	##     ## ##     ## ##       ##     ##         ##                ##        ##    ##
	##     ## ##     ## ##       ##     ##         ##                ##        ##    ##
	########   #######  ########  #######          #########         ##         ######

	Postgres schema for bolo state data.

 */
DROP TABLE IF EXISTS states;
DROP TABLE IF EXISTS states_staging;
DROP TABLE IF EXISTS history;
DROP TABLE IF EXISTS history_anomalies;
DROP TABLE IF EXISTS datapoints;

DROP FUNCTION IF EXISTS reconcile();
DROP FUNCTION IF EXISTS track_datapoint(metric, text, timestamp);

DROP TYPE IF EXISTS status;
DROP TYPE IF EXISTS metric;

DROP TABLE IF EXISTS events;
DROP INDEX IF EXISTS events_idx;
-- --------------------------------------------------------------------

CREATE TYPE status AS ENUM ('OK', 'WARNING', 'CRITICAL', 'UNKNOWN');

-- --------------------------------------------------------------------

CREATE TABLE states_staging (
	name         TEXT NOT NULL,
	status       status NOT NULL,
	message      TEXT,

	inserted_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
	occurred_at  TIMESTAMP NOT NULL
);

CREATE TABLE states (
	name    TEXT NOT NULL UNIQUE,
	status  status NOT NULL,
	message TEXT,

	first_seen TIMESTAMP NOT NULL,
	last_seen  TIMESTAMP NOT NULL,

	CHECK (last_seen >= first_seen)
);

CREATE TABLE history (
	name    TEXT NOT NULL,
	status  status NOT NULL,
	message TEXT,

	started_at           TIMESTAMP NOT NULL,
	tentative_ended_at   TIMESTAMP NOT NULL,
	ended_at             TIMESTAMP DEFAULT NULL,

	UNIQUE (name, started_at),

	CHECK (tentative_ended_at >= started_at),
	CHECK (ended_at IS NULL OR ended_at >= started_at)
);

CREATE TABLE history_anomalies (
	name         TEXT NOT NULL,
	status       status NOT NULL,
	message      TEXT,

	inserted_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
	occurred_at  TIMESTAMP NOT NULL
);

CREATE OR REPLACE FUNCTION reconcile() RETURNS INTEGER AS $$
DECLARE
	st   RECORD;
	cur  RECORD;
	prev RECORD;
	n    INTEGER;
BEGIN
	n := 0;

	RAISE NOTICE 'Importing state data from staging tables...';
	FOR st IN SELECT * FROM states_staging ORDER BY occurred_at, name LIMIT 500 LOOP

		n := n + 1;

		/*
			 ######  ########    ###    ######## ########  ######
			##    ##    ##      ## ##      ##    ##       ##    ##
			##          ##     ##   ##     ##    ##       ##
			 ######     ##    ##     ##    ##    ######    ######
			      ##    ##    #########    ##    ##             ##
			##    ##    ##    ##     ##    ##    ##       ##    ##
			 ######     ##    ##     ##    ##    ########  ######
		 */
		SELECT * INTO cur FROM states WHERE name = st.name;

		/* No existing state record;
		   Create a new one! */
		IF NOT FOUND THEN
			INSERT INTO states (
				name, status, message,
				first_seen, last_seen
			) VALUES (
				st.name, st.status, st.message,
				st.occurred_at, st.occurred_at
			);

		/* Otherwise, if the staging record is newer than the
		   data in the state table (per last_seen), update the
		   state table. */
		ELSIF cur.last_seen < st.occurred_at THEN

			/* Handle status change by reseting first_seen
			   and recording the new status / message. */
			IF cur.status != st.status THEN
				UPDATE states SET
					status     = st.status,
					message    = st.message,
					first_seen = st.occurred_at,
					last_seen  = st.occurred_at
				WHERE
					name = st.name;

			/* Handle continued problem states by updating
			   last_seen and the message only. */
			ELSE
				UPDATE states SET
					last_seen = st.occurred_at,
					message   = st.message
				WHERE
					name = st.name;

			END IF;
		END IF;

		/*
			##     ## ####  ######  ########  #######  ########  ##    ##
			##     ##  ##  ##    ##    ##    ##     ## ##     ##  ##  ##
			##     ##  ##  ##          ##    ##     ## ##     ##   ####
			#########  ##   ######     ##    ##     ## ########     ##
			##     ##  ##        ##    ##    ##     ## ##   ##      ##
			##     ##  ##  ##    ##    ##    ##     ## ##    ##     ##
			##     ## ####  ######     ##     #######  ##     ##    ##
		 */
		SELECT * INTO prev FROM history
			WHERE name = st.name
			  AND started_at <= st.occurred_at
			ORDER BY started_at DESC
			LIMIT 1;

		/* (A)
			<no history>

			             +---------+
			   plus      | WARNING |
			             +---------+

			Insert a new, open history state.

			Keep track of the `tentative_ended_at`, to
			detect window insertion situations.
		 */
		IF NOT FOUND THEN
			INSERT INTO history
				(name, status, message, started_at, tentative_ended_at, ended_at)
				VALUES (st.name, st.status, st.message, st.occurred_at, st.occurred_at, NULL);

		/* (B)
			+---------------
			| WARNING     ...
			+-----------------

			             +---------+
			   plus      | WARNING |
			             +---------+

			Continuation of existing open history state.

			Update the `tentative_ended_at` value.
		 */
		ELSIF prev.ended_at IS NULL
		  AND st.occurred_at > prev.tentative_ended_at
		  AND prev.status = st.status THEN
			UPDATE history
				SET tentative_ended_at = st.occurred_at
				WHERE history = prev;

		/* (C)
			+---------------
			| WARNING     ...
			+-----------------

			             +----------+
			   plus      | CRITICAL |
			             +----------+

			State transition.

			Close out the previously open history state
			(by setting `ended_at` to the `started_at` of the new state)
			and insert a new open history state.
		 */
		ELSIF prev.ended_at IS NULL
		  AND prev.tentative_ended_at < st.occurred_at
		  AND prev.status != st.status THEN
			UPDATE history
				SET           ended_at = st.occurred_at,
				    tentative_ended_at = st.occurred_at
				WHERE history = prev;
			INSERT INTO history
				(name, status, message, started_at, tentative_ended_at, ended_at)
				VALUES (st.name, st.status, st.message, st.occurred_at, st.occurred_at, NULL);

		/*
			Everything else is an anomaly; out-of-order data,
			Strange time boundaries, duplicate records, etc.
		 */
		ELSE
			RAISE INFO 'History anomaly detected; logging for review';

			INSERT INTO history_anomalies
				(name, status, message, inserted_at, occurred_at)
			VALUES (
				st.name, st.status, st.message, st.inserted_at, st.occurred_at);
		END IF;

		-- clear the staging table
		DELETE FROM states_staging WHERE states_staging = st;
	END LOOP;

	RETURN n;
END;
$$
LANGUAGE plpgsql;


CREATE TYPE metric AS ENUM ('SAMPLE', 'RATE', 'COUNTER');

CREATE TABLE datapoints (
	id         SERIAL PRIMARY KEY,
	name       TEXT NOT NULL,
	type       metric NOT NULL,
	first_seen TIMESTAMP NOT NULL,
	last_seen  TIMESTAMP NOT NULL,

	CHECK (last_seen >= first_seen)
);

CREATE FUNCTION track_datapoint(mtype metric, mname text, ts timestamp) RETURNS INTEGER AS $$
DECLARE
	existing RECORD;
BEGIN
	SELECT * INTO existing FROM datapoints WHERE type = mtype AND name = mname;
	IF NOT FOUND THEN
		RAISE NOTICE 'New datapoint detected (%s: %s); inserting', mtype, mname;
		INSERT INTO datapoints
			(name, type, first_seen, last_seen)
			VALUES (mname, mtype, ts, ts);

	ELSE
		RAISE NOTICE 'Updating last_seen of datapoint (%s: %s)', mtype, mname;
		UPDATE datapoints
			SET last_seen = ts
			WHERE datapoints = existing;

	END IF;

	RETURN 0;
END
$$
LANGUAGE plpgsql;

CREATE TABLE events (
	id          SERIAL PRIMARY KEY,
	name        TEXT NOT NULL,
	occurred_at  TIMESTAMP NOT NULL,

	extra       TEXT,

	UNIQUE (name, occurred_at)
);
CREATE INDEX events_idx ON events (name, occurred_at, extra);
