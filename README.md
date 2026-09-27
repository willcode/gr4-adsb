# gr4-adsb — an ADS-B receiver on GNU Radio 4

A 1090 MHz Mode S / ADS-B receiver on GNU Radio 4: one console application, and the three blocks the
1090 MHz chain needs, which this package supplies because GNU Radio 4 carries no ADS-B family. The
receiver decodes the extended squitter (aircraft identification, barometric altitude, position and
velocity) and prints one line per frame.

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

The package builds against an installed GNU Radio 4; see Building for what that has to include.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=<gnuradio4-prefix>
cmake --build build
```

Replay a recorded capture, interleaved 16-bit I/Q in host byte order, the form most SDR recorders
write:

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

Once a device streams, add what it needs: the RTL-SDR a gain or its AGC, the HackRF a high gain,
and on a device whose default filter is narrower than the pulses, the analog bandwidth (see
Running). On a small computer add `--phase-steps 1 --coarse-ratio 1`, which turns the framer's
sub-sample phase search off.

## How it works

```
  source ──► PpmFramer<complex<float>> ──► ModeSDecode ──► AdsbPrinter
                                                                │
                                                                ▼
                                                           ConsoleSink

  source, live:  SoapySource<complex<float>>
  source, file:  BasicFileSource<int16_t> ──► InterleavedToComplex
