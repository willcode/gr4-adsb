// adsb_print — the demo application: build the 1090 MHz receive chain, run it, print one line per decoded frame.
//
// Live:  SoapySource -> Abs -> PpmFramer -> ModeSDecode -> AdsbPrinter -> ConsoleSink
// File:  BasicFileSource<int16_t> -> InterleavedToComplex -> Abs -> (the same tail)
//
// Everything above Abs is upstream GNU Radio 4; the framer, the decoder and the renderer are this module's blocks.

#include <atomic>
#include <chrono>
#include <complex>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

#include <gnuradio-4.0/basic/ConverterBlocks.hpp>
#include <gnuradio-4.0/fileio/BasicFileIo.hpp>

#include <gr4adsb/adsb/AdsbPrinter.hpp>
#include <gr4adsb/adsb/ModeS.hpp>
#include <gr4adsb/digital/PpmFramer.hpp>

#ifdef GR4ADSB_HAVE_SOAPY
#include <gnuradio-4.0/sdr/SoapySource.hpp>
#endif

namespace {

std::atomic<bool> gStop{false};

extern "C" void onSignal(int) { gStop.store(true, std::memory_order_relaxed); }

/// @brief One text record in, one line on stdout. `AdsbPrinter` did the rendering; this only writes what it published.
struct ConsoleSink : gr::Block<ConsoleSink> {
    gr::PortIn<gr::DataSet<std::uint8_t>, gr::Async> in;

    GR_MAKE_REFLECTABLE(ConsoleSink, in);

    std::uint64_t nLines = 0ULL;

    [[nodiscard]] gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        for (const gr::DataSet<std::uint8_t>& record : inSpan) {
            std::fwrite(record.signal_values.data(), 1UZ, record.signal_values.size(), stdout);
            std::fputc('\n', stdout);
            ++nLines;
        }
        const std::size_t moved = inSpan.size();
        if (moved > 0UZ) {
            std::fflush(stdout);
        }
        std::ignore = inSpan.consume(moved);
        // frames are seconds apart on a quiet band, so an empty span is the normal case and has to say so
        return moved == 0UZ ? gr::work::Status::INSUFFICIENT_INPUT_ITEMS : gr::work::Status::OK;
    }
};

struct Options {
    std::string device{};              ///< the SoapySDR device string, e.g. "driver=hackrf"
    std::string file{};                ///< a recorded interleaved int16 capture, instead of a device
    double      frequency = 1090.0e6;  ///< Hz
    float       rate      = 4.0e6F;    ///< Hz; must be an even integer multiple of 1 MHz
    double      bandwidth = 5.0e6;     ///< Hz of analog RF bandwidth
    double      gain      = 40.0;      ///< dB of receive gain
    float       threshold = 2.0F;      ///< the framer's preamble nomination ratio
    double      seconds   = 0.0;       ///< run bound; 0 runs until the source ends or the user interrupts
};

void usage() {
    std::println(stderr, "usage: adsb_print (--device <soapy device> | --file <interleaved int16 capture>)");
    std::println(stderr, "  --device STR     SoapySDR device string, e.g. \"driver=hackrf\" or \"driver=uhd,type=b200\"");
    std::println(stderr, "  --file PATH      replay a recorded capture instead of opening a device");
    std::println(stderr, "  --frequency HZ   center frequency (default 1090e6)");
    std::println(stderr, "  --rate HZ        sample rate, an even integer multiple of 1 MHz (default 4e6)");
    std::println(stderr, "  --bandwidth HZ   analog RF bandwidth (default 5e6; a 1536 kHz filter admits nothing)");
    std::println(stderr, "  --gain DB        receive gain (default 40)");
    std::println(stderr, "  --threshold R    framer nomination ratio, greater than one (default 2)");
    std::println(stderr, "  --seconds N      stop after N seconds");
}

