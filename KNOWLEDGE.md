# gr4-adsb knowledge

A seed for a session that has to work on this package. It tells you where things are and what holds;
the code's `/*|` blocks carry the detail at each place. `README.md` and `blocks/README.md` are for a
human user of the package.

## Purpose

A 1090 MHz Mode S / ADS-B receiver on GNU Radio 4: three blocks (namespace `adsb`, header-only, built
as a block library) and one console program. GNU Radio 4 has no ADS-B family; this package is it.

## Map

| Path                                   | What it is                                                                    |
| -------------------------------------- | ----------------------------------------------------------------------------- |
| `blocks/include/adsb/PpmFramer.hpp`    | samples to one frame record per Mode S frame; parity checked here             |
| `blocks/include/adsb/ModeS.hpp`        | bit-field readers (`detail::`) and `ModeSDecode`, octets to typed keys        |
| `blocks/include/adsb/AdsbPrinter.hpp`  | decoded record to one console line, as a text record                          |
| `blocks/include/adsb/RecordFields.hpp` | typed metadata reads shared by the two consumers                              |
| `blocks/include/adsb/BlockLabels.hpp`  | family label, compiled only where the core supports labels                    |
| `app/adsb.cpp`                         | the `adsb` program: builds the chain in C++, live (SoapySDR) or from a file   |
| `graphs/*.yaml`                        | the same chain as graph files: capture, hackrf, rtl out to ZeroMQ; printer in |
| `tools/adsb_listen.py`                 | reads the ZeroMQ packets without GNU Radio (pyzmq)                            |
| `tools/adsb_control.py`                | runs a graph file as a control-plane session over REST                        |
| `blocks/test/qa_*.cpp`                 | one suite per block, two recording suites, the label suite                    |
| `blocks/benchmarks/bm_PpmScanner.cpp`  | scanner throughput on noise and on the anchor frame                           |

## The chain

```
source -> PpmFramer<complex<float>> -> ModeSDecode -> AdsbPrinter -> console or packets
            octets + framing keys       + adsb_* keys    + line, protocol text/adsb
```

`PpmFramer<float>` takes a magnitude stream (`|x|`, from `gr::blocks::math::Abs`) and skips the
sub-sample phase search. The complex form is the default everywhere: the search interpolates samples.
The scanner itself is core's `gr::digital::PpmScanner`; `modeS()` is its framing.

## Record keys

Every record is a `DataSet<uint8_t>`: items are the frame's octets (the line's bytes after the
printer), metadata in `meta_information[0]`. Each block copies the keys it received and writes its own
over them. Reads are exact-type: a key at another type reads as absent (`detail::read<T>`).

| Key                                                          | Type           | Writer          | Meaning                                                                                                     |
| ------------------------------------------------------------ | -------------- | --------------- | ----------------------------------------------------------------------------------------------------------- |
| `trigger_name`                                               | string         | framer          | `frame_label` setting, default `mode_s`                                                                     |
| `protocol`                                                   | string         | all three       | framer: profile; decoder: `mode_s/adsb`, `mode_s`, `mode_s/short`, `mode_s/unchecked`; printer: `text/adsb` |
| `sample_start`                                               | uint64         | framer          | absolute index of the first data sample                                                                     |
| `sample_rate`                                                | float          | framer          | rate in force for the frame; `sample_start / sample_rate` is the time; the input tag is `gr:sample_rate`    |
| `sample_phase`                                               | float          | framer          | sub-sample offset in [-0.5, 0.5); timing is `sample_start + sample_phase`                                   |
| `sequence`                                                   | uint64         | framer          | per-block record count, reset by a settings change                                                          |
| `crc_remainder`                                              | Size_t         | framer          | 0 on an admitted frame                                                                                      |
| `crc_ok`                                                     | bool           | framer          | long frames only; absent on short frames                                                                    |
| `preamble_strong`, `preamble_weak`                           | float          | framer          | least pulse-slot mean, greatest gap-slot mean, at the deciding offset                                       |
| `mode_s_format`                                              | Size_t         | framer, decoder | downlink format; the decoder rewrites it from the octets                                                    |
| `mode_s_icao`                                                | string         | decoder         | six hex digits; formats 17 and 18 (DF11 is short and is not decoded)                                        |
| `adsb_type_code`, `adsb_message`                             | Size_t, string | decoder         | formats 17/18; message names in `detail::messageOf`                                                         |
| `adsb_callsign`, `adsb_emitter_category`                     | string, Size_t | decoder         | type codes 1 to 4                                                                                           |
| `adsb_altitude_ft`, `adsb_altitude_q`                        | int32, bool    | decoder         | type codes 9 to 18                                                                                          |
| `adsb_cpr_format`, `adsb_cpr_latitude`, `adsb_cpr_longitude` | Size_t         | decoder         | this frame's encoded half, always                                                                           |
| `adsb_latitude`, `adsb_longitude`                            | double         | decoder         | only when an even/odd pair resolved                                                                         |
| `adsb_velocity_subtype`                                      | Size_t         | decoder         | type code 19                                                                                                |
| `adsb_ground_speed_kt`, `adsb_track_deg`                     | double         | decoder         | subtypes 1, 2, when both components are stated                                                              |
| `adsb_airspeed_kt`, `adsb_airspeed_type`                     | double, string | decoder         | subtypes 3, 4; type `ias` or `tas`                                                                          |
| `adsb_heading_deg`                                           | double         | decoder         | subtypes 3, 4, heading status set                                                                           |
| `adsb_vertical_rate_fpm`, `adsb_vertical_rate_source`        | int32, string  | decoder         | when stated; source `barometric` or `geometric`                                                             |

