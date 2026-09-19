#!/usr/bin/env python3
"""adsb_control - run one of this package's graphs as a session of a GNU Radio 4 control plane.

The receiving machine runs a gr4cp_server; this runs anywhere and drives it. It reads a graph file,
posts it as a session, starts it, reads named blocks' settings back while the session runs, and
stops and deletes the session on the way out.

    python3 tools/adsb_control.py --server http://p25.local:20081 \\
        --graph graphs/adsb-rtl.yaml --seconds 60 --show packet_sink

The requests it makes:

    POST   /sessions                                 the graph file becomes a session
    POST   /sessions/{id}/start                      the graph runs
    GET    /sessions/{id}                            the session's state
    GET    /sessions/{id}/blocks/{name}/settings     a block's settings and counters, by graph name
    POST   /sessions/{id}/stop, DELETE /sessions/{id}   the end of the session

The control plane starts and stops the receiver; it does not carry its output, so this script
prints no frames. The frames leave the receiving machine as packets on the graph's own ZeroMQ
socket, which tools/adsb_listen.py and graphs/adsb-printer.yaml read.

A block's settings stop being readable at the moment its session's graph asks to stop, while the
control plane still calls the session running, so this script keeps and reports the last good read
of each block.

Python 3 and nothing else. Exit 0 when the run finished, 1 when the server refused or did not
answer, 2 when the command line or the graph file was unusable.
"""

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request


class ServerError(Exception):
    """A request the server refused, or did not answer."""


def parse(text):
    """A reply body as JSON where it is JSON, and as itself where it is not."""
    if not text:
        return None
    try:
        return json.loads(text)
    except ValueError:
        return text


def error_text(status, payload):
    """The server's message for a failed request, or the status where it sent none."""
    if isinstance(payload, dict) and isinstance(payload.get("error"), dict):
        failure = payload["error"]
        return "%s: %s" % (failure.get("code", status), failure.get("message", ""))
    if isinstance(payload, str) and payload:
        return "%s: %s" % (status, payload.strip())
    return "the server answered %s" % status


class Server:
    """The control plane's REST API over urllib."""

    def __init__(self, base):
        self.base = base.rstrip("/")

    def call(self, method, path, body=None, timeout=30.0):
        """One request, as the status and whatever came back parsed.

        A server that answered, whatever it answered, comes back as a status. A server that did not
        answer at all raises, because "refused the request" and "was not there" are different things
        for a caller to report.
        """
        url = self.base + path
        data = None if body is None else json.dumps(body).encode("utf-8")
        request = urllib.request.Request(url, data=data, method=method)
        if data is not None:
            request.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(request, timeout=timeout) as reply:
                return reply.status, parse(reply.read().decode("utf-8"))
        except urllib.error.HTTPError as failure:
            return failure.code, parse(failure.read().decode("utf-8"))
        except Exception as failure:  # noqa: BLE001 - every transport failure reads the same way here
            raise ServerError("%s %s: the server did not answer: %s" % (method, url, failure))

    def ok(self, method, path, body=None, timeout=30.0):
        """One request whose failure is this program's failure, reported in the server's words."""
        status, payload = self.call(method, path, body, timeout)
        if status >= 300:
            raise ServerError("%s %s: %s" % (method, path, error_text(status, payload)))
        return payload


class Session:
    """One session: the graph it was made from, and what its blocks say while it runs."""

    RETRIES = 3

    def __init__(self, server, name, graph):
        self.server = server
        self.name = name
        self.graph = graph
        self.id = None
        self.last = {}
        self.misses = {}

    def create(self):
        created = self.server.ok("POST", "/sessions", {"name": self.name, "grc": self.graph})
        self.id = created["id"]
        return self.id

    def start(self):
        self.server.ok("POST", "/sessions/%s/start" % self.id)
        return self.server.ok("GET", "/sessions/%s" % self.id)

    def describe(self):
        return self.server.ok("GET", "/sessions/%s" % self.id)

    def read(self, block):
        """One block's settings by the name the graph file gave it, kept where the server answered."""
        if self.misses.get(block, 0) >= self.RETRIES:
            return self.last.get(block)
        try:
            status, payload = self.server.call("GET", "/sessions/%s/blocks/%s/settings" % (self.id, block))
        except ServerError:
            status, payload = 0, None
        if status == 0 or status >= 300:
            self.misses[block] = self.misses.get(block, 0) + 1
            return self.last.get(block)
        self.last[block] = payload.get("settings", {})
        self.misses[block] = 0
        return self.last[block]

    def stop(self):
        self.quietly("POST", "/sessions/%s/stop")

    def remove(self):
        self.quietly("DELETE", "/sessions/%s")

    def quietly(self, method, path):
        """The end of a session, asked for on the way out of a run that may itself have failed."""
        if self.id is None:
            return
        try:
            self.server.call(method, path % self.id)
        except ServerError:
            pass


def show_settings(block, settings):
    """One block's settings, one `block: key = value` line each, in key order."""
    if not settings:
        print("%s: the server did not answer for this block" % block)
        return
    for key in sorted(settings):
        print("%s: %s = %s" % (block, key, settings[key]))
    sys.stdout.flush()


def run(options):
    server = Server(options.server)
    with open(options.graph, "r", encoding="utf-8") as handle:
        document = handle.read()
    if not document.strip():
        sys.stderr.write("adsb_control: %s is empty\n" % options.graph)
        return 2

    name = options.name or os.path.splitext(os.path.basename(options.graph))[0]
    session = Session(server, name, document)
    try:
        identifier = session.create()
        print("session %s from %s" % (identifier, options.graph))
        described = session.start()
        print("state %s" % described.get("state"))
        sys.stdout.flush()

        deadline = None if options.seconds <= 0.0 else time.monotonic() + options.seconds
        while deadline is None or time.monotonic() < deadline:
            time.sleep(1.0)
            for block in options.show:
                session.read(block)
        for block in options.show:
            show_settings(block, session.last.get(block))
    except ServerError as failure:
        sys.stderr.write("adsb_control: %s\n" % failure)
        return 1
    except KeyboardInterrupt:
        print("interrupted; stopping the session")
    finally:
        session.stop()
        session.remove()
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description="Run one of this package's graphs as a session of a GNU Radio 4 control plane.")
    parser.add_argument("--server", default="http://127.0.0.1:20081", help="the control plane's address (default http://127.0.0.1:20081)")
    parser.add_argument("--graph", required=True, help="the graph file to run as a session, e.g. graphs/adsb-rtl.yaml")
    parser.add_argument("--name", default=None, help="the session's name (default: the graph file's base name)")
    parser.add_argument("--seconds", type=float, default=0.0, help="how long the session runs; the default runs until interrupted")
    parser.add_argument("--show", action="append", default=[], metavar="BLOCK",
                        help="print this block's settings when the run ends, by the name the graph file gave it; repeatable")
    return run(parser.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
