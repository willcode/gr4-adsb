# gr4-adsb — an ADS-B receiver as a GNU Radio 4 out-of-tree module

A 1090 MHz Mode S / ADS-B receiver: a block library (the plugin) and one console application that
runs it. It decodes the extended squitter — aircraft identification, barometric altitude, position
and velocity — and prints one line per frame.

```
   29.768  DF17  AB0969  ident   callsign AAL160
   29.781  DF17  AB0969  airpos  alt 39000 ft  lat 42.40448  lon -71.34696  [AAL160]
   29.782  DF17  A9A901  veloc   gs 349 kt  track 103 deg  vs -1024 ft/min
```

The columns are the time in seconds from the frame's own position in the stream, the downlink
format, the ICAO address, what the message said, and the aircraft's identification in brackets once
an earlier frame from that address has given one.

## Target

The demo needs an installed GNU Radio 4 SDK: core, library and blocks in one prefix. The set it was
built, tested and measured against, on 2026-09-09, was

- **core** — `2885898b`, with pull requests #35, #39, #44, #45, #47 and #49 merged into it,
- **library** — `d2e91398`, and
- **blocks** — `db8836c2`, with pull requests #27, #31, #32, #34, #37 and #38 merged into it.

It also configures, builds and runs against the plain `main` of the three projects; the two places
where an older SDK costs something are called out under Building and Running below.

## The chain

```
live:   SoapySource<complex<float>> ──────────────────────────► PpmFramer<complex<float>> ─► ModeSDecode ─► AdsbPrinter ─► ConsoleSink
file:   BasicFileSource<int16_t> ─► InterleavedToComplex ─────► (the same tail)
```

- **`PpmFramer`** (this module) reads frames out of the sample stream. Pulse-position modulation
  puts one pulse per bit period, in the first half for a one and the second for a zero, so a bit is
  decided by comparing the two halves of one period against each other: there is no carrier to
  acquire and no clock to recover. The framer nominates a position wherever the preamble's four
  pulses stand out from the gaps by at least `threshold`, reads the frame, and checks the 24-bit
  parity. Only a frame the parity vouches for consumes its own window; every other nomination
  advances the search by one sample, so a false nomination cannot swallow the real frame behind it.
  Mode S carries nothing in phase and every decision here is made on magnitude, but the complex
  samples reach the block rather than their magnitudes, because deciding a position at the
  sub-sample phase its preamble sits on interpolates the signal and a magnitude stream has thrown
  away what an interpolation reads. `PpmFramer<float>` takes magnitudes as before and decides at
  integer positions only; the demo instantiates `PpmFramer<std::complex<float>>`.
- **`ModeSDecode`** (this module) reads the downlink message into typed metadata and leaves the
  frame's octets as the payload, so nothing the frame said is lost.
- **`AdsbPrinter`** (this module) renders one line of text per record, published as a record of its
  own rather than written to a terminal.
- **`ConsoleSink`** lives in `app/` and writes each of those lines to stdout. It is the only piece
  of the chain that assumes a console, which is what makes the rest of it reusable.

## The rate rule

**`--rate` must be an even integer multiple of 1 MHz.** A Mode S slot is half a microsecond, so the
rate has to make a slot a whole number of samples; the framer refuses anything else rather than
resample behind your back. 2, 4, 6, 8 MS/s are all fine, 2.4 MS/s is not.

**The framer decides each position at the sub-sample phase its preamble sits on, and that is on by
default.** A burst does not arrive on the sample clock. A pulse that straddles two samples splits its
energy between a pulse slot and a gap slot, and a gap slot is the very quantity the shape test
measures the pulses against, so bad timing lowers the numerator and raises the denominator at once.
So a position first passes a cheap test at integer positions, at `--threshold` times `--coarse-ratio`
(default 0.5), and each of those candidates is then measured again at `--phase-steps` offsets
(default 8) spanning one sample period, on the signal interpolated from the complex samples. The best
offset has to reach the full `--threshold`, and it decides the bits as well; it travels on the record
as `sample_phase`, so a frame's position in the stream is `sample_start + sample_phase` and not
`sample_start` alone. `--phase-steps 1` turns all of this off and leaves the decisions exactly where
they were before it existed.

