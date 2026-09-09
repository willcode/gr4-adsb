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

This demo is built against **GNU Radio 4 main plus the pull requests still expected to merge**, not
against bare main. At the time of writing that is:

- **core** — main `2885898b` plus #35, #38, #39, #44, #45, #47
- **library** — main `d2e91398`; nothing of ours is open against it
- **blocks** — main `3294b9fe` (which already carries #26, merged upstream on 2026-09-09) plus #27,
  #31, #32, #34, #36, #37

The SDK it was built and tested against on 2026-09-09 was an integration of those branches, not an
upstream release. It also configures and runs against bare upstream main — the two places where that
costs something are called out under Building and Running below.

## The chain

```
live:   SoapySource<complex<float>> ──────────────────────────► Abs ─► PpmFramer ─► ModeSDecode ─► AdsbPrinter ─► ConsoleSink
file:   BasicFileSource<int16_t> ─► InterleavedToComplex ─────► Abs ─► (the same tail)
```

- **`Abs`** takes the magnitude. Mode S carries nothing in phase, so this is where the complex
  stream ends.
- **`PpmFramer`** (this module) reads frames out of the magnitude stream. Pulse-position modulation
  puts one pulse per bit period, in the first half for a one and the second for a zero, so a bit is
  decided by comparing the two halves of one period against each other: there is no carrier to
  acquire and no clock to recover. The framer nominates a position wherever the preamble's four
  pulses stand out from the gaps by at least `threshold`, reads the frame, and checks the 24-bit
  parity. Only a frame the parity vouches for consumes its own window; every other nomination
  advances the search by one sample, so a false nomination cannot swallow the real frame behind it.
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

### The SDK needs the SDR family

The live path needs `gnuradio-4.0/sdr/SoapySource.hpp`, which the blocks project installs only when
it is configured with **`-DGR4_ENABLE_SDR=ON`** — the family defaults to OFF. Without it this module
still configures and builds, and `adsb_print` offers `--file` alone; the configure says which of the
two you got.

Two things about that family are worth knowing when you build against a stock SDK:

- It is the only block family that exports no CMake target — there is no `gnuradio4::gr-sdr` — so
  `app/CMakeLists.txt` finds its headers on the prefix and `SoapySDR` itself rather than linking one
  target. If upstream starts exporting the target, that block of CMake collapses to one line.
- `SoapySource.hpp` includes `gnuradio-4.0/sdr/NamespaceCompatibility.hpp`, which the family did not
  install before **gnuradio4-blocks #26**. Only on an SDK predating that fix does the build tree
  supply exactly that one header from a directory holding nothing else, leaving every other
  `gnuradio-4.0` header to come from the prefix. The shim is gated on the file being absent, so on a
  current SDK it does not fire and the configure says so.

### The plugin

`blocks/CMakeLists.txt` calls `gr_add_block_library` from the SDK's `GnuRadioBlockLib` package — the
same mechanism core's `test_external/minimal_blocklib` uses, which is the supported way to register
blocks out of tree. It runs the SDK's registration generator over the three headers that carry
`GR_REGISTER_BLOCK` and produces:

- `libGr4AdsbBlocksShared.so`, installed under `<prefix>/lib/gnuradio-4/plugins`, and
- `libGr4AdsbBlocksStatic.a`, which `adsb_print` links.

Either form registers its blocks through the generated `<gnuradio-4.0/Gr4AdsbBlocks.hpp>`, whose
`gr::blocklib::initGr4AdsbBlocks(registry)` a consumer calls explicitly. Explicit initialization is
the contract: a shared library nothing references is not recorded in `DT_NEEDED` under the linker's
default `--as-needed`, so relying on static initializers to register is not portable.

`adsb_print` does not need any of that — it names every block type and uses `emplaceBlock<T>`, which
is the direct, statically checked route. The registry matters for a consumer that builds a graph
from a saved description or loads the shared object as a plugin.

**The plugin ABI is versioned and the loader demands an exact match**, so rebuild this module
against whatever SDK it is going to be loaded into. On the target SDK above the ABI is **2**
(`GR_PLUGIN_CURRENT_ABI_VERSION` in `Plugin.hpp`, from core #38); an SDK without #38 is still at 1,
and a plugin built against one will not load into the other.

## Running

Against a recorded capture — interleaved 16-bit I/Q, host byte order, which is what most SDR
recorders write:

```
./build/app/adsb_print --file ADSB_20260812_182544_1090000000_2000000_cs16.sigmf-data --rate 2e6
```

The file run is not paced: it reads as fast as the disk delivers, which is what you want for a
capture and means the printed timestamps are stream positions rather than wall-clock times. (GNU
Radio 4's `Throttle` would pace it, but upstream's version needs a tag-propagation policy that is
not in an upstream release yet, so nothing here uses it.)

Only on an SDK predating **gnuradio4-blocks #27** does the file source size its reader chunk from an
unconstrained output port, ask for more than any span can offer, and never start. The application
bounds that port and gives it a ring several chunks deep regardless; with #27 the source sizes the
chunk from the actual ring instead, and the bound is redundant but harmless.

Start with the rate alone and let the device choose everything else — this is the form most likely
to open a radio that is fussy about its parameter set:

```
./build/app/adsb_print --device "driver=hackrf"        --rate 4e6
./build/app/adsb_print --device "driver=uhd,type=b200" --rate 4e6
./build/app/adsb_print --device "driver=sdrplay"       --rate 6e6
```

Then add bandwidth, and on devices with a real gain control the gain, once it streams:

```
./build/app/adsb_print --device "driver=hackrf"  --rate 4e6 --bandwidth 5e6 --gain 40
./build/app/adsb_print --device "driver=sdrplay" --rate 6e6 --bandwidth 6e6
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

`--device` takes the SoapySDR device string you would give any other Soapy program. The application
splits it into the two settings the block wants — the `driver=` key becomes `device` and the rest
becomes `device_parameter` — so a bare `--device hackrf` works too.

Full command line: `--device`, `--file`, `--frequency` (default 1090e6), `--rate` (default 4e6),
`--threshold` (default 2), `--seconds`, and the three optional device settings `--bandwidth`,
`--gain` and `--agc on|off`, each left to the device when not given. Exit codes: 0 on a clean stop,
2 on a bad command line, 1 on a graph error, each with the reason on stderr. Sweep `--gain` if the
counters printed at exit show nominations without admitted frames.

On an SDK predating **gnuradio4-blocks #26** (merged upstream on 2026-09-09), `SoapySource` dropped a
whole 8192-sample chunk whenever the graph below it stopped taking samples — about 2 ms of air at
4 MS/s, so lost frames. Nothing here works around it; on such an SDK it shows up as a lower frame
rate than the band deserves.

## What is carried, and from where

Most of the chain is upstream GNU Radio 4 and is used as installed. What follows is carried into this
repository because it is not in an upstream release. It comes from the willcode forks of GNU Radio 4 —
[gnuradio4-blocks](https://github.com/willcode/gnuradio4-blocks) and
[gnuradio4-library](https://github.com/willcode/gnuradio4-library) — branch `rework`, blocks at
`f634dfc0` and library at `cfe5081`. Each row names the commit that last touched the file it was taken
from, so a later sync can tell at a glance what has moved.

| Carried into | Taken from | Commit |
|---|---|---|
| `blocks/digital/…/Crc.hpp` | library `algorithm/digital/Crc.hpp` — the parameterized CRC the Mode S parity is one instance of | `a5ca312` |
| `blocks/digital/…/Ppm.hpp` | library `algorithm/digital/Ppm.hpp` — `PpmScanner`, the framing kernel, and the `modeS()` framing constant | `52f03fa` |
| `blocks/digital/…/PpmFramer.hpp` | blocks `blocks/digital/PpmFramer.hpp` — the block over that kernel | `9ee457d2` |
| `blocks/adsb/…/ModeS.hpp` | blocks `blocks/adsb/ModeS.hpp` — the message layer | `9fd023db` |
| `blocks/adsb/…/AdsbPrinter.hpp` | blocks `blocks/adsb/AdsbPrinter.hpp` — the renderer | `23fb32f4` |
| `blocks/test/qa_PpmFramer.cpp` | blocks `blocks/digital/test/qa_PpmFramer.cpp` | `9ee457d2` |
| `blocks/test/qa_ModeSDecode.cpp` | blocks `blocks/adsb/test/qa_ModeSDecode.cpp` | `9fd023db` |
| `blocks/test/qa_AdsbPrinter.cpp` | blocks `blocks/adsb/test/qa_AdsbPrinter.cpp` | `23fb32f4` |
| `blocks/test/TestSpans.hpp` | blocks `blocks/digital/test/TestSpans.hpp` | `ca84b8f4` |

They keep their contract comments and their tests. Everything was renamed out of the fork's
namespaces into `gr4adsb`, and the include path is `gr4adsb/…`, so nothing here collides with an
installed GNU Radio. If these blocks are upstreamed, this module shrinks to the application.

## Next

Two components are named but deliberately not built or wired, because they are not defined yet:

- **A map display.** It attaches to the decoded record stream — `ModeSDecode`'s output, before
  `AdsbPrinter`, where position, altitude, callsign and velocity are already typed metadata on the
  record and nothing has to be parsed back out of a rendered line.
- **A waterfall.** It attaches to the magnitude stream — `Abs`'s output, before `PpmFramer`, which is
  the last point where every sample is still present. A spectrum display would instead want a tap on
  the complex stream ahead of `Abs`.

Neither is in the graph. Adding either means one more `emplaceBlock` and one more `connect` at the
point named above; the existing chain does not have to change to make room.