Tags never propagate; the framer moves the input tags inside a frame onto that record's
`timing_events`.

## Settings and counters

Each block's header block lists its settings (when a change applies, what a bad value does) and its
counters. All counters are plain members, read on the block's thread or after the graph stops. A
settings change on `PpmFramer` resets all its counters and held state. `app/adsb.cpp` prints nSamples,
nCandidates, nNominations, nAdmitted, nCrcFailed and nShortFormat at exit, with the console's line
count: `nNominations` low means threshold or rate; `nCrcFailed` high against `nAdmitted` means signal
quality. `nOverrunDropped` means the sink stopped taking records; `nTailDropped` is the undecided tail at
the end and at each rate change. Neither is printed.

## Common tasks

- **Decode another ME message.** `ModeS.hpp`: add its name to `detail::messageOf`, a branch in
  `readExtendedSquitter` writing `adsb_*` keys, the reader beside the others. `AdsbPrinter.hpp`:
  a branch in `detailOf`. Tests: a case in `qa_ModeSDecode.cpp` built from the standard's bit numbers
  with `Frame::put`, a line case in `qa_AdsbPrinter.cpp`, and the new keys in the boundary case's typed lists
  (`kStringKeys`, `kSizeKeys`, `kDoubleKeys`, `kIntegerKeys`) so they are checked across the packet
  encoder; a bool key has no list (`adsb_altitude_q` is in none), so add one with the key.
- **Change the rate rule or the framing.** `PpmFramer::slotsFor` is the one rate rule; the program's
  command-line check calls it. The framing constants live in core's `Ppm.hpp`, not here.
- **Add a front end.** A graph file in `graphs/` (copy `adsb-hackrf.yaml`) or a branch in
  `app/adsb.cpp`; the blocks do not change.
- **Build and test.** CMake against a GNU Radio 4 install on `CMAKE_PREFIX_PATH`; `BUILD_TESTING` (CTest,
  default on) and Boost.UT (an installed `ut` package or `-DGR_BOOST_UT_INCLUDE_DIR`) build the suites,
  which are skipped at configure without Boost.UT; `ctest` runs every suite. `GR4_RECORDINGS_DIR` points the two `recording`
  suites at the capture `ADSB_20260812_182544_1090000000_2000000_cs16.sigmf-data`; without it they exit
  77 and count as skipped. `qa_AdsbLabels` builds only where the core supports block labels.

## Invariants and traps

- The octets are never altered after the framer; the parity it checked covers what every later record
  carries. Decoded facts go in metadata, never in the items.
- Bit numbers follow the standard: bit 1 is the top bit of octet 0 (`detail::field`).
- An admitted frame consumes its whole window; any other nomination advances one sample. That is why
  the parity check sits in the framer.
- A position needs both halves from one address within `pair_seconds`; a lone half, a half with no
  `sample_start` and `sample_rate`, or a pair whose latitudes give different NL states no position, by
  design.
- `processBulk` must not return OK having consumed and published nothing; the scheduler reads that as
  a stall.
- A settings change on `PpmFramer` after `start()` restarts its index at zero: `sample_start` and tag
  indices then count from the change. The program and the graph files change no setting while running.
- A record that reaches `AdsbPrinter` with `mode_s_format` but no decode (only with `emit_short` or
  `emit_unchecked`) prints its DF and protocol, not its octets.
- `BlockLabels.hpp` content sits under `GR4ADSB_BLOCK_LABELS`; an older core carries an incompatible
  header under the same name.

## Domain

ICAO Annex 10 Volume IV (Mode S framing, the AC altitude code), RTCA DO-260B (extended squitter
messages, CPR position: A.1.7). 2 MS/s is one sample per half-microsecond slot; the program defaults
to 4 MS/s.