**It pays most where the pulse is oversampled, and it pays at 2 MS/s as well.** Above two samples a
slot the argument is straightforward: a pulse spans more than one sample, so an interpolation has
something to work with. On a synthetic burst through a front end exactly one slot wide, sampled at
one sample a slot, it should buy nothing at all — that filter makes the samples the linear
interpolation of the ideal slot values, and no interpolation recovers what a linear one already
threw away. That is what the measurement says: at one sample a slot the search lifts the worst
preamble shape ratio from 1.000 to 1.353 against a threshold of 2 and admits the same offsets the
integer test admits; at three samples a slot it lifts 2.600 to 3.470 and admits every offset.
`qa_PpmFramer` here holds the one-sample-a-slot half of that comparison, over eight rendered
offsets.

**A real RTL-SDR is not that filter, and on its captures the search finds frames.** Replayed offline
at 2 MS/s, threshold 2, the same two 15 s captures the RTL-SDR paragraph under Running tables, the
admitted frames go from 16 to 28 and from 21 to 44 — every frame the integer decision found is still
found, at the same stream position, and the new ones carry the same two aircraft on coherent
descents with the 24-bit parity vouching for each. So the synthetic case is a floor, not a
prediction: what the search buys at one sample a slot depends on the receiver's own front end, which
on a dongle is nothing like a half-microsecond boxcar.

**It costs candidate work, and the noise floor sets how much.** The coarse test at half the threshold
passes far more positions than the full test does, and every one of them is interpolated. Measured
single-threaded on this machine, replaying 15 s of air: 0.71 s of CPU at `--phase-steps 1` against
7.87 s at 8 on the noisier capture (1.6 M candidates), and 0.61 s against 3.02 s on the quieter one
(474 k). On the 30 s recording under Running, which is quiet, 1.67 s against 1.82 s. So the fine
step is real-time at 2 MS/s on one core with room to spare, but the margin is the noise floor's to
give — raise `--coarse-ratio` if a run cannot keep up.

**2 MS/s is still the floor and still a weak one, for the reason it always was: the sample rate.**
Expect fewer frames and many more parity failures than at 4 or 6 MS/s from the same antenna, and
`--agc on` is still the first setting to reach for on a dongle — it changes the parity failure rate,
which is a signal-quality change, where the phase step changes the frame count. An RTL-SDR tops out
below 4 MS/s, so 2 MS/s is its only rate here; see the RTL-SDR paragraph under Running.

## The bandwidth note

**`--bandwidth` matters more than `--rate`.** A Mode S pulse is 0.5 µs, which needs several MHz of
analog bandwidth to survive with its edges intact. A receiver whose front end defaults to a
rate-derived filter — 1536 kHz is a common one — smears the pulses together and admits *nothing*, at
any sample rate.

`--bandwidth` nonetheless has **no default here**: unset, the device picks its own for the rate, and
that is the setting most likely to make a radio start at all. Set it explicitly once the run works.
Measured good points on an SDRplay RSP1A: **4 MS/s at 5 MHz** and **6 MS/s at 6 MHz**. If a run
reports nominations but no admitted frames, widen the analog filter before touching anything else.

## Building

