#!/usr/bin/env python3
"""adsb_listen - read the decoded frames off the receiver's ZeroMQ socket and print one line each.

The small reader at the other end of graphs/adsb-capture.yaml and graphs/adsb-rtl.yaml. Where
graphs/adsb-printer.yaml is the rich view -- a whole GNU Radio 4 graph that renders the same lines the
one-machine application prints -- this is the proof that the packets are readable by a program that is
not GNU Radio 4 at all: sixty lines of struct.unpack against the published header table.

    python3 tools/adsb_listen.py --endpoint tcp://127.0.0.1:5560 --seconds 30

One line per frame: the time the frame sits at in the receiver's stream, the frame's octets in hex, and
every metadata key the decoder wrote, which is where the ICAO address, the downlink format, the message
type and whatever that type carried arrive.

The wire is gr::network::PacketEnvelope, which GNU Radio 4's own PacketEnvelope.hpp states field by
field: four ZeroMQ frames -- the subscription topic, a 32-byte little-endian header, a YAML metadata
document, and the payload. Nothing below imports a GNU Radio module; the header is a fixed table of
fixed-width little-endian fields and is readable from that table alone, which is the point of it.

pyzmq is the one requirement and there is no way around it: ZeroMQ is a wire protocol (ZMTP) over the
socket, not a datagram format, so a raw socket would have to implement the handshake and the framing.
Install it with `pip install pyzmq`, or on Fedora `sudo dnf install python3-zmq`, on Debian and Ubuntu
`sudo apt install python3-zmq`.
"""

import argparse
import binascii
import struct
import sys
import time
import zlib

try:
    import zmq
except ImportError:
    sys.stderr.write("adsb_listen: pyzmq is needed to read a ZeroMQ socket; see the note at the top of this file\n")
    sys.exit(3)

MAGIC = b"GR4P"
HEADER_BYTES = 32
WIRE_VERSION = 1
LITTLE_ENDIAN = 0
YAML_PMT = 1
CRC32_RESIDUE = 0x2144DF1C  # CRC-32/ISO-HDLC over an intact header, its own check word included

# gr::pmt::ValueType's numeric codes, as PacketEnvelope.hpp's itemSizeOf restates them so that a
# reader needs no framework header. Only the sizes are needed here; the payload is bytes either way.
ITEM_SIZES = {2: 1, 3: 2, 4: 4, 5: 8, 6: 1, 7: 2, 8: 4, 9: 8, 10: 4, 11: 8, 12: 8, 13: 16}
ITEM_UINT8 = 6

# The keys the ADS-B decoder writes, in the order a line reads best. Anything else the record carries is
# printed after them in the order the metadata document lists it, which is lexicographic.
LEADING_KEYS = ("protocol", "mode_s_format", "mode_s_icao", "adsb_type_code", "adsb_message", "adsb_callsign")
HIDDEN_KEYS = ("signal_name", "signal_quantity", "signal_unit", "signal_min", "signal_max")


class Refused(Exception):
    """A message that is not an envelope this reader implements, in the words the refusal is named by."""


def parse_header(frame):
    """The header's fields, or a refusal naming the first rule the message broke."""
    if len(frame) != HEADER_BYTES:
        raise Refused("header_bytes")
    if frame[:4] != MAGIC:
        raise Refused("magic")
    (version, order, header_bytes, item_type, item_size, encoding,
     item_count, payload_bytes, meta_bytes, flags, _crc) = struct.unpack_from("<HBBHBBIIIII", frame, 4)
    if version != WIRE_VERSION:
        raise Refused("wire_version %d" % version)
    if order != LITTLE_ENDIAN:
        raise Refused("byte_order")
    if header_bytes != HEADER_BYTES:
        raise Refused("header_bytes")
    if zlib.crc32(frame) != CRC32_RESIDUE:
        raise Refused("header_crc")
    if flags != 0:
        raise Refused("flags")
    if encoding != YAML_PMT:
        raise Refused("meta_encoding %d" % encoding)
    if ITEM_SIZES.get(item_type) != item_size:
        raise Refused("unsupported_item_type %d" % item_type)
    if item_count * item_size != payload_bytes:
        raise Refused("length_mismatch")
    return {"item_type": item_type, "item_size": item_size, "item_count": item_count,
            "payload_bytes": payload_bytes, "meta_bytes": meta_bytes}


def parse_metadata(frame):
    """The top-level keys of the YAML metadata document, as `gr::pmt::yaml` writes one.

    An entry is `key: <tag> <value>`, unindented, and a string carries an empty tag and so reads with two
    spaces and a quoted value. A nested value -- which no key this receiver writes uses -- is left out
    rather than half read, and its key is reported with an empty value so the omission is visible.
    """
    values = {}
    for line in frame.decode("utf-8", "replace").splitlines():
        if not line or line[0].isspace() or line.lstrip().startswith("#"):
            continue
        key, separator, rest = line.partition(":")
        if not separator:
            continue
        key = key.strip()
        rest = rest.strip()
        if rest.startswith("!!"):
            tag, _, text = rest.partition(" ")
            values[key] = read_tagged(tag, text.strip())
        elif rest[:1] in ('"', "'") and len(rest) >= 2 and rest[-1] == rest[0]:
            values[key] = unquote(rest)
        elif rest in ("", "[", "{"):
            values[key] = ""
        else:
            values[key] = rest
    return values


