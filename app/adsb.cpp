/*| role: the adsb program: builds the 1090 MHz receive chain, runs it, and prints one line per decoded frame.
    frame: live: SoapySource -> PpmFramer<complex<float>> -> ModeSDecode -> AdsbPrinter -> ConsoleSink.
        file: BasicFileSource<int16_t> -> InterleavedToComplex -> the same tail. PpmFramer, ModeSDecode and
        AdsbPrinter are this package's (blocks/); the rest is GNU Radio 4 as installed. ConsoleSink, below, is
        the only block that assumes a terminal.
    why: the framer takes complex samples, not magnitudes, because its sub-sample phase search interpolates them.
    contract: exit status 2 for an unusable command line, 1 when the graph cannot be built or stops on an error,
        0 otherwise. The closing stderr lines report the framer's counts and whether the graph ended itself.
*/

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <iterator>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

#include <adsb/AdsbPrinter.hpp>
#include <adsb/ModeS.hpp>
#include <adsb/PpmFramer.hpp>

#include <gnuradio-4.0/basic/ConverterBlocks.hpp>
#include <gnuradio-4.0/fileio/BasicFileIo.hpp>

#ifdef GR4ADSB_HAVE_SOAPY
#include <gnuradio-4.0/sdr/SoapySource.hpp>
#endif

namespace {

std::atomic<bool> gStop{false};

extern "C" void onSignal(int) { gStop.store(true, std::memory_order_relaxed); }

/*| role: writes each text record's items to stdout as one line; AdsbPrinter has already rendered it.
 */
struct ConsoleSink : gr::Block<ConsoleSink> {
    gr::PortIn<gr::DataSet<std::uint8_t>, gr::Async> in;

    GR_MAKE_REFLECTABLE(ConsoleSink, in);

    std::uint64_t nLines = 0ULL;

    [[nodiscard]] gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        const std::size_t moved = inSpan.size();
        for (std::size_t i = 0UZ; i < moved; ++i) {
            const gr::DataSet<std::uint8_t>& record = inSpan[i];
            std::fwrite(record.signal_values.data(), 1UZ, record.signal_values.size(), stdout);
            std::fputc('\n', stdout);
            ++nLines;
        }
        if (moved > 0UZ) {
            std::fflush(stdout);
        }
        std::ignore = inSpan.consume(moved);
        /*| trap: an empty span returns INSUFFICIENT_INPUT_ITEMS; OK would report progress that was not made.
         */
        return moved == 0UZ ? gr::work::Status::INSUFFICIENT_INPUT_ITEMS : gr::work::Status::OK;
    }
};

/*| role: the parsed command line; usage() lists each option.
    frame: frequency, rate and bandwidth in Hz, gain in dB, seconds of wall-clock run time (0: until the source
        ends or SIGINT). device and file are exclusive. pluginDirs is where --verbose loads from; parse() fills in
        this package's and the SDK's plugin directories when none is given.
    trap: an unset gain or bandwidth must reach SoapySource as an empty vector, never as an absent key: the block
        applies every setting it holds when it opens the device, and its own defaults are 10 dB of gain and
        500 kHz of bandwidth. A device can refuse the whole activation over a combination it cannot serve; an
        SDRplay does, with `Init() failed: sdrplay_api_Fail`.
*/
struct Options {
    std::string device{};
    std::string file{};
    double      frequency = 1090.0e6;
    float       rate      = 4.0e6F;
    float       threshold = 2.0F;
    double      seconds   = 0.0;

    std::uint32_t phaseSteps  = 8U;
    float         coarseRatio = 0.5F;

    std::optional<double> gain{};
    std::optional<double> bandwidth{};
    bool                  agc = false;

    bool                     verbose = false;
    std::vector<std::string> pluginDirs{};
};

void usage() {
    std::println(stderr, "usage: adsb (--device <soapy device> | --file <interleaved int16 capture>)");
    std::println(stderr, "  --device STR     SoapySDR device string, e.g. \"driver=hackrf\" or \"driver=uhd,type=b200\"");
    std::println(stderr, "  --file PATH      replay a recorded capture instead of opening a device");
    std::println(stderr, "  --frequency HZ   center frequency (default 1090e6)");
    std::println(stderr, "  --rate HZ        sample rate, an even integer multiple of 1 MHz (default 4e6)");
    std::println(stderr, "  --bandwidth HZ   analog RF bandwidth (default: the device's own)");
    std::println(stderr, "  --gain DB        receive gain (default: the device's own)");
    std::println(stderr, "  --agc on|off     ask for the device's automatic gain control (default off)");
    std::println(stderr, "  --threshold R    framer nomination ratio, greater than one (default 2)");
    std::println(stderr, "  --phase-steps N  sub-sample offsets each candidate is re-tested at; 1 turns the search off (default 8)");
    std::println(stderr, "  --coarse-ratio R share of the threshold the integer-position test admits at, in (0, 1] (default 0.5)");
    std::println(stderr, "  --seconds N      stop after N seconds");
    std::println(stderr, "  --verbose        load the block libraries and print the registry keys they add, then run");
    std::println(stderr, "  --plugin-dir DIR directory --verbose loads from; repeatable (default: this package's and the SDK's)");
}

