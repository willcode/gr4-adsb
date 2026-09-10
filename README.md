# gr4-adsb — an ADS-B receiver as a GNU Radio 4 out-of-tree module

A 1090 MHz Mode S / ADS-B receiver: a block library and one console application that runs it. It
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

The module builds against an installed GNU Radio 4; see [Building](#building) for what that has to
include.

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
./build/app/adsb --device "driver=hackrf"        --rate 4e6
./build/app/adsb --device "driver=uhd,type=b200" --rate 4e6
```

Once a device streams, add the analog bandwidth, and on a device with a real gain control the gain:

```
./build/app/adsb --device "driver=hackrf"  --rate 4e6 --bandwidth 5e6 \
                 --gain 40
```

## How it works

```
  source ──► PpmFramer<complex<float>> ──► ModeSDecode ──► AdsbPrinter
                                                                │
                                                                ▼
                                                           ConsoleSink

  source, live:  SoapySource<complex<float>>
  source, file:  BasicFileSource<int16_t> ──► InterleavedToComplex
```

**`PpmFramer`** (this module) reads frames out of the sample stream. Pulse-position modulation puts
one pulse per bit period, in the first half for a one and the second for a zero, so a bit is decided
by comparing the two halves of one period against each other: there is no carrier to acquire and no
clock to recover. The framer nominates a position wherever the preamble's four pulses stand out
from the gaps by at least `--threshold`, reads the frame there, and checks the 24-bit parity. Only a
frame whose parity checks consumes its own window; every other nomination advances the search by one
sample, so a false nomination cannot hide a real frame behind it.

**`ModeSDecode`** (this module) reads the downlink message into typed metadata and leaves the
frame's octets as the payload, so nothing the frame said is lost. **`AdsbPrinter`** (this module)
renders one line of text per record and publishes it as a record of its own. **`ConsoleSink`** lives
in `app/` and writes those lines to stdout; it is the only block in the chain that assumes a
console.

**The sample rate must be an even integer multiple of 1 MHz.** A Mode S slot is half a microsecond,
so the rate has to make a slot a whole number of samples. The framer refuses any other rate rather
than resampling silently: 2, 4, 6 and 8 MS/s are all fine, 2.4 MS/s is not. Higher is better here,
and 2 MS/s is a weak floor — expect fewer frames and many more parity failures than at 4 or 6 MS/s
from the same antenna. An RTL-SDR tops out below 4 MS/s, so 2 MS/s is its only rate.

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
| `--verbose` | off | list the block library's registry keys, then run |
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

**`--verbose` loads the module's block library through `gr::PluginLoader` before the run and prints
the registry keys it adds.** The chain itself never uses the registry, so the keys printed are
exactly what the library brought. The rest of the run is unchanged.

```
$ ./build/app/adsb --verbose --file capture.cs16 --rate 2e6
adsb: plugin directory .../build/blocks
adsb: loaded .../build/blocks/libGr4AdsbBlocksShared.so (8 registrations)
adsb: the registry gained 4 keys:
adsb:   gr4adsb::AdsbPrinter
adsb:   gr4adsb::ModeSDecode
adsb:   gr4adsb::PpmFramer<complex<float32>>
adsb:   gr4adsb::PpmFramer<float32>
```

Each block is entered under its type name and under its alias, which for these blocks is the same
string, so eight registrations leave four keys. `--plugin-dir` overrides the directory; its default
is the one belonging to the running executable, the build tree's for a binary in the build tree and
`<prefix>/lib/gnuradio-4/plugins` for an installed one.

## Building

Requirements:

- **GNU Radio 4**, installed into one prefix. Built and tested against the `main` branches of GNU
  Radio 4 core, library and blocks as of 9 September 2026.
- **The SDR family**, for live devices. The blocks project installs
  `gnuradio-4.0/sdr/SoapySource.hpp` only when its SDR family is enabled, and that family is off by
  default. Without it this module still configures and builds, and `adsb` offers `--file` alone. The
  configure line says which of the two this build got.
- **SoapySDR** and a driver for the radio, again only for live devices.
- **Boost.UT**, only to build the tests. It is not part of GNU Radio 4; point
  `-DGR4ADSB_BOOST_UT_INCLUDE_DIR` at its include directory if it is not already findable.

A GNU Radio 4 blocks tree from before September 2026 needs one allowance and has one limitation.
`SoapySource.hpp` there includes a compatibility header the family does not install, so this
module's build supplies exactly that one header from a directory of its own, leaving every other
`gnuradio-4.0` header to come from the prefix; the shim is gated on the file being absent and says
so at configure time. And `SoapySource` there drops a whole 8192-sample chunk whenever the graph
below it stops taking samples, about 2 ms of air at 4 MS/s, which costs frames. Nothing here works
around the second.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=<gnuradio4-prefix> \
      -DCMAKE_INSTALL_PREFIX=<install-prefix>
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build
```

`-DGR4ADSB_ENABLE_TESTING=OFF` skips the tests and `-DGR4ADSB_WARNINGS_AS_ERRORS=OFF` relaxes
`-Werror`. The three tests exercise the blocks directly, without a registry or a plugin:
`qa_PpmFramer` frames rendered bursts and checks the positions, the sub-sample offsets and the
buffer bounds; `qa_ModeSDecode` decodes known messages of each downlink format; `qa_AdsbPrinter`
checks the rendered lines. The install writes `adsb` to `<prefix>/bin`, the block library to
`<prefix>/lib/gnuradio-4/plugins` and the headers to `<prefix>/include/gr4adsb`.

## Module layout

`lib/` holds the code that is not a block: the PPM framing scanner, the parameterized CRC the Mode S
parity is one instance of, the MMSE interpolator the framer's phase search reads through, and the
filter design and window that build its tap bank. Each sits under `lib/include/gr4adsb/algorithm/`
at the path it would take in GNU Radio 4's own library, because that is where each would be added.

`blocks/` holds the blocks: `PpmFramer`, which wraps the scanner, and `ModeSDecode` and
`AdsbPrinter`, the message layer above it. `app/` holds the application and `ConsoleSink`. What
decides which side of the `lib`/`blocks` line a file belongs on is whether it came from the common
library or would be added to it; code specific to one block stays with that block.

`blocks/CMakeLists.txt` calls `gr_add_block_library` from GNU Radio 4's `GnuRadioBlockLib` package,
which is the supported way to register blocks out of tree. It runs the registration generator over
the three headers that carry `GR_REGISTER_BLOCK` and produces `libGr4AdsbBlocksShared.so`, which a
plugin loader opens, and `libGr4AdsbBlocksStatic.a`, which a consumer links instead. Both are
installed under `<prefix>/lib/gnuradio-4/plugins`; the directory follows `CMAKE_INSTALL_LIBDIR`, so
it is `lib64` on a distribution that puts libraries there.

A consumer that links either form calls `gr::blocklib::initGr4AdsbBlocks(registry)`, declared in the
generated `<gnuradio-4.0/Gr4AdsbBlocks.hpp>`, to register the blocks. Explicit initialization is the
contract for a linked library: a shared library nothing references is not recorded in `DT_NEEDED`
under the linker's default `--as-needed`, so registration through static initializers is not
portable there. A library that is opened rather than linked does register from its static
initializers as it loads, which is what `--verbose` reports.

`adsb` links neither form. It names every block type and uses `emplaceBlock<T>`, the direct,
statically checked route, which is what a program that knows its own chain needs. The registry is
for a program that does not: one building a graph from a saved description, or loading the shared
object as a plugin.

**The plugin ABI is versioned and the loader demands an exact match**, so rebuild this module
against whatever GNU Radio 4 it is going to be loaded into.

### Carried files

Most of the chain is GNU Radio 4's own and is used as installed. These files are carried into this
repository because they are not in a GNU Radio 4 release yet. They come from the `rework` branches
of [gnuradio4-blocks](https://github.com/willcode/gnuradio4-blocks) and
[gnuradio4-library](https://github.com/willcode/gnuradio4-library).

| Carried into | Taken from |
|---|---|
| `lib/…/algorithm/digital/Crc.hpp` | library `algorithm/digital/Crc.hpp` |
| `lib/…/algorithm/digital/Ppm.hpp` | library `algorithm/digital/Ppm.hpp` |
| `lib/…/algorithm/sync/MmseInterpolator.hpp` | library `algorithm/sync/…` |
| `lib/…/algorithm/filter/FilterDesign.hpp` | library `algorithm/filter/…` |
| `lib/…/algorithm/fourier/window.hpp` | library `algorithm/fourier/…` |
| `blocks/digital/…/PpmFramer.hpp` | blocks `blocks/digital/PpmFramer.hpp` |
| `blocks/adsb/…/ModeS.hpp` | blocks `blocks/adsb/ModeS.hpp` |
| `blocks/adsb/…/AdsbPrinter.hpp` | blocks `blocks/adsb/AdsbPrinter.hpp` |
| `blocks/test/qa_PpmFramer.cpp` | blocks `blocks/digital/test/…` |
| `blocks/test/qa_ModeSDecode.cpp` | blocks `blocks/adsb/test/…` |
| `blocks/test/qa_AdsbPrinter.cpp` | blocks `blocks/adsb/test/…` |
| `blocks/test/TestSpans.hpp` | blocks `blocks/digital/test/TestSpans.hpp` |

`window.hpp` is carried although GNU Radio 4 installs a window header of its own, because the
installed one stops at Kaiser and the filter design here names Bartlett, Welch and Parzen. All the
carried files keep their contract comments and their tests.

Everything sits in the `gr4adsb` namespace, the blocks under the `gr4adsb/…` include path and the
algorithm headers under `gr4adsb/algorithm/…`, so nothing here collides with an installed GNU Radio.
The three interpolation headers keep their own sub-namespaces under that name, `gr4adsb::sync`,
`gr4adsb::filter::design` and `gr4adsb::algorithm::window`, so they sit beside GNU Radio 4's copies
of the same code rather than over them. If these blocks land in GNU Radio 4 itself, this module
shrinks to the application.

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
