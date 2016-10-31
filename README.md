bolo - A Versatile Monitoring Toolkit
=====================================

[![Travis](https://img.shields.io/travis/bolo/bolo.svg)](https://travis-ci.org/bolo/bolo)

bolo is a toolkit for building distributed, scalable monitoring
systems.  It provides a set of small, versatile components that
you can plug together in new and exciting ways, to gather and
aggregate metrics, mine data and track system states.

Getting Started
---------------

To compile the software, use the standard incantation:

    $ ./bootstrap
    $ ./configure
    $ make
    $ make check           # optional, but recommended
    $ sudo make install

This will compile and install all of the bolo components in
standard systems places.  bolo requires libzmq3, pthreads, librrd,
libpcre3 and [libvigor][libvigor] to run.  It uses [ctap][ctap] to
run automated tests (which are, again, highly recommended).

A Minimal System
----------------

The core of every bolo deployment is the `bolo` daemon itself,
which requires a configuration file, usually **/etc/bolo.conf**.

    # /etc/bolo.conf
    #
    listener   tcp://*:2999
    controller tcp://127.0.0.1:2998
    broadcast  tcp://*:2997

    log info daemon
    savefile /var/lib/bolo/save.db
    keysfile /var/lib/bolo/keys.db

    dumpfiles /var/tmp/bolo.%s

    type :generic {
      freshness 60
      critical "no result from monitored thing"
    }
    state :generic m/./

    window @minutely 60
    use @minutely
    sample  m/./
    counter m/./
    rate    m/./

This is a **catch-all** configuration, that expects all submitted
metrics to be collected with at least a 1-minute resolution, and
aggregated at the per-minute mark.  Check **bolo.conf(5)** for
details and more advanced usage.

You'll have to create the bolo user (or adjust how you launch the
bolo daemon):

    $ sudo useradd -rUd /var/lib/bolo -s /sbin/nologin bolo
    $ sudo mkdir /var/lib/bolo
    $ sudo chown -R bolo:bolo /var/lib/bolo

With this in place, start up the bolo daemon:

    $ sudo bolo

By default, bolo starts up daemonized, and reads from
/etc/bolo.conf.

Submitting Data
---------------

The **bolo send** utility can be used to submit metrics and state
data to bolo, via the _listener_ port (TCP/2999 in our case):

    $ bolo send -e tcp://localhost:2999 -t sample life 42
    $ bolo send -e tcp://localhost:2999 -t sample universe 42
    $ bolo send -e tcp://localhost:2999 -t sample everything 42

At this point, you will want to have [bolo-collectors][collectors]
installed to give you some real metric collectors, for things like
memory usage, swap rates, CPU time allocation, etc.  The output
from these collectors can be piped directly into **bolo send**:

    $ /path/to/collector | bolo send -e tcp://localhost:2999

With collectors installed, you can also run **dbolo**, the
distributed bolo agent.  Create the _/etc/dbolo.conf_
configuration file:

    # /etc/dbolo.conf
    @10s /usr/lib/bolo/collectors/linux
    @1h  /usr/lib/bolo/collectors/hostinfo

And then launch dbolo as a standalone daemon:

    $ sudo dbolo

Now you've got a small agent process running and submitting
Linux-y performance data every 10s, and host metadata every hour.

Using the Data
--------------

As the bolo daemon receives metrics and state data from monitored
hosts, it aggregates the data and regularly broadcasts these
summaries out to connected subscribers.  One such subscriber is
**bolo2rrd**, which listens for samples, counters and rate data
and creates / updates RRD files on disk.

    $ sudo mkdir /srv/rrd
    $ sudo bolo2rrd -r /srv/rrd -e tcp://localhost:2997

bolo2rrd daemonizes into the background.

The **bolo2log** utility can be run in foreground mode, where it
will dump broadcast data it receives in real-time.  This can be
useful for subscriber prototyping in languages like Perl or Ruby,
without having to deal directly with 0MQ and the bolo message
protocol.

    $ bolo2log -Fvvve tcp://localhost:2997 | grep 'universe'

Writing subscribers isn't that difficult, and provides an
interesting way of customizing the behavior of your monitoring
system without needing to modify bolo itself.

Notes On Production Environments
--------------------------------

If you're into [BOSH][bosh] for your infrastructure, we have a
[bolo-boshrelease][boshrel] you can check out!

If you are operating inside of a more traditional server-centric
environment, running on either Debian / Ubuntu or CentOS, you can
take advantage of pre-built packages for bolo and its dependencies,
hosted on [packagecloud][pkgcloud].

For Debian/Ubuntu:

    $ curl -s https://packagecloud.io/install/repositories/bolo/bolo/script.deb.sh | sudo bash
    $ sudo apt-get install bolo dbolo bolo-collectors

For CentOS:

    $ curl -s https://packagecloud.io/install/repositories/bolo/bolo/script.rpm.sh | sudo bash
    $ sudo yum install bolo dbolo bolo-collectors

Next Steps
----------

Congratulations!  You have a bolo-based monitoring system up.

So where do you go from here?

You'll probably want to build a user-interface to display graphs
rendered from the RRD files, even if that's just to render the
images via cron and serve them up statically.



[ctap]:       https://github.com/jhunt/ctap
[libvigor]:   https://github.com/jhunt/libvigor
[collectors]: https://github.com/bolo/bolo-collectors
[boshrel]:    https://github.com/bolo/bolo-boshrelease

[bosh]:       http://bosh.io
[pkgcloud]:   http://packagecloud.io
