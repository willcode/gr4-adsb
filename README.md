# gr4-adsb — an ADS-B receiver on GNU Radio 4

A 1090 MHz Mode S / ADS-B receiver: one console application over GNU Radio 4's own blocks. It
decodes the extended squitter — aircraft identification, barometric altitude, position and
velocity — and prints one line per frame.

```
     0.544  DF17  ADD376  veloc   gs 352 kt  track 85 deg  vs -1792 ft/min
     0.563  DF17  ADD376  airpos  alt 11550 ft  lat 42.56886  lon -71.58014
     1.897  DF17  ADD376  ident   callsign JBU134
     2.287  DF17  ADD376  tc 28 (aircraft_status)  [JBU134]
```

The columns are the time in seconds from the frame's position in the stream, the downlink format,
the ICAO address, what the message said, and the aircraft identification in brackets once an
earlier frame from that address has given one.

## Quick start

The package builds against an installed GNU Radio 4; see [Building](#building) for what that has
to include.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=<gnuradio4-prefix>
cmake --build build
```

Replay a recorded capture — interleaved 16-bit I/Q in host byte order, which is what most SDR
recorders write:

```
./build/app/adsb --file capture.cs16 --rate 2e6
```

Or open a radio. Give the rate alone first: a device that is fussy about its parameter set is most
likely to open this way.

```
./build/app/adsb --device "driver=rtlsdr"        --rate 2e6 --gain 40
./build/app/adsb --device "driver=hackrf"        --rate 2e6 --gain 100
./build/app/adsb --device "driver=sdrplay"       --rate 6e6
./build/app/adsb --device "driver=uhd,type=b200" --rate 4e6
```

Once a device streams, add what it needs: the RTL-SDR a gain or its AGC, the HackRF a high gain, and
on a device whose default filter is narrower than the pulses, the analog bandwidth (see Running). On a
small computer add `--phase-steps 1 --coarse-ratio 1` as well: the framer's sub-sample phase search
(see How it works) costs more than a Raspberry Pi 3 core has at 2 MS/s, and with it off the chain
keeps up.

## How it works

```
  source ──► PpmFramer<complex<float>> ──► ModeSDecode ──► AdsbPrinter
                                                                │
                                                                ▼
                                                           ConsoleSink

  source, live:  SoapySource<complex<float>>
  source, file:  BasicFileSource<int16_t> ──► InterleavedToComplex
```

**`PpmFramer`** reads frames out of the sample stream. Pulse-position modulation puts one pulse
per bit period, in the first half for a one and the second for a zero, so a bit is decided by
comparing the two halves of one period against each other: there is no carrier to acquire and no
clock to recover. The framer nominates a position wherever the preamble's four pulses stand out
from the gaps by at least `--threshold`, reads the frame there, and checks the 24-bit parity. Only
a frame whose parity checks consumes its own window; every other nomination advances the search by
one sample, so a false nomination cannot hide a real frame behind it.

**`ModeSDecode`** reads the downlink message into typed metadata and leaves the frame's octets as
the payload, so nothing the frame said is lost. **`AdsbPrinter`** renders one line of text per
record and publishes it as a record of its own. Those three are GNU Radio 4's own blocks, from its
adsb family. **`ConsoleSink`** is defined in `app/` and writes the lines to stdout;
it is the only block this package supplies and the only one in the chain that assumes a console.

**The sample rate must be an even integer multiple of 1 MHz.** A Mode S slot is half a
microsecond, so the rate has to make a slot a whole number of samples. The framer refuses any other
rate rather than resampling silently: 2, 4, 6 and 8 MS/s are all fine, 2.4 MS/s is not. Higher is
better here, and 2 MS/s is a weak floor — expect fewer frames and many more parity failures than at
4 or 6 MS/s from the same antenna. An RTL-SDR tops out below 4 MS/s, so 2 MS/s is its only rate.

**The framer decides each position at the sub-sample phase of its preamble.** A burst does not
arrive on the sample clock, and a pulse that straddles two samples splits its energy between a pulse
slot and a gap slot — the gap slots being what the shape test measures the pulses against, so a bad
sample phase lowers the numerator and raises the denominator at once. Each position therefore passes
a cheap test at integer positions first, at `--threshold` times `--coarse-ratio`, and every
candidate is then measured again at `--phase-steps` offsets spanning one sample period, on the
signal interpolated from the complex samples. The best offset has to reach the full `--threshold`,
and it decides the bits as well; it travels on the record as `sample_phase`, so a frame's position
in the stream is `sample_start + sample_phase`. This is why the framer takes complex samples rather
than magnitudes: a magnitude stream does not carry what the interpolation reads.

The search helps most where a pulse is oversampled, from about two samples a slot up, because there
the interpolation has something to work with. At one sample a slot the gain is smaller and depends
on the receiver's own front end. It costs interpolation work on every candidate the coarse test
passes, and the noise floor sets how many that is; raise `--coarse-ratio` if a run cannot keep up.
`--phase-steps 1` turns the search off and decides every position at the integer sample.

**Analog bandwidth matters more than sample rate.** A Mode S pulse is 0.5 µs and needs several MHz
of analog bandwidth to keep its edges. A receiver whose front end defaults to a rate-derived filter
(1536 kHz is a common one) smears the pulses together and admits nothing at any sample rate. If a
run reports nominations but no admitted frames, widen the analog filter before changing anything
else.

## Running

`--device` and `--file` are exclusive and one of them is required. `--device` takes the SoapySDR
device string any other Soapy program takes; the application splits it into the two settings the
block wants, so a bare `--device hackrf` works too.

| option | default | what it sets |
|---|---|---|
| `--device STR` | — | SoapySDR device string |
| `--file PATH` | — | replay a capture instead of opening a device |
| `--frequency HZ` | 1090e6 | center frequency |
| `--rate HZ` | 4e6 | sample rate, an even multiple of 1 MHz |
| `--bandwidth HZ` | device | analog RF bandwidth |
| `--gain DB` | device | receive gain |
| `--agc on\|off` | off | ask for the device's own AGC |
| `--threshold R` | 2 | preamble nomination ratio, greater than one |
| `--phase-steps N` | 8 | offsets per candidate; 1 turns the search off |
| `--coarse-ratio R` | 0.5 | share of the threshold for the integer test |
| `--seconds N` | 0 | stop after N seconds; 0 runs to the end |
| `--verbose` | off | list GNU Radio 4's registry keys, then run |
| `--plugin-dir DIR` | built in | where `--verbose` loads from |

Exit codes are 0 on a clean stop, 2 on a bad command line and 1 on a graph error, each with the
reason on stderr. Every run prints its counters at exit: samples, candidates, nominations, admitted
frames, parity failures and short formats. Nominations without admitted frames mean the frames are
arriving damaged, which is a bandwidth or gain problem rather than a threshold one.

A file replay is not paced: it reads as fast as the disk delivers, so the printed timestamps are
stream positions rather than wall-clock times.

**`--gain`, `--bandwidth` and `--agc` are asked for only when given.** Left off, the device keeps
its own gain, its own bandwidth for the rate and its own gain mode. The application prints at start
what it asked the device for and what it left alone, so a run that fails to start says which was
which.

**Leave `--gain` off on an SDRplay.** `--gain` is SoapySDR's overall gain, and the SDRplay driver
(SoapySDRPlay3) has no such thing: it exposes two gain-reduction elements, `IFGR` (20 to 59 dB,
ignored while the driver's AGC is on, which it is by default) and `RFGR`, the LNA state. SoapySDR
spreads an overall gain across those elements, and a request near 40 dB lands the highest LNA state;
above 1 GHz an RSP1A has fewer states than that, and the device then refuses the whole activation
with `Init() failed: sdrplay_api_Fail`. Rate, frequency and bandwidth alone stream cleanly. 6 MS/s
with 6 MHz of bandwidth, and 4 MS/s with 5 MHz, are good operating points on an RSP1A.

**On an RTL-SDR give a gain, or `--agc on`.** The dongle's own default is 0 dB, which hears nothing;
`--gain 40` is a working figure. With a fixed tuner gain the 8-bit samples can still sit low in their
range, so that gap slots and bit decisions rest on one or two quantization steps; `--agc on` turns on
the RTL2832's digital gain as well as the tuner's and lifts the signal into the converter's range.
`--agc off` is the default and asks for nothing; the block only ever turns AGC on, so there is no way
from here to force it off on a device that enables it by itself. An SDRplay runs its AGC by default,
which suits ADS-B.

**A HackRF needs `--gain 100` or thereabouts.** The driver opens the radio with 16 dB on the LNA and
16 dB on the VGA, which leaves the noise floor below one step of the 8-bit converter, so nothing is
heard; at 40 dB still nothing, at 70 dB only the nearest aircraft. `--gain` is SoapySDR's overall
gain and the HackRF driver splits it across the amplifier, the LNA and the VGA — 100 dB lands as
LNA 31, AMP 14, VGA 55 — and 110 dB overloads the front end. Leave `--bandwidth` to the driver unless
a measurement says otherwise.

**`--verbose` opens every shared block library in the plugin directory through `gr::PluginLoader`
and prints the registry keys they add.** It names each library and how many registrations it
carries, then lists the keys, sorted; among them are
`gr::blocks::adsb::PpmFramer<complex<float32>>`, `gr::blocks::adsb::ModeSDecode` and
`gr::blocks::adsb::AdsbPrinter`, the three blocks this chain builds directly. Each block is entered
under its type name and under its alias, so a library adds fewer keys than it reports
registrations. The chain itself never uses the registry, so the keys printed are exactly what the
libraries brought, and the rest of the run is unchanged. `--plugin-dir` overrides the directory; its
default is the plugin directory of the GNU Radio 4 this build was configured against,
`<prefix>/lib/gnuradio-4/plugins` or `lib64` on a distribution that puts libraries there.

## Two machines

The receiver and the display do not have to be the same computer. `adsb` is one process with the
whole chain in it; the same chain also exists as graph files, cut in two at the point where a frame
has been decoded, so the radio can sit on a small machine near the antenna and the reading can
happen at a desk.

```
  near the antenna                                        at the desk
  ───────────────────────────────────────────────         ──────────────────────────────────────
  radio ─► PpmFramer ─► ModeSDecode ─► DataSetToPacket     ZmqPacketSource ─► PacketToDataSet
                                            │                      ▲                │
                                            ▼                      │                ▼
                                       ZmqPacketSink ═══════ tcp:5560 ═══════┘   AdsbPrinter
```

**The decode happens at the radio and crosses the network once.** What travels is the decoder's own
record — the frame's octets and the typed metadata it read out of them, the ICAO address, the
message type, the altitude, the position — so the far end renders and never re-decodes, and a second
reader of the same socket sees exactly what the first one sees. The samples do not travel: 2 MS/s of
complex floats is 16 MB/s and there is no reason to carry it.

### The four graphs

| file | where it runs | what it is |
|---|---|---|
| `graphs/adsb-hackrf.yaml` | at the antenna | the capture half on a HackRF, which is the better of the two radios here |
| `graphs/adsb-rtl.yaml` | at the antenna | the same on an RTL-SDR, which is what most people have |
| `graphs/adsb-capture.yaml` | anywhere | the same reading the recorded capture instead of a radio, for trying the split with no radio at all |
| `graphs/adsb-printer.yaml` | at the desk | the printer half, which prints the lines the one-process application prints |

**The gain decides whether the receiver hears anything, and the two radios want very different
figures.** A HackRF at 1090 MHz needs far more gain than it does lower down: nothing at 40 dB, the
nearest aircraft at 70, and the sky at 100 — which SoapyHackRF splits as LNA 31, AMP 14, VGA 55. An
RTL-SDR's own default is 0 dB, which also hears nothing; 40 dB is its working figure, and on the same
antenna it hears far less than the HackRF. Hence the order: the HackRF graph is the one to run, and
the RTL graph is kept because the dongle is common.

**The two framer settings differ from the application's defaults, and the reason is the Raspberry
Pi 3's processor.** `adsb` defaults to `--phase-steps 8 --coarse-ratio 0.5`: each candidate position
is re-tested at eight sub-sample offsets, which finds frames whose preamble falls between two
samples and is worth having at 2 MS/s. On a Pi 3 it is not affordable — with it the chain runs at a
fifth of real time and the radio overflows. The graphs therefore carry `phase_steps: 1` and
`coarse_ratio: 1.0`, the fine search off, at which the Pi keeps up at the full rate. On a machine
with the cycles, put 8 and 0.5 back.

### Running the two halves

Either half is run by `gnuradio_4_0_graph_run`, GNU Radio 4's own runner of a graph file, which is
installed with the framework. It names no block, no setting and no chain — the file names the
blocks, the plugin directories supply them — so it runs any graph file, not only these four. This
package builds no runner of its own.

```
on the machine with the radio, where /opt/gr4-adsb holds this package and /opt/gnuradio4 the SDK:
    /opt/gnuradio4/bin/gnuradio_4_0_graph_run --graph /opt/gr4-adsb/graphs/adsb-hackrf.yaml

at the desk, with <receiver> edited into the endpoint of graphs/adsb-printer.yaml, from the
GNU Radio 4 this package was built against:
    gnuradio_4_0_graph_run --graph graphs/adsb-printer.yaml
```

Its own usage text names the options:

```
  --graph <file>     the graph to run, in the GRC YAML dialect; - reads it from standard input
  --plugin-dir <dir> a directory to load plugins and block libraries from; repeatable
  --seconds <s>      stop the graph after <s> seconds; without it the run ends when the graph does
  --show <name>      print the settings of the block named <name> when the run ends; repeatable
  --verbose          list what each plugin directory loaded and the keys it brought
  --help, -h         this text
```

`--show packet_sink` on the capture half and `--show packet_source` on the printer half are the two
worth having: each prints that block's settings and its counters when the run ends. Exit status is
the same as `adsb`'s — 0 on a clean stop, 2 on a command line that cannot be used and 1 on a graph
that could not be read, loaded or run — and SIGINT and SIGTERM stop a run as `--seconds` does.

**Neither command needs `--plugin-dir`.** The tool always searches the plugin directory of its own
installation, which is where every block these graphs name lives, and
`GNURADIO4_PLUGIN_DIRECTORIES`, the colon-separated list the framework's plugin loader reads, adds
more. That is the difference from a runner built inside this package, which compiled in the
directory CMake found the SDK at and so had to be told the real one on any machine it had been
cross-built for. `adsb --verbose` still has that dependency, because it is this package's own
program; see its `--plugin-dir` in Running.

The printer prints the same lines `adsb` prints, because it is the same `AdsbPrinter`. `to_stdout`
is what makes it print: the block publishes its line as a record, and the application's own
`ConsoleSink` — which writes those records to a terminal — is not a registered block and so cannot
be named from a graph file.

**Start the reading end first.** A ZeroMQ publisher discards what it sends before a subscriber has
connected, so a receiver started first will have published its first frames into an empty room.

### The small script

`tools/adsb_listen.py` is the other reader of the same socket, and the demonstration that the
packets are readable by something that is not GNU Radio 4:

```
python3 tools/adsb_listen.py --endpoint tcp://<receiver>:5560
```

`--topic` is the subscription prefix (`adsb`, as the graphs publish it); `--seconds N` or `--count N`
bounds the run, and without either it reads until interrupted. One line per frame: the time in the
receiver's stream, the frame's octets in hex, and every metadata key the decoder wrote.

```
     2.767  8DAA1BAA592E206EF883BEB1DE82  protocol mode_s/adsb  mode_s_format 17  mode_s_icao AA1BAA
            adsb_type_code 11  adsb_message airborne_position  adsb_altitude_ft 3600  ...  crc_ok true
```

It needs `pyzmq` and nothing else — `pip install pyzmq`, or `dnf install python3-zmq`, or `apt
install python3-zmq`. There is no way to do without it: ZeroMQ is a wire protocol over the socket,
not a datagram format, so a raw socket would have to implement its handshake and framing. What the
script does implement is the envelope inside it, `gr::network::PacketEnvelope`: four frames — the
subscription topic, a 32-byte little-endian header whose first four bytes are the magic `GR4P` and
whose last four are a CRC over the header itself, a YAML metadata document, and the payload. GNU
Radio 4's own `PacketEnvelope.hpp` states that header field by field and is the reference the script
was written from.

Both readers can run at once. The sink publishes; any number of subscribers take what it publishes.

### Under a control plane

The reading end does not have to reach into the receiving machine to start it. `gr4cp_server`, the
GNU Radio 4 control plane, runs sessions from graph files it is given over HTTP, so the receiving
machine runs one server and nothing else, and the desk decides what it runs and when.

```
on the machine with the radio:
    GR4CP_PORT=20081 /opt/gr4cp/bin/gr4cp_server

at the desk:
    gnuradio_4_0_graph_run --graph graphs/adsb-printer.yaml   # started first, as always
    python3 tools/adsb_control.py --server http://<receiver>:20081 \
        --graph graphs/adsb-hackrf.yaml --seconds 60 --show packet_sink
```

`adsb_control.py` posts the graph file as a session (`--name` names it; the default is the file's
base name), starts it, reads the settings and counters of each `--show` block when the run ends, and
stops and deletes the session on the way out; without `--seconds` the session runs until the script
is interrupted. It needs Python 3 and nothing else, because all it sends is text.

The server takes its port from `GR4CP_PORT` and listens on every interface, so a machine on an
untrusted network needs a firewall rule on that port and on 5560. Its block directories are compiled
in and `GNURADIO4_PLUGIN_DIRECTORIES` overrides them; the one these graphs need is the SDK's own,
`/opt/gnuradio4/lib/gnuradio-4/plugins` on a machine installed as above, because every block they
name is GNU Radio 4's and this package installs no block library. **The server and the block
libraries it loads have to come from one GNU Radio 4 prefix**: a plugin built against another one
puts two builds of the framework in one process, and the catalog that reads the plugin's blocks
either skips them or stops the server.

The control plane starts and stops the receiver; it does not carry its output. The frames still
leave on the graph's own socket, to the printer half or to `adsb_listen.py`.

## Building

Requirements:

- **GNU Radio 4**, core, library and blocks installed into one prefix, with the library's
  pulse-position framing scanner and the blocks project's `adsb` family (`PpmFramer`, `ModeSDecode`
  and `AdsbPrinter`).
- **The SDR family**, for live devices. The blocks project builds and exports `gnuradio4::gr-sdr`
  only when `GR4_ENABLE_SDR` is on, and that is off by default. Without it this package still
  configures and builds, and `adsb` offers `--file` alone. The configure line says which of the two
  this build got.
- **SoapySDR** and a driver for the radio, again only for live devices.
- **The network family** (`GR4_ENABLE_NETWORK` on, with libzmq and the cppzmq header), for the
  two-machine graphs alone: the one-process `adsb` does not use it.
- **`gnuradio_4_0_graph_run`**, the framework's own runner of a graph file, again for the
  two-machine graphs alone. It is installed with GNU Radio 4, in the same prefix; this package
  builds no runner.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=<gnuradio4-prefix> \
      -DCMAKE_INSTALL_PREFIX=<install-prefix>
cmake --build build
cmake --install build
```

`-DGR4ADSB_WARNINGS_AS_ERRORS=OFF` relaxes `-Werror`. The install writes `adsb` to `<prefix>/bin`
and nothing else; the blocks the chain is built from, and the runner the graph files are run by, are
installed with GNU Radio 4.

The package ships no test. What a test of this receiver would exercise — the framing, the message
decode and the rendering of a line — is tested in GNU Radio 4 beside each of those blocks, and what
is left here is the application and `ConsoleSink`. A replay over a recorded capture is how a build
is checked end to end, and a capture large enough to carry frames is too large to ship.

## Layout

`app/` holds `adsb` alone: `adsb.cpp`, the one-process application and `ConsoleSink` together.
Everything else in the chain belongs to GNU Radio 4 and is used as installed, and so is the runner
the graph files are run by.

`graphs/` holds the same chain as graph files, cut in two at the decoded frame, and `tools/` the two
Python programs that go with them — a reader of the receiver's socket and a client of a control
plane. Neither directory is compiled and neither is installed by the build; they are text. A graph
file is read by `gnuradio_4_0_graph_run` or by a control plane, and the Python programs by
`python3`.

`adsb` names every block type and builds its chain with `emplaceBlock<T>`, the direct, statically
checked route, which is what a program that knows its own chain needs; `--verbose` is the only place
it opens the registry. The registry is for a program that does not know its chain, and
`gnuradio_4_0_graph_run` is that one: it builds every graph out of the keys the plugin directories
brought, which is why one program runs either half of the split and needs no rebuild when a graph
file changes. That program belongs to GNU Radio 4 rather than to this package, because nothing in it
is about ADS-B.

## Next

Two components are not built or wired here, because they are not specified yet. The attachment point
for each is:

- **A map display**, on the decoded record stream: `ModeSDecode`'s output, ahead of `AdsbPrinter`,
  where position, altitude, callsign and velocity are typed metadata on the record and nothing has
  to be parsed back out of a rendered line.
- **A waterfall**, on the sample stream ahead of `PpmFramer`, the last point where every sample is
  present and still complex. A spectrum display taps the same place; a magnitude display puts its
  own `gr::blocks::basic::Abs<std::complex<float>>` on that tap rather than in the framer's path.

Adding either takes one more `emplaceBlock` and one more `connect` at the point named; the existing
chain does not change.

## License

MIT. See [LICENSE](LICENSE).