```

**`PpmFramer`** reads frames out of the sample stream. Pulse-position modulation puts one pulse per
bit period, in the first half for a one and the second for a zero, so a bit is decided by comparing
the two halves of one period against each other: the chain acquires no carrier and recovers no
clock. The framer nominates a position wherever the preamble's four pulses stand out from the
gaps by at least `--threshold`, reads the frame there, and checks the 24-bit parity. Only a frame
whose parity checks consumes its own window; every other nomination advances the search by one
sample, so a false nomination cannot hide a real frame behind it.

**`ModeSDecode`** reads the downlink message into typed metadata and leaves the frame's octets as
the payload, so nothing the frame said is lost. **`AdsbPrinter`** renders one line of text per
record and publishes it as a record of its own. Those three are this package's own, in `blocks/`
and in the namespace `adsb`, built into one block library that a graph runner loads from a plugin
directory. **`ConsoleSink`** is defined in `app/`, writes the lines to stdout, and is the only
block in the chain that assumes a console. It sits outside the library and cannot be named from a
graph file; `AdsbPrinter`'s `to_stdout` prints for a graph instead.

The registry keys, which a graph file names and `adsb --verbose` prints:

- `adsb::PpmFramer<complex<float32>>` and `adsb::PpmFramer<float32>` — read samples or magnitudes
  and publish one record per frame
- `adsb::ModeSDecode` — reads a frame's record and publishes the same octets with typed metadata
- `adsb::AdsbPrinter` — reads a decoded record and publishes one rendered line as a record

Against a core with block labels, each block declares the family `adsb` and its role: the framer
and the decoder `processor`, the printer `sink`. The configure compiles a probe against the core
it finds and says which it got.

They ship in one library, `libAdsbDemoBlocksShared.so`. `blocks/README.md` states the record the
three pass between them, field by field.

**The sample rate must be an even integer multiple of 1 MHz.** A Mode S slot is half a microsecond,
so the rate has to make a slot a whole number of samples. The framer refuses any other rate rather
than resampling silently: 2, 4, 6 and 8 MS/s are all fine, 2.4 MS/s is not. A higher rate decodes
more of what the antenna hears; an RTL-SDR tops out below 4 MS/s, so 2 MS/s is its only rate.

**The framer decides each position at the sub-sample phase of its preamble.** A burst does not
arrive on the sample clock, and a pulse that straddles two samples splits its energy between a
pulse slot and a gap slot, the two the shape test compares. Each position therefore passes a cheap
test at integer positions first, at `--threshold` times `--coarse-ratio`, and every candidate is
then measured again at `--phase-steps` offsets spanning one sample period, on the signal
interpolated from the complex samples. The best offset has to reach the full `--threshold`, and it
decides the bits as well; it travels on the record as `sample_phase`, so a frame's position in the
stream is `sample_start + sample_phase`. The framer therefore takes complex samples rather than
magnitudes: a magnitude stream carries nothing for the interpolation to read. The search costs
interpolation work on every candidate the coarse test passes, so raise `--coarse-ratio` if a run
cannot keep up, and `--phase-steps 1` turns it off.

**Analog bandwidth matters more than sample rate.** A Mode S pulse is 0.5 µs and needs several MHz
of analog bandwidth to keep its edges. A receiver whose front end defaults to a rate-derived filter
(1536 kHz is a common one) smears the pulses together and admits nothing at any sample rate. If a
run reports nominations but no admitted frames, widen the analog filter before changing anything
else.

## Running

`--device` and `--file` are exclusive and one of them is required. `--device` takes the SoapySDR
device string any other Soapy program takes; the application splits it into the two settings the
block takes, so a bare `--device hackrf` works too.

| option | default | what it sets |
|---|---|---|
| `--device STR` | — | SoapySDR device string |
| `--file PATH` | — | replay a capture instead of a device |
| `--frequency HZ` | 1090e6 | center frequency |
| `--rate HZ` | 4e6 | sample rate, an even multiple of 1 MHz |
| `--bandwidth HZ` | device | analog RF bandwidth |
| `--gain DB` | device | receive gain |
| `--agc on\|off` | off | ask for the device's own AGC |
| `--threshold R` | 2 | nomination ratio, greater than one |
| `--phase-steps N` | 8 | offsets per candidate; 1 turns the search off |
| `--coarse-ratio R` | 0.5 | share of the threshold for the integer test |
| `--seconds N` | 0 | stop after N seconds; 0 runs to the end |
| `--verbose` | off | print the registry keys the plugins bring, then run |
| `--plugin-dir DIR` | built in | where `--verbose` loads from; repeatable |

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
spreads an overall gain across those elements, and a request near 40 dB asks for the highest LNA
state; above 1 GHz an RSP1A has fewer states than that, and the device then refuses the whole
activation with `Init() failed: sdrplay_api_Fail`. Rate, frequency and bandwidth alone stream
cleanly. 6 MS/s with 6 MHz of bandwidth, and 4 MS/s with 5 MHz, are good operating points on an
RSP1A.

**On an RTL-SDR give a gain, or `--agc on`.** The dongle's own default of 0 dB picks up nothing;
`--gain 40` is a working figure. With a fixed tuner gain the 8-bit samples can still sit low in
their range, so that gap slots and bit decisions rest on one or two quantization steps; `--agc on`
turns on the RTL2832's digital gain as well as the tuner's and lifts the signal into the
converter's range. `--agc off` is the default and asks for nothing; the block only ever turns AGC
on, so nothing here forces it off on a device that enables it by itself. An SDRplay runs its AGC by
default, which suits ADS-B.

**A HackRF needs `--gain 100` or thereabouts.** The driver opens the radio with 16 dB on the LNA
and 16 dB on the VGA, which leaves the noise floor below one step of the 8-bit converter. `--gain`
is SoapySDR's overall gain and the HackRF driver splits it across the amplifier, the LNA and the
VGA (100 dB lands as LNA 31, AMP 14, VGA 55); 110 dB overloads the front end. Leave `--bandwidth`
to the driver unless a measurement says otherwise.

**`--verbose` opens every shared block library in the plugin directories through
`gr::PluginLoader` and prints the registry keys they add**, then runs. Each block is entered under
its type name and under its alias, so a library adds fewer keys than it reports registrations. Two
directories are compiled in: this package's own plugin directory and the GNU Radio 4 one this build
was configured against, each `<prefix>/lib/gnuradio-4/plugins`, or `lib64` on a distribution that
puts libraries there. An installed `adsb --verbose` therefore needs nothing on its command line; a
run from the build tree needs `--plugin-dir build/blocks`, where the build tree holds the library
instead. `--plugin-dir` replaces both defaults and may be given more than once.

The first of the two follows `CMAKE_INSTALL_PREFIX`. The second is where CMake found the SDK. A
build that installs somewhere other than it runs reads the SDK from elsewhere, and
`-DGR4ADSB_SDK_PLUGIN_DIR=<dir>` names that directory instead; a cross build that finds the SDK in
a staging tree gives the target's own. The configure line prints both.

## Two machines

The receiver and the display do not have to be the same computer. `adsb` is one process with the
whole chain in it; the same chain also exists as graph files, cut in two at the point where a frame
has been decoded, so the radio can sit on a small machine near the antenna and the reading can
happen at a desk.

```
  near the antenna
    radio ─► PpmFramer ─► ModeSDecode ─► DataSetToPacket ─► ZmqPacketSink
                                                                   │
                                                             tcp:5560
                                                                   │
  at the desk                                                      ▼
    AdsbPrinter ◄─ PacketToDataSet ◄─ ZmqPacketSource ◄────────────╯