/// @brief @p text as a number, and nothing when it is not one in full — so "4e6x" is refused rather than read as 4e6.
[[nodiscard]] std::optional<double> number(std::string_view text) {
    try {
        std::size_t  used  = 0UZ;
        const double value = std::stod(std::string(text), &used);
        return used == text.size() ? std::optional<double>{value} : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

/// @brief The SoapySDR device string @p spec split the way `SoapySource` wants it: the driver name, then the rest.
///
/// The block takes the driver under `device` and merges the remaining comma-separated `key=value` pairs from
/// `device_parameter`, so "driver=uhd,type=b200" becomes ("uhd", "type=b200"). A string that names no driver key is
/// taken to be the driver itself, which is what "--device hackrf" means.
[[nodiscard]] std::pair<std::string, std::string> splitDevice(std::string_view spec) {
    std::string driver;
    std::string rest;
    for (std::size_t at = 0UZ; at <= spec.size();) {
        const std::size_t      comma = std::min(spec.find(',', at), spec.size());
        const std::string_view field = spec.substr(at, comma - at);
        at                           = comma + 1UZ;
        if (field.empty()) {
            continue;
        }
        if (field.starts_with("driver=")) {
            driver = std::string(field.substr(7UZ));
        } else {
            rest.append(rest.empty() ? "" : ",").append(field);
        }
    }
    if (driver.empty()) {
        return {std::string(spec), std::string{}};
    }
    return {driver, rest};
}

/// @brief The command line, or nothing when it is not usable. Everything it refuses is reported here.
[[nodiscard]] std::optional<Options> parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        const bool             hasValue = i + 1 < argc;
        const auto             text     = [&]() -> std::string_view { return hasValue ? std::string_view(argv[++i]) : std::string_view{}; };
        const auto             refuse   = [&arg](std::string_view what) { std::println(stderr, "adsb_print: {} needs {}", arg, what); };

        if (!hasValue && arg != "--help") {
            refuse("a value");
            return std::nullopt;
        }
        if (arg == "--device") {
            options.device = std::string(text());
        } else if (arg == "--file") {
            options.file = std::string(text());
        } else if (arg == "--frequency" || arg == "--rate" || arg == "--bandwidth" || arg == "--gain" || arg == "--threshold" || arg == "--seconds") {
            const std::optional<double> value = number(text());
            if (!value.has_value()) {
                refuse("a number");
                return std::nullopt;
            }
            if (arg == "--frequency") {
                options.frequency = *value;
            } else if (arg == "--rate") {
                options.rate = static_cast<float>(*value);
            } else if (arg == "--bandwidth") {
                options.bandwidth = *value;
            } else if (arg == "--gain") {
                options.gain = *value;
            } else if (arg == "--threshold") {
                options.threshold = static_cast<float>(*value);
            } else {
                options.seconds = *value;
            }
        } else {
            if (arg != "--help") {
                std::println(stderr, "adsb_print: unknown argument {}", arg);
            }
            return std::nullopt;
        }
    }

    if (options.device.empty() == options.file.empty()) {
        std::println(stderr, "adsb_print: give exactly one of --device and --file");
        return std::nullopt;
    }
    // the framer's own rule, asked of the framer, so the application and the block cannot disagree about it
    if (gr4adsb::PpmFramer::slotsFor(options.rate) == 0UZ) {
        std::println(stderr, "adsb_print: --rate {} is not an even integer multiple of 1 MHz, so a half-microsecond slot is not a whole number of samples", options.rate);
        return std::nullopt;
    }
    if (options.threshold <= 1.F) {
        std::println(stderr, "adsb_print: --threshold must be greater than one, got {}", options.threshold);
        return std::nullopt;
    }
    if (options.frequency <= 0.0 || options.bandwidth < 0.0 || options.seconds < 0.0) {
        std::println(stderr, "adsb_print: --frequency must be positive, and --bandwidth and --seconds must not be negative");
        return std::nullopt;
    }
    if (!options.file.empty() && !std::filesystem::exists(options.file)) {
        std::println(stderr, "adsb_print: {} does not exist", options.file);
        return std::nullopt;
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    const std::optional<Options> parsed = parse(argc, argv);
    if (!parsed.has_value()) {
        usage();
        return 2;
    }
    const Options& options = *parsed;

    gr::Graph flow;
    auto&     magnitude = flow.emplaceBlock<gr::blocks::basic::Abs<std::complex<float>>>();
    auto&     framer    = flow.emplaceBlock<gr4adsb::PpmFramer>({
               {"sample_rate", options.rate},
               {"profile", std::string("mode_s")},
               {"threshold", options.threshold},
    });
    auto&     decoder   = flow.emplaceBlock<gr4adsb::ModeSDecode>();
    auto&     printer   = flow.emplaceBlock<gr4adsb::AdsbPrinter>();
    auto&     console   = flow.emplaceBlock<ConsoleSink>();

    bool wired = true;
    if (!options.file.empty()) {
        // A capture is interleaved int16 and the file source hands those out as they lie; pairing them into complex
        // is the only conversion needed. No scaling block belongs here: the framer nominates on a ratio and decides
        // bits by comparison, so raw converter counts feed it as well as normalized floats do. The run is as fast as
        // the file reads — nothing paces it.
        auto& source = flow.emplaceBlock<gr::blocks::fileio::BasicFileSource<std::int16_t>>({{"file_name", options.file}});
        auto& pair   = flow.emplaceBlock<gr::blocks::basic::InterleavedToComplex<std::int16_t, std::complex<float>>>();
        std::println(stderr, "adsb_print: {} at {:.3f} MS/s, threshold {}", options.file, static_cast<double>(options.rate) / 1e6, options.threshold);
        wired = flow.connect<"out", "interleaved">(source, pair).has_value() && flow.connect<"out", "in">(pair, magnitude).has_value();
    } else {
#ifdef GR4ADSB_HAVE_SOAPY
        const auto [driver, parameters] = splitDevice(options.device);
        auto& source                    = flow.emplaceBlock<gr::blocks::sdr::SoapySource<std::complex<float>, 1UZ>>({
                               {"device", driver},
                               {"device_parameter", parameters},
                               {"sample_rate", options.rate},
                               {"frequency", std::vector{options.frequency}},
                               {"rx_gains", std::vector{options.gain}},
                               {"rx_bandwidths", std::vector{options.bandwidth}},
        });
        std::println(stderr, "adsb_print: {} at {:.4f} MHz, {:.3f} MS/s, gain {} dB, bandwidth {:.3f} MHz, threshold {}", options.device, options.frequency / 1e6, static_cast<double>(options.rate) / 1e6, options.gain, options.bandwidth / 1e6, options.threshold);
        wired = flow.connect<"out", "in">(source, magnitude).has_value();
#else
        std::println(stderr, "adsb_print: built without SoapySDR, so only --file is available; see the README under Building");
        return 1;
#endif
    }

    wired = wired                                                              //
            && flow.connect<"abs", "in">(magnitude, framer).has_value()         //
            && flow.connect<"out", "in">(framer, decoder).has_value()           //
            && flow.connect<"out", "in">(decoder, printer).has_value()          //
            && flow.connect<"out", "in">(printer, console).has_value();
    if (!wired) {
        std::println(stderr, "adsb_print: the graph could not be connected");
        return 1;
    }

    std::signal(SIGINT, onSignal);

    gr::scheduler::Simple<> scheduler;
    if (!scheduler.exchange(std::move(flow)).has_value()) {
        std::println(stderr, "adsb_print: the scheduler refused the graph");
        return 1;
    }

    // The scheduler's result carries the reason a block refused to start — the framer throws on a rate it cannot
    // slot, for instance — so a run that decodes nothing still says why.
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::thread       runner([&scheduler, &done, &failed] {
        if (const auto result = scheduler.runAndWait(); !result.has_value()) {
            std::println(stderr, "adsb_print: the graph stopped: {}", result.error().message);
            failed.store(true, std::memory_order_relaxed);
        }
        done.store(true, std::memory_order_relaxed);
    });

    const auto start = std::chrono::steady_clock::now();
    while (!done.load(std::memory_order_relaxed) && !gStop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (options.seconds > 0.0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= options.seconds) {
            break;
        }
    }
    if (!done.load(std::memory_order_relaxed)) {
        scheduler.requestStop();
    }
    runner.join();

    std::println(stderr, "adsb_print: {} samples, {} nominations -> {} admitted, {} crc failed, {} short; {} lines", framer.nSamples, framer.nNominations, framer.nAdmitted, framer.nCrcFailed, framer.nShortFormat, console.nLines);
    return failed.load(std::memory_order_relaxed) ? 1 : 0;
}