/*| contract: text as a number, or nothing unless the whole string is one: "4e6x" is refused, not read as 4e6.
 */
[[nodiscard]] std::optional<double> number(std::string_view text) {
    try {
        std::size_t  used  = 0UZ;
        const double value = std::stod(std::string(text), &used);
        return used == text.size() ? std::optional<double>{value} : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

#ifdef GR4ADSB_HAVE_SOAPY
/*| contract: splits a SoapySDR device string as SoapySource takes it: the driver for `device`, the other
        comma-separated key=value pairs for `device_parameter`. "driver=uhd,type=b200" gives ("uhd", "type=b200");
        a string with no driver key is the driver itself.
    trap: guarded with its one caller; unused, it is an error under this package's -Werror.
*/
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
#endif

/*| contract: the options, or nothing when the command line is not usable; each refusal is reported on stderr.
        The rate is checked with PpmFramer::slotsFor(), so the framer's rule is not restated here.
*/
[[nodiscard]] std::optional<Options> parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        const bool             hasValue = i + 1 < argc;
        const auto             text     = [&]() -> std::string_view { return hasValue ? std::string_view(argv[++i]) : std::string_view{}; };
        const auto             refuse   = [&arg](std::string_view what) { std::println(stderr, "adsb: {} needs {}", arg, what); };

        if (arg == "--verbose") {
            options.verbose = true;
            continue;
        }
        if (!hasValue && arg != "--help") {
            refuse("a value");
            return std::nullopt;
        }
        if (arg == "--device") {
            options.device = std::string(text());
        } else if (arg == "--file") {
            options.file = std::string(text());
        } else if (arg == "--plugin-dir") {
            options.pluginDirs.emplace_back(text());
        } else if (arg == "--agc") {
            const std::string_view value = text();
            if (value != "on" && value != "off") {
                refuse("on or off");
                return std::nullopt;
            }
            options.agc = value == "on";
        } else if (arg == "--frequency" || arg == "--rate" || arg == "--bandwidth" || arg == "--gain" || arg == "--threshold" || arg == "--phase-steps" || arg == "--coarse-ratio" || arg == "--seconds") {
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
            } else if (arg == "--phase-steps") {
                if (*value < 1.0 || *value > 1024.0 || *value != std::floor(*value)) {
                    refuse("a whole number of offsets, at least one and at most 1024");
                    return std::nullopt;
                }
                options.phaseSteps = static_cast<std::uint32_t>(*value);
            } else if (arg == "--coarse-ratio") {
                options.coarseRatio = static_cast<float>(*value);
            } else {
                options.seconds = *value;
            }
        } else {
            if (arg != "--help") {
                std::println(stderr, "adsb: unknown argument {}", arg);
            }
            return std::nullopt;
        }
    }

    if (options.device.empty() == options.file.empty()) {
        std::println(stderr, "adsb: give exactly one of --device and --file");
        return std::nullopt;
    }
    if (adsb::PpmFramer<std::complex<float>>::slotsFor(options.rate) == 0UZ) {
        std::println(stderr, "adsb: --rate {} is not an even integer multiple of 1 MHz, so a half-microsecond slot is not a whole number of samples", options.rate);
        return std::nullopt;
    }
    if (options.threshold <= 1.F) {
        std::println(stderr, "adsb: --threshold must be greater than one, got {}", options.threshold);
        return std::nullopt;
    }
    if (!(options.coarseRatio > 0.F) || options.coarseRatio > 1.F) {
        std::println(stderr, "adsb: --coarse-ratio must lie in (0, 1], got {}", options.coarseRatio);
        return std::nullopt;
    }
    if (options.frequency <= 0.0 || options.seconds < 0.0 || options.bandwidth.value_or(1.0) <= 0.0) {
        std::println(stderr, "adsb: --frequency and --bandwidth must be positive, and --seconds must not be negative");
        return std::nullopt;
    }
    if (!options.file.empty() && !std::filesystem::exists(options.file)) {
        std::println(stderr, "adsb: {} does not exist", options.file);
        return std::nullopt;
    }
    if (options.pluginDirs.empty()) {
        /*| frame: GR4ADSB_PLUGIN_DIR holds this package's block library and GR4ADSB_SDK_PLUGIN_DIR every other
                family; both come from the install the build was configured for, so an installed program needs no
                --plugin-dir.
        */
        options.pluginDirs.assign({std::string(GR4ADSB_PLUGIN_DIR), std::string(GR4ADSB_SDK_PLUGIN_DIR)});
    }
    return options;
}