```

**The decode happens at the radio and crosses the network once.** The decoder's own record travels:
the frame's octets and the typed metadata read out of them, the ICAO address, the message type, the
altitude, the position. The far end renders and never re-decodes, and a second reader of the same
socket sees exactly what the first one sees. The samples stay where they were read, 2 MS/s of
complex floats being 16 MB/s that nothing downstream needs.

| file | where it runs | what it is |
|---|---|---|
| `graphs/adsb-hackrf.yaml` | at the antenna | the capture half on a HackRF |
| `graphs/adsb-rtl.yaml` | at the antenna | the capture half on an RTL-SDR |
| `graphs/adsb-capture.yaml` | anywhere | the same from a recorded capture |
| `graphs/adsb-printer.yaml` | at the desk | the printer half |

`graphs/adsb-capture.yaml` reads `capture.cs16` from the directory the graph is run in; put the
path to a recording of your own in its `file_name`, and give the framer that recording's rate.

`docs/` holds one page per graph file: the blocks it names, their parameters, the connections
between them, and a flowchart of the chain.

**The gain decides whether the receiver picks anything up, and the two radios take very different
figures.** A HackRF at 1090 MHz needs far more gain than it does lower down: about 100 dB, which
SoapyHackRF splits as LNA 31, AMP 14, VGA 55. An RTL-SDR's own default of 0 dB also picks up
nothing; 40 dB is its working figure.

**The two framer settings in the graphs differ from the application's defaults.** `adsb` defaults
to `--phase-steps 8 --coarse-ratio 0.5`: each candidate position is re-tested at eight sub-sample
offsets, which finds frames whose preamble falls between two samples. That costs more than a
machine as small as a Raspberry Pi 3 has at 2 MS/s, so the graphs carry `phase_steps: 1` and
`coarse_ratio: 1.0`, the sub-sample search off. On a machine with the cycles, put 8 and 0.5 back.

`rungraph`, GNU Radio 4's own runner of a graph file, runs either half, and is installed with the
framework. It names no block, no setting and no chain: the file names the blocks and the plugin
directories supply them, so it runs any graph file, not only these four. This package builds no
runner of its own.

```
on the machine with the radio, where /opt/gr4-adsb holds this package and
/opt/gnuradio4 the SDK:
    /opt/gnuradio4/bin/rungraph \
        --graph /opt/gr4-adsb/share/gr4-adsb/graphs/adsb-hackrf.yaml \
        --plugin-dir /opt/gr4-adsb/lib/gnuradio-4/plugins

at the desk, with <receiver> edited into the endpoint of adsb-printer.yaml:
    rungraph --graph <prefix>/share/gr4-adsb/graphs/adsb-printer.yaml \
             --plugin-dir <prefix>/lib/gnuradio-4/plugins
```

Its options are `--graph <file>`, the graph to run in the GRC YAML dialect, with `-` reading it
from standard input; `--plugin-dir <dir>`, repeatable; `--seconds <s>`, a run bound; `--show
<name>`, which prints the settings and counters of the block named `<name>` when the run ends, also
repeatable; `--verbose`; and `--help`. `--show packet_sink` on the capture half and `--show
packet_source` on the printer half are the two worth having. Exit status is the same as `adsb`'s,
and SIGINT and SIGTERM stop a run as `--seconds` does.

**Both commands name one plugin directory, this package's own.** The tool always searches the
plugin directory of its own installation, which holds every block these graphs name but the three
ADS-B ones; those three sit in this package's library, under this package's own prefix, which the
tool does not search. `GNURADIO4_PLUGIN_DIRECTORIES`, the colon-separated list the framework's
plugin loader reads, does the same for a whole shell:

```
export GNURADIO4_PLUGIN_DIRECTORIES=/opt/gr4-adsb/lib/gnuradio-4/plugins
/opt/gnuradio4/bin/rungraph \
    --graph /opt/gr4-adsb/share/gr4-adsb/graphs/adsb-printer.yaml