def read_tagged(tag, text):
    """One tagged scalar as the nearest Python value, and as its own text where the tag is not a number."""
    if tag == "!!bool":
        return text == "true"
    if tag == "!!null":
        return None
    if tag == "!!str":
        return unquote(text)
    if tag.startswith("!!uint") or tag.startswith("!!int"):
        try:
            return int(text)
        except ValueError:
            return text
    if tag.startswith("!!float"):
        try:
            return float(text)
        except ValueError:
            return text
    return text


def unquote(text):
    """A quoted scalar's own characters, with the escapes `gr::pmt::yaml` writes read back."""
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "\"'":
        text = text[1:-1]
    return text.replace('\\"', '"').replace("\\n", "\n").replace("\\t", "\t").replace("\\\\", "\\")


def show(value):
    """One metadata value as a line reads it: a float at the precision the document carries."""
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, float):
        return "%g" % value
    if value is None:
        return "null"
    return str(value)


def stream_time(meta):
    """Seconds into the receiver's stream the frame started, from the two keys the framer writes."""
    start = meta.get("sample_start")
    rate = meta.get("sample_rate")
    if not isinstance(start, (int, float)) or not isinstance(rate, (int, float)) or not rate > 0:
        return None
    return float(start) / float(rate)


def line_of(header, meta, payload):
    """The line one packet prints: the time, the octets, and every key the decoder wrote."""
    seconds = stream_time(meta)
    fields = []
    for key in LEADING_KEYS:
        if key in meta:
            fields.append("%s %s" % (key, show(meta[key])))
    for key in sorted(meta):
        if key in LEADING_KEYS or key in HIDDEN_KEYS:
            continue
        fields.append("%s %s" % (key, show(meta[key])))
    octets = binascii.hexlify(payload).decode("ascii").upper()
    if header["item_type"] != ITEM_UINT8:
        octets = "%s (item_type %d)" % (octets, header["item_type"])
    return "%s  %s  %s" % ("     -" if seconds is None else "%10.3f" % seconds, octets, "  ".join(fields))


def listen(endpoint, topic, seconds, count):
    """Subscribe, read until the bound is reached, and return what was counted."""
    context = zmq.Context.instance()
    socket = context.socket(zmq.SUB)
    socket.setsockopt(zmq.SUBSCRIBE, topic.encode("utf-8"))
    socket.setsockopt(zmq.RCVHWM, 64)
    socket.setsockopt(zmq.LINGER, 0)
    socket.connect(endpoint)
    sys.stderr.write("adsb_listen: subscribed to %s, topic %s\n" % (endpoint, topic if topic else "(every)"))
    sys.stderr.flush()

    packets = 0
    refused = 0
    deadline = None if seconds <= 0.0 else time.monotonic() + seconds
    try:
        while True:
            if deadline is not None:
                left = deadline - time.monotonic()
                if left <= 0.0:
                    break
                if not socket.poll(int(left * 1000.0)):
                    continue
            message = socket.recv_multipart()
            try:
                if len(message) != 4:
                    raise Refused("frame_count %d" % len(message))
                header = parse_header(message[1])
                if len(message[2]) != header["meta_bytes"] or len(message[3]) != header["payload_bytes"]:
                    raise Refused("length_mismatch")
                meta = parse_metadata(message[2]) if header["meta_bytes"] else {}
            except Refused as why:
                refused += 1
                sys.stderr.write("adsb_listen: refused a message: %s\n" % why)
                sys.stderr.flush()
                continue
            packets += 1
            print(line_of(header, meta, message[3]))
            sys.stdout.flush()
            if count and packets >= count:
                break
    except KeyboardInterrupt:
        pass
    finally:
        socket.close()
    return packets, refused


def main(argv):
    parser = argparse.ArgumentParser(description="Print the decoded ADS-B frames a GNU Radio 4 receiver publishes over ZeroMQ.")
    parser.add_argument("--endpoint", default="tcp://127.0.0.1:5560",
                        help="the receiver's ZeroMQ endpoint (default tcp://127.0.0.1:5560); the receiving machine's address goes here when the two halves are on two machines")
    parser.add_argument("--topic", default="adsb", help="the subscription prefix the receiver publishes under (default adsb); an empty string takes everything")
    parser.add_argument("--seconds", type=float, default=0.0, help="stop after this many seconds; without it the reader runs until interrupted")
    parser.add_argument("--count", type=int, default=0, help="stop after this many packets")
    options = parser.parse_args(argv)

    packets, refused = listen(options.endpoint, options.topic, options.seconds, options.count)
    sys.stderr.write("adsb_listen: %d packet(s), %d refused\n" % (packets, refused))
    return 0 if packets > 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
