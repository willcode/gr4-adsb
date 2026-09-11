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
./build/app/adsb --device "driver=rtlsdr"        --rate 2e6 --agc on
./build/app/adsb --device "driver=sdrplay"       --rate 6e6
./build/app/adsb --device "driver=hackrf"        --rate 4e6 --gain 90
./build/app/adsb --device "driver=uhd,type=b200" --rate 4e6
```

Once a device streams, add what it needs: the RTL-SDR its AGC, the HackRF a high gain, and on a
device whose default filter is narrower than the pulses, the analog bandwidth (see Running).

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
digital and adsb families. **`ConsoleSink`** is defined in `app/` and writes the lines to stdout;
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

**On an RTL-SDR use `--agc on` and no `--gain`.** With a fixed tuner gain the dongle's 8-bit samples
sit at the bottom of their range, so gap slots and bit decisions rest on one or two quantization
steps. `--agc on` turns on the RTL2832's digital gain as well as the tuner's and lifts the signal
into the converter's range. `--agc off` is the default and asks for nothing; the block only ever
turns AGC on, so there is no way from here to force it off on a device that enables it by itself. An
SDRplay runs its AGC by default, which suits ADS-B.

**A HackRF needs `--gain 90` or thereabouts.** The driver opens the radio with 16 dB on the LNA and
16 dB on the VGA, which leaves the noise floor below one step of the 8-bit converter, so only a
nearby aircraft is heard. `--gain` is SoapySDR's overall gain and the HackRF driver splits it
across the amplifier, the LNA and the VGA; around 90 dB is a good point at 4 MS/s, and 110 dB
overloads the front end. Leave `--bandwidth` to the driver unless a measurement says otherwise.

**`--verbose` opens every shared block library in the plugin directory through `gr::PluginLoader`
and prints the registry keys they add.** It names each library and how many registrations it
carries, then lists the keys, sorted; among them are
`gr::blocks::digital::PpmFramer<complex<float32>>`, `gr::blocks::adsb::ModeSDecode` and
`gr::blocks::adsb::AdsbPrinter`, the three blocks this chain builds directly. Each block is entered
under its type name and under its alias, so a library adds fewer keys than it reports
registrations. The chain itself never uses the registry, so the keys printed are exactly what the
libraries brought, and the rest of the run is unchanged. `--plugin-dir` overrides the directory; its
default is the plugin directory of the GNU Radio 4 this build was configured against,
`<prefix>/lib/gnuradio-4/plugins` or `lib64` on a distribution that puts libraries there.

## Building

Requirements:

- **GNU Radio 4**, installed into one prefix. Core, library and blocks at their `main` branches as
  of 10 September 2026, together with the parts of this receiver that are not merged there yet: the
  pulse-position framing scanner in the library, the `PpmFramer` block in the blocks project's
  digital family, and that project's `adsb` family, `ModeSDecode` and `AdsbPrinter`. The willcode
  forks carry them as the leading commits of `restack/modem-20260910`, to be filed as pull requests
  against GNU Radio 4.
- **The SDR family**, for live devices. The blocks project builds and exports `gnuradio4::gr-sdr`
  only when `GR4_ENABLE_SDR` is on, and that is off by default. Without it this package still
  configures and builds, and `adsb` offers `--file` alone. The configure line says which of the two
  this build got.
- **SoapySDR** and a driver for the radio, again only for live devices.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=<gnuradio4-prefix> \
      -DCMAKE_INSTALL_PREFIX=<install-prefix>
cmake --build build
cmake --install build
```

`-DGR4ADSB_WARNINGS_AS_ERRORS=OFF` relaxes `-Werror`. The install writes `adsb` to `<prefix>/bin`
and nothing else; the blocks the chain is built from are installed with GNU Radio 4.

The package ships no test. What a test of this receiver would exercise — the framing, the message
decode and the rendering of a line — is tested in GNU Radio 4 beside each of those blocks, and what
is left here is the application and `ConsoleSink`. A replay over a recorded capture is how a build
is checked end to end, and a capture large enough to carry frames is too large to ship.

## Layout

`app/` holds the whole package: `adsb.cpp`, which is the application and `ConsoleSink` together.
Everything else in the chain belongs to GNU Radio 4 and is used as installed.

`adsb` names every block type and builds its chain with `emplaceBlock<T>`, the direct, statically
checked route, which is what a program that knows its own chain needs. The registry is for a
program that does not: one building a graph from a saved description, or loading a block library as
a plugin. `--verbose` is the only place this application opens one.

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