The module builds against an installed GNU Radio 4 SDK: core, library and blocks installed into one
prefix.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<sdk>
cmake --build build
ctest --test-dir build --output-on-failure
```

Options: `-DGR4ADSB_ENABLE_TESTING=OFF` skips the tests, `-DGR4ADSB_WARNINGS_AS_ERRORS=OFF` relaxes
`-Werror`. The tests need Boost.UT, which is not part of the SDK; point
`-DGR4ADSB_BOOST_UT_INCLUDE_DIR` at its include directory if it is not already findable.

### Building the SDK

The three GNU Radio 4 projects install into one prefix in the order core, library, blocks, each
configured with **`-DENABLE_TESTING=OFF`** (their default is ON):

```
cmake -S gnuradio4-core    -B build-core    -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTING=OFF -DCMAKE_INSTALL_PREFIX=<sdk>
cmake -S gnuradio4-library -B build-library -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTING=OFF -DCMAKE_PREFIX_PATH=<sdk> -DCMAKE_INSTALL_PREFIX=<sdk>
cmake -S gnuradio4-blocks  -B build-blocks  -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTING=OFF -DCMAKE_PREFIX_PATH=<sdk> -DCMAKE_INSTALL_PREFIX=<sdk> -DGR4_ENABLE_SDR=ON
```

with `cmake --build` and `cmake --install` after each. Testing off is required, not a shortcut. The
suites are no part of an SDK and need Boost.UT, and with tests on the blocks tree has to compile
every family's tests before it can install; a test that includes another family's headers without
linking that family compiles only when a blocks install already sits in the prefix, which a fresh
SDK build does not have. This module brings its own tests and runs them against the installed SDK.

### The SDK needs the SDR family

The live path needs `gnuradio-4.0/sdr/SoapySource.hpp`, which the blocks project installs only when
it is configured with **`-DGR4_ENABLE_SDR=ON`** — the family defaults to OFF. Without it this module
still configures and builds, and `adsb` offers `--file` alone; the configure says which of the
two you got.

Two things about that family are worth knowing:

- An SDK that exports `gnuradio4::gr-sdr` puts the sdr include path, the algorithm headers
  `SoapySource.hpp` pulls in and `SoapySDR` itself on that one target, and `app/CMakeLists.txt` links
  it and nothing else. An SDK that installs the sdr headers but exports no target for them leaves the
  same file to find the headers on the prefix and `SoapySDR` by hand. The configure line says which of
  the two it took.
- `SoapySource.hpp` includes `gnuradio-4.0/sdr/NamespaceCompatibility.hpp`, which the family did not
  install before **gnuradio4-blocks #26**. Only on an SDK predating that does the build tree supply
  exactly that one header from a directory holding nothing else, leaving every other `gnuradio-4.0`
  header to come from the prefix. The shim is gated on the file being absent, so on a newer SDK it
  does not fire and the configure says so.

### The plugin

`blocks/CMakeLists.txt` calls `gr_add_block_library` from the SDK's `GnuRadioBlockLib` package — the
same mechanism core's `test_external/minimal_blocklib` uses, which is the supported way to register
blocks out of tree. It runs the SDK's registration generator over the three headers that carry
`GR_REGISTER_BLOCK` and produces:

- `libGr4AdsbBlocksShared.so`, the loadable one, and
- `libGr4AdsbBlocksStatic.a`, for a consumer that would rather link the blocks in than open them,

both installed under `<prefix>/lib/gnuradio-4/plugins` — `lib64` on a distribution that puts
libraries there, since it follows `CMAKE_INSTALL_LIBDIR`.

Either form registers its blocks through the generated `<gnuradio-4.0/Gr4AdsbBlocks.hpp>`, whose
`gr::blocklib::initGr4AdsbBlocks(registry)` a consumer that links it calls explicitly. Explicit
initialization is the contract for a linked library: a shared library nothing references is not
recorded in `DT_NEEDED` under the linker's default `--as-needed`, so relying on static initializers
to register is not portable. A library that is *opened* rather than linked is the other case, and
there the static initializers are exactly what run: `gr::PluginLoader` sees the registry entries
appear as it loads the file, which is what `--verbose` reports.

`adsb` links neither. It names every block type and uses `emplaceBlock<T>`, which is the direct,
statically checked route and all a program that knows its own chain needs. The registry is for a
program that does not: one building a graph from a saved description, or loading the shared object
as a plugin. `--verbose` is that program, here — see Running.

**The plugin ABI is versioned and the loader demands an exact match**, so rebuild this module
against whatever SDK it is going to be loaded into. It is `GR_PLUGIN_CURRENT_ABI_VERSION` in the
SDK's `Plugin.hpp`, and it is **1** on the SDK above; a plugin built against one value will not load
into a core built with another.

## Running

Against a recorded capture — interleaved 16-bit I/Q, host byte order, which is what most SDR
recorders write:

```
./build/app/adsb --file ADSB_20260812_182544_1090000000_2000000_cs16.sigmf-data --rate 2e6
```

The file run is not paced: it reads as fast as the disk delivers, which is what you want for a
capture and means the printed timestamps are stream positions rather than wall-clock times. GNU
Radio 4's `Throttle` would hold it to the sample rate, which is what a live run wants and a replay
does not, so the chain does not carry one.

The file source reads on a thread of its own and takes the size of a read from what its output port
can carry, so the application sets that port to 64 ki samples and gives it a ring eight reads deep.
That is the only tuning in the file path, and it is what keeps the reader running ahead of the
framer instead of waiting on it.

### `--verbose`: the blocks through the registry

The chain the application builds names every block type, so nothing in it needs the registry. **With
`--verbose` the run first shows the module's other face**: `gr::PluginLoader` opens the shared block
library, whose registrations enter the process-wide `gr::BlockRegistry`, and the application prints
the file it opened and the keys that appeared. Then it runs exactly as it would have.

```
$ ./build/app/adsb --verbose --file ADSB_20260812_182544_1090000000_2000000_cs16.sigmf-data --rate 2e6
adsb: plugin directory .../gr4-adsb/build/blocks
adsb: loaded .../gr4-adsb/build/blocks/libGr4AdsbBlocksShared.so (8 registrations)
adsb: the registry gained 4 keys:
adsb:   gr4adsb::AdsbPrinter
adsb:   gr4adsb::ModeSDecode
adsb:   gr4adsb::PpmFramer<complex<float32>>
adsb:   gr4adsb::PpmFramer<float32>
adsb: ADSB_20260812_182544_1090000000_2000000_cs16.sigmf-data at 2.000 MS/s, threshold 2, 8 phase steps at a coarse ratio of 0.5
    2.767  DF17  AA1BAA  airpos  alt 3600 ft