/*| contract: loads the shared block libraries in directories and prints to stderr the registry keys they add:
        the names a graph built from a saved description would use. The chain in main() uses emplaceBlock<T> and
        registers nothing, so every printed key came from a loaded library.
    invariant: the libraries stay mapped after the loader is destroyed: gr::PluginLoader keeps a shared object
        that registered blocks mapped for the life of the process, since the registry holds factory pointers
        into it.
*/
void reportPlugins(const std::vector<std::string>& directories) {
    gr::BlockRegistry&       registry = gr::globalBlockRegistry();
    std::vector<std::string> before   = registry.keys();
    std::ranges::sort(before);

    const gr::PluginLoader loader(registry, gr::globalSchedulerRegistry(), directories);

    for (const std::string& directory : directories) {
        std::println(stderr, "adsb: plugin directory {}", directory);
    }
    for (const gr::PluginLoader::BlockLibrary& library : loader.blockLibraries()) {
        std::println(stderr, "adsb: loaded {} ({} registrations)", library.file, library.nBlockRegistrations);
    }
    for (const auto& plugin : loader.plugins()) {
        std::println(stderr, "adsb: loaded plugin \"{}\" {} with {} blocks", plugin->metadata.plugin_name, plugin->metadata.plugin_version, plugin->availableBlocks().size());
    }
    for (const auto& [file, reason] : loader.failedPlugins()) {
        std::println(stderr, "adsb: {} did not load: {}", file, reason);
    }
    for (const std::string& file : loader.skippedFiles()) {
        std::println(stderr, "adsb: {} was not opened; its name only reads as a shared object", file);
    }

    std::vector<std::string> after = registry.keys();
    std::ranges::sort(after);
    std::vector<std::string> added;
    std::ranges::set_difference(after, before, std::back_inserter(added));
    if (added.empty()) {
        std::println(stderr, "adsb: nothing in those directories registered a block");
        return;
    }
    std::println(stderr, "adsb: the registry gained {} keys:", added.size());
    for (const std::string& key : added) {
        std::println(stderr, "adsb:   {}", key);
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::optional<Options> parsed = parse(argc, argv);
    if (!parsed.has_value()) {
        usage();
        return 2;
    }
    const Options& options = *parsed;

    if (options.verbose) {
        reportPlugins(options.pluginDirs);
    }

    gr::Graph flow;
    auto&     framer  = flow.emplaceBlock<adsb::PpmFramer<std::complex<float>>>({
        {"sample_rate", options.rate},
        {"profile", std::string("mode_s")},
        {"threshold", options.threshold},
        {"phase_steps", static_cast<gr::Size_t>(options.phaseSteps)},
        {"coarse_ratio", options.coarseRatio},
    });
    auto&     decoder = flow.emplaceBlock<adsb::ModeSDecode>();
    auto&     printer = flow.emplaceBlock<adsb::AdsbPrinter>();
    auto&     console = flow.emplaceBlock<ConsoleSink>();

    bool wired = true;
    if (!options.file.empty()) {
        /*| why: a capture is interleaved int16, and pairing into complex is the only conversion needed: the framer
                nominates on a ratio and decides bits by comparison, so raw counts serve as well as normalized
                floats.
            frame: nothing paces a file run; it goes as fast as the file reads.
        */
        auto& source = flow.emplaceBlock<gr::blocks::fileio::BasicFileSource<std::int16_t>>({{"file_name", options.file}});

        /*| trap: the file source sizes each read from its output port's max_samples. Unbounded, the port reports
                SIZE_MAX, which no read can publish, so the bound is required.
            why: a read is half the edge's 64 ki int16 (33 ms at 2 MS/s): a read the size of the edge could only be
                published into an empty ring, while a half lets one read be consumed as the next arrives.
        */
        constexpr std::size_t kReaderRing = 1UZ << 16U;
        source.out.max_samples            = kReaderRing / 2UZ;

        auto& pair = flow.emplaceBlock<gr::blocks::basic::InterleavedToComplex<std::int16_t, std::complex<float>>>();
        std::println(stderr, "adsb: {} at {:.3f} MS/s, threshold {}, {} phase steps at a coarse ratio of {}", options.file, static_cast<double>(options.rate) / 1e6, options.threshold, options.phaseSteps, options.coarseRatio);
        wired = flow.connect<"out", "interleaved">(source, pair, gr::EdgeParameters{.minBufferSize = kReaderRing}).has_value() && flow.connect<"out", "in">(pair, framer).has_value();
    } else {
#ifdef GR4ADSB_HAVE_SOAPY
        const auto [driver, parameters] = splitDevice(options.device);

        /*| contract: rate and frequency are always set; gain_mode only with --agc on, since true is the one value
                the block acts on.
        */
        gr::property_map settings{
            {"device", driver},
            {"device_parameter", parameters},
            {"sample_rate", options.rate},
            {"frequency", std::vector{options.frequency}},
            {"rx_gains", options.gain.has_value() ? std::vector{*options.gain} : std::vector<double>{}},
            {"rx_bandwidths", options.bandwidth.has_value() ? std::vector{*options.bandwidth} : std::vector<double>{}},
        };
        if (options.agc) {
            settings.insert_or_assign(gr::property_map::key_type("gain_mode"), gr::pmt::Value(true));
        }
        auto& source = flow.emplaceBlock<gr::blocks::sdr::SoapySource<std::complex<float>, 1UZ>>(std::move(settings));

        /*| contract: the startup lines name every setting asked of the device and every one left at its default.
         */
        std::string asked;
        std::string left;
        const auto  note = [](std::string& into, std::string_view what) { into.append(into.empty() ? "" : ", ").append(what); };
        options.gain.has_value() ? note(asked, std::format("gain {} dB", *options.gain)) : note(left, "gain");
        options.bandwidth.has_value() ? note(asked, std::format("bandwidth {:.3f} MHz", *options.bandwidth / 1e6)) : note(left, "bandwidth");
        options.agc ? note(asked, "AGC on") : note(left, "gain mode");
        std::println(stderr, "adsb: {} at {:.4f} MHz, {:.3f} MS/s, threshold {}, {} phase steps at a coarse ratio of {}", options.device, options.frequency / 1e6, static_cast<double>(options.rate) / 1e6, options.threshold, options.phaseSteps, options.coarseRatio);
        std::println(stderr, "adsb: asking the device for: rate, frequency{}{}", asked.empty() ? "" : ", ", asked);
        std::println(stderr, "adsb: left at the device's default: {}", left.empty() ? std::string("nothing") : left);

        wired = flow.connect<"out", "in">(source, framer).has_value();
#else
        std::println(stderr, "adsb: built without SoapySDR, so only --file is available; see the README under Building");
        return 1;
#endif
    }

    wired = wired                                                      //
            && flow.connect<"out", "in">(framer, decoder).has_value()  //
            && flow.connect<"out", "in">(decoder, printer).has_value() //
            && flow.connect<"out", "in">(printer, console).has_value();
    if (!wired) {
        std::println(stderr, "adsb: the graph could not be connected");
        return 1;
    }

    std::signal(SIGINT, onSignal);

    gr::scheduler::Simple<> scheduler;
    if (!scheduler.exchange(std::move(flow)).has_value()) {
        std::println(stderr, "adsb: the scheduler refused the graph");
        return 1;
    }

    /*| contract: runAndWait()'s error carries the exception a block raised at start, such as the framer's refusal
            of a rate that does not divide into whole slots; it is printed and the exit status is 1.
    */
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::thread       runner([&scheduler, &done, &failed] {
        if (const auto result = scheduler.runAndWait(); !result.has_value()) {
            std::println(stderr, "adsb: the graph stopped: {}", result.error().message);
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
    const bool endedItself = done.load(std::memory_order_relaxed);
    runner.join();

    std::println(stderr, "adsb: {} samples, {} candidates -> {} nominations -> {} admitted, {} crc failed, {} short; {} lines", framer.nSamples, framer.nCandidates, framer.nNominations, framer.nAdmitted, framer.nCrcFailed, framer.nShortFormat, console.nLines);
    std::println(stderr, "adsb: {}", endedItself ? "the graph ended on its own" : "stopped before the graph ended");
    return failed.load(std::memory_order_relaxed) ? 1 : 0;
}