```

`adsb` needs neither, because both directories it loads from are compiled into it.

The printer half prints the same lines `adsb` prints, because it is the same `AdsbPrinter`.
`to_stdout` makes it print: the block publishes its line as a record, and the application's own
`ConsoleSink` is unregistered and cannot be named from a graph file.

**Start the reading end first.** A ZeroMQ publisher discards what it sends before a subscriber has
connected, so a receiver started first discards its own first frames.

## The two Python programs

`tools/adsb_listen.py` is the other reader of the capture half's socket, for a program that is not
GNU Radio 4:

```
python3 tools/adsb_listen.py --endpoint tcp://<receiver>:5560
```

`--topic` is the subscription prefix (`adsb`, as the graphs publish it); `--seconds N` or
`--count N` bounds the run, and without either it reads until interrupted. One line per frame: the
time in the receiver's stream, the frame's octets in hex, and every metadata key the decoder wrote.

```
     2.767  8DAA1BAA592E206EF883BEB1DE82  protocol mode_s/adsb
            mode_s_format 17  mode_s_icao AA1BAA  adsb_type_code 11
            adsb_message airborne_position  adsb_altitude_ft 3600  crc_ok true
```

It needs `pyzmq` and nothing else: `pip install pyzmq`, or `dnf install python3-zmq`, or `apt
install python3-zmq`. ZeroMQ is a wire protocol over the socket rather than a datagram format, so a
raw socket would have to implement its handshake and framing. The script implements the envelope
inside it, `gr::network::PacketEnvelope`, whose four frames are the subscription topic, a 32-byte
little-endian header whose first four bytes are the magic `GR4P` and whose last four are a CRC over
the header itself, a YAML metadata document, and the payload. GNU Radio 4's own
`PacketEnvelope.hpp` states that header field by field and is the reference the script was written
from.

Both readers can run at once. The sink publishes; any number of subscribers take what it publishes.

`tools/adsb_control.py` runs a graph as a session of `gr4cp_server`, the GNU Radio 4 control plane,
so the receiving machine runs one server and the desk decides what it runs and when.

```
on the machine with the radio:
    export GNURADIO4_PLUGIN_DIRECTORIES=\
    /opt/gnuradio4/lib/gnuradio-4/plugins:/opt/gr4-adsb/lib/gnuradio-4/plugins
    GR4CP_PORT=20081 /opt/gr4cp/bin/gr4cp_server

at the desk, the printer half started first:
    python3 adsb_control.py --server http://<receiver>:20081 \
        --graph adsb-hackrf.yaml --seconds 60 --show packet_sink