```

Eight registrations for four keys: every block is entered under its type name and under its alias,
and for these blocks the two spell the same string, so the second entry replaces the first. The four
keys are what a graph loaded from a saved description would name. A file the loader could not use is
reported with its reason, and one the scan declined to open is named rather than passed over.

**`--plugin-dir` says where to look.** Its default is the directory belonging to the copy of the
application that is running: the build tree's `blocks/` directory for a binary still in the build
tree, and `<prefix>/lib/gnuradio-4/plugins` for an installed one, where `<prefix>` is the
`CMAKE_INSTALL_PREFIX` the module was configured with and `lib` follows `CMAKE_INSTALL_LIBDIR`. An
install that was moved after the fact, or a plugin directory belonging to something else, is what
`--plugin-dir` is for.

Start with the rate alone and let the device choose everything else — this is the form most likely
to open a radio that is fussy about its parameter set:

```
./build/app/adsb --device "driver=hackrf"        --rate 4e6
./build/app/adsb --device "driver=uhd,type=b200" --rate 4e6
./build/app/adsb --device "driver=sdrplay"       --rate 6e6
./build/app/adsb --device "driver=rtlsdr"        --rate 2e6 --agc on
```

Then add bandwidth, and on devices with a real gain control the gain, once it streams:

```
./build/app/adsb --device "driver=hackrf"  --rate 4e6 --bandwidth 5e6 --gain 40
./build/app/adsb --device "driver=sdrplay" --rate 6e6 --bandwidth 6e6
```

**`--gain`, `--bandwidth` and `--agc` are asked for only when you give them.** Left off, the device
keeps its own gain, its own bandwidth for the rate, and its own gain mode. The block pushes every
setting it holds when it opens the device, and its own defaults are 10 dB and 500 kHz, so without
this a parameter set nobody asked for would reach the radio. The application prints, at start,
exactly what it asked the device for and what it left alone, so a failed run says which it was.

**Leave `--gain` off on an SDRplay.** `--gain` is SoapySDR's overall gain, and the SDRplay driver
(SoapySDRPlay3) has no such thing: it exposes two gain-reduction elements, `IFGR` (20 to 59 dB,
ignored while the driver's AGC is on, which it is by default) and `RFGR`, the LNA state (0 to 9).
SoapySDR spreads an overall gain across those elements, and `--gain 40` lands LNA state 9; above
1 GHz the RSP1A has states 0 to 8 only, so the device refuses the whole activation with
`Init() failed: sdrplay_api_Fail`. The block's old 10 dB default failed the same way. Rate,
frequency and bandwidth alone stream cleanly at 6 MS/s. This was pinned with a standalone SoapySDR
probe that replays the block's call order one setter at a time: the sequence fails only once the
gain step is added, with the driver reporting `RFGR=9` just before the failing activation.

`--agc off` is the default and asks for nothing; the block only ever turns AGC *on*, so there is no
way from here to force it off on a device that enables it by itself. An SDRplay runs its AGC by
default, which suits ADS-B.

**On an RTL-SDR, use `--agc on` and no `--gain`.** With a fixed tuner gain the dongle's 8-bit
samples sit at the bottom of their range: a raw capture at 40 dB had a noise floor of 1.0 LSB rms
and one at the 49.6 dB maximum 1.4 LSB, so gap slots and bit decisions are made on one or two
quantization steps. `--agc on` turns on the RTL2832's digital gain as well as the tuner's, which
lifts the signal into the converter's range. Measured on air, same antenna, 20 s each, at 2 MS/s:

| settings | nominations | admitted | parity failed |
|---|---:|---:|---:|
| `--gain 40` | 103 | 12 | 32 |
| `--gain 49.6` | 226 | 15 | 73 |
| `--agc on` | 51 | 27 | 1 |
| `--agc on`, second run | 50 | 18 | 3 |
| `--agc on --threshold 1.5` | 168 | 27 | 44 |

Traffic differs between windows, but the parity failure rate does not lie: with AGC nearly every
long nomination is a real frame. Interpolating the 2 MS/s capture to 4 MS/s ahead of the framer was
also tried offline and made things worse, because resampling smears the pulse into the gap slots the
shape test measures — which is why the fine step interpolates inside the shape test instead, where
it can choose the offset rather than commit to one.

Both fixed-gain runs above were captured raw and replayed offline, 15 s each, so the phase step can
be measured against the integer decision on identical air. At 2 MS/s, `--phase-steps 1` against the
default 8:

| capture | phase steps | threshold | candidates | nominations | admitted | parity failed | short |
|---|---:|---:|---:|---:|---:|---:|---:|
| 40 dB | 1 | 2 | 3 398 | 3 398 | 16 | 605 | 2 777 |
| 40 dB | 8 | 2 | 1 601 985 | 3 450 | **28** | 608 | 2 814 |
| 40 dB | 8 | 1.5 | 1 602 462 | 3 710 | 50 | 691 | 2 969 |
| 49.6 dB | 1 | 2 | 3 014 | 3 014 | 21 | 655 | 2 338 |
| 49.6 dB | 8 | 2 | 474 100 | 3 083 | **44** | 661 | 2 378 |
| 49.6 dB | 8 | 1.5 | 475 120 | 3 491 | 80 | 806 | 2 605 |

`--phase-steps 1` reproduces the counters these captures gave before the fine step existed, sample
for sample, which is what makes the rest of the table a comparison. At the default it admits 28
frames where the integer decision admitted 16, and 44 where it admitted 21, while the parity
failures barely move (605 to 608, 655 to 661): the search is finding frames, not accepting noise.
Every frame the integer decision found is still found at the same stream position, and the added
ones decode as the same two aircraft on coherent descents.

A lower `--threshold` does buy real frames on these fixed-gain captures — 28 to 50 and 44 to 80 —
but it buys parity failures in the same breath, and on air with AGC an earlier comparison found no
gain from it at all. `--agc on` remains the setting that matters on a dongle; `--threshold 1.5` is
for a capture you already have and cannot retake.

`--device` takes the SoapySDR device string you would give any other Soapy program. The application
splits it into the two settings the block wants — the `driver=` key becomes `device` and the rest
becomes `device_parameter` — so a bare `--device hackrf` works too.

**`--phase-steps` and `--coarse-ratio` are the fine step's two settings.** A position first has to
pass a cheap test at integer sample positions, at `--threshold` times `--coarse-ratio` (default 0.5);
those passes are the `candidates` in the exit line. Each candidate is then measured again at
`--phase-steps` offsets spanning one sample period (default 8), and the best of them has to reach the
full `--threshold` to become a nomination. `--phase-steps 1` turns the fine step off and leaves every
decision exactly where it was before there was one, coarse test included. `--coarse-ratio` is the
cost dial: raising it toward 1 admits fewer candidates and so interpolates less, at the risk of
refusing a badly timed preamble before the search ever sees it, which is the whole thing the search
is for. See the rate rule for what it buys and what it costs.

Full command line: `--device`, `--file`, `--frequency` (default 1090e6), `--rate` (default 4e6),
`--threshold` (default 2), `--phase-steps` (default 8), `--coarse-ratio` (default 0.5), `--seconds`,
`--verbose`, `--plugin-dir`, and the three optional device settings `--bandwidth`, `--gain` and
`--agc on|off`, each left to the device when not given. Exit codes: 0 on a clean stop, 2 on a bad
command line, 1 on a graph error, each with the reason on stderr. Sweep `--gain` if the counters
printed at exit show nominations without admitted frames.

On an SDK predating **gnuradio4-blocks #26**, `SoapySource` dropped a whole 8192-sample chunk
whenever the graph below it stopped taking samples — about 2 ms of air at 4 MS/s, so lost frames.
Nothing here works around it; on such an SDK it shows up as a lower frame rate than the band
deserves.

## What is carried, and from where

Most of the chain is GNU Radio 4's own and is used as installed. What follows is carried into this
repository because it is not in a GNU Radio 4 release. It comes from
[gnuradio4-blocks](https://github.com/willcode/gnuradio4-blocks) and
[gnuradio4-library](https://github.com/willcode/gnuradio4-library), branch `rework`, blocks at
`71a8d45c` and library at `e65a957`. Each row names the commit that last touched the file it was taken
from, so a later sync can tell at a glance what has moved.

| Carried into | Taken from | Commit |
|---|---|---|
| `blocks/digital/…/Crc.hpp` | library `algorithm/digital/Crc.hpp` — the parameterized CRC the Mode S parity is one instance of | `a5ca312` |
| `blocks/digital/…/Ppm.hpp` | library `algorithm/digital/Ppm.hpp` — `PpmScanner`, the framing kernel, and the `modeS()` framing constant | `e65a957` |
| `blocks/digital/…/sync/MmseInterpolator.hpp` | library `algorithm/sync/MmseInterpolator.hpp` — the tap bank the phase search interpolates with | `813f054` |
| `blocks/digital/…/filter/FilterDesign.hpp` | library `algorithm/filter/FilterDesign.hpp` — carried whole for the one function the bank calls; not in a GNU Radio 4 release | `7a32d0e` |
| `blocks/digital/…/fourier/window.hpp` | library `algorithm/fourier/window.hpp` — the SDK installs an older one whose `window::Type` stops at Kaiser, and `FilterDesign.hpp` names Bartlett, Welch and Parzen | `a48ad14` |
| `blocks/digital/…/PpmFramer.hpp` | blocks `blocks/digital/PpmFramer.hpp` — the block over that kernel | `71a8d45c` |
| `blocks/adsb/…/ModeS.hpp` | blocks `blocks/adsb/ModeS.hpp` — the message layer | `9fd023db` |
| `blocks/adsb/…/AdsbPrinter.hpp` | blocks `blocks/adsb/AdsbPrinter.hpp` — the renderer | `23fb32f4` |
| `blocks/test/qa_PpmFramer.cpp` | blocks `blocks/digital/test/qa_PpmFramer.cpp` | `71a8d45c` |
| `blocks/test/qa_ModeSDecode.cpp` | blocks `blocks/adsb/test/qa_ModeSDecode.cpp` | `9fd023db` |
| `blocks/test/qa_AdsbPrinter.cpp` | blocks `blocks/adsb/test/qa_AdsbPrinter.cpp` | `23fb32f4` |
| `blocks/test/TestSpans.hpp` | blocks `blocks/digital/test/TestSpans.hpp` — the span pair alone; the file it came from also carries graph-driving helpers no test here uses | `ca84b8f4` |

They keep their contract comments and their tests. Everything was renamed into the `gr4adsb`
namespace, and the include path is `gr4adsb/…`, so nothing here collides with an installed GNU
Radio. The three interpolation headers keep their own sub-namespaces under that name —
`gr4adsb::sync`, `gr4adsb::filter::design` and `gr4adsb::algorithm::window` — so they sit beside the
SDK's copies of the same code rather than over them, which is what lets `window.hpp` be carried at
all. If these blocks land in GNU Radio 4 itself, this module shrinks to the application.

## Next

Two components are named but deliberately not built or wired, because they are not defined yet:

- **A map display.** It attaches to the decoded record stream — `ModeSDecode`'s output, before
  `AdsbPrinter`, where position, altitude, callsign and velocity are already typed metadata on the
  record and nothing has to be parsed back out of a rendered line.
- **A waterfall.** It attaches to the sample stream ahead of `PpmFramer`, which is the last point
  where every sample is still present, and where they are still complex — so a spectrum display taps
  the same place, and a magnitude display puts a `gr::blocks::basic::Abs<std::complex<float>>` of its
  own on the tap rather than in the framer's path.

Neither is in the graph. Adding either means one more `emplaceBlock` and one more `connect` at the
point named above; the existing chain does not have to change to make room.