```

`adsb_control.py` posts the graph file as a session (`--name` names it; the default is the file's
base name), starts it, reads the settings and counters of each `--show` block when the run ends,
and stops and deletes the session on the way out; without `--seconds` the session runs until the
script is interrupted. It needs Python 3 and nothing else, because it sends only text.

The server takes its port from `GR4CP_PORT` and listens on every interface, so a machine on an
untrusted network needs a firewall rule on that port and on 5560. Its block directories are
compiled in and `GNURADIO4_PLUGIN_DIRECTORIES` overrides them; the command above sets it because
these graphs need two directories, the SDK's own for most of the chain and this package's for the
three ADS-B blocks. **The server and the block libraries it loads have to come from one GNU
Radio 4 prefix**: a plugin built against another one puts two builds of the framework in one
process, and the catalog that reads the plugin's blocks either skips them or stops the server.

The control plane starts and stops the receiver; it does not carry its output. The frames still
leave on the graph's own socket, to the printer half or to `adsb_listen.py`.

## Building

Requirements:

- **GNU Radio 4**, core, library and blocks installed into one prefix, with the library's
  pulse-position framing scanner, `gr::digital::PpmScanner`, which this package's framer runs on,
  and the core's `gr_add_block_library`, which builds `blocks/` into the package's plugin.
- **The SDR family**, for live devices. The blocks project builds and exports `gnuradio4::gr-sdr`
  only when `GR4_ENABLE_SDR` is on, and that is off by default. Without it this package still
  configures and builds, and `adsb` offers `--file` alone. The configure line says which of the two
  this build got.
- **SoapySDR** and a driver for the radio, again only for live devices.
- **The network family** (`GR4_ENABLE_NETWORK` on, with libzmq and the cppzmq header), for the
  two-machine graphs alone: the one-process `adsb` does not use it.
- **`rungraph`**, the framework's own runner of a graph file, again for the two-machine graphs
  alone. It is installed with GNU Radio 4, in the same prefix; this package builds no runner.
- **Boost.UT**, for the block tests alone. GNU Radio 4's own tests are written in it and it is not
  installed with the framework, so this package looks for it the same way the framework's build
  does: an installed `ut` CMake package, or `-DGR_BOOST_UT_INCLUDE_DIR=<dir>` naming a source-built
  copy. Without it the build skips the tests and produces everything else.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=<gnuradio4-prefix> \
      -DCMAKE_INSTALL_PREFIX=<install-prefix>
cmake --build build
cmake --install build
```

`-DGR4ADSB_WARNINGS_AS_ERRORS=OFF` relaxes `-Werror`. The install writes `adsb` to `<prefix>/bin`,
the block library to `<prefix>/lib/gnuradio-4/plugins`, and the graph files and the two Python
programs to `<prefix>/share/gr4-adsb`. The rest of the chain, and the runner the graph files are
run by, are installed with GNU Radio 4.

## Tests

```
ctest --test-dir build
```

Three tests sit one beside each block: the framing, the message decode and the rendering of a
line. Every frame in them is built field by field from the standard rather than taken from a
capture, so none needs a recording or a radio.

Two more tests replay a recorded 1090 MHz capture at 2 MS/s,
`ADSB_20260812_182544_1090000000_2000000_cs16.sigmf-data`. `qa_PpmRecording` runs the
pulse-position scanner over it at four thresholds, and `qa_AdsbRecording` runs the whole chain from
a file source to the printer and checks the three lines the capture decodes to. The capture is too
large to ship. Name the directory holding it at configure time:

```
cmake -S . -B build -DGR4_RECORDINGS_DIR=<dir>
```

Without the capture the two recording tests report a skip. Both carry the label `recording`, so
`ctest --test-dir build -LE recording` leaves them out. The application and `ConsoleSink` stay
uncovered by the tests: a replay with `adsb --file` checks those end to end.

Where the core has block labels, `qa_AdsbLabels` checks the registry key, the family and the role
of each block.

## Layout

`blocks/` holds the three ADS-B blocks: the headers under `include/adsb/`, one test beside each
and the two recording tests in `test/`, and the pulse-position scanner's benchmark in
`benchmarks/`. The headers are compiled
twice: into the block library, whose generated units register the blocks under their keys, and into
whatever names the type directly.

`app/` holds `adsb`: `adsb.cpp`, the one-process application and `ConsoleSink` together. Everything
in the chain that is not in `blocks/` belongs to GNU Radio 4 and is used as installed, the runner
of the graph files among it.

`graphs/` holds the same chain as graph files, cut in two at the decoded frame, and `tools/` the
two Python programs that go with them: a reader of the receiver's socket and a client of a control
plane. Neither is compiled; both are text, and the install puts them under
`<prefix>/share/gr4-adsb`.

`adsb` names every block type and builds its chain with `emplaceBlock<T>`, the direct, statically
checked route a program that knows its own chain takes; `--verbose` is the only place it opens the
registry. The registry serves a program that does not know its chain, and `rungraph` is that
program: it builds every graph out of the keys the plugin directories brought, so one program runs
either half of the split and needs no rebuild when a graph file changes.

## License

MIT. See [LICENSE](LICENSE).
