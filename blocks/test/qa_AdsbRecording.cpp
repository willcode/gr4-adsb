/*| role: the whole ADS-B chain over a recorded 1090 MHz capture.
    why: the test has an executable of its own because its exit status is the whole binary's: an absent recording skips
        this test alone, and the synthetic tests still run. The graph holds GNU Radio 4's file source, converters and
        magnitude, then this package's framer, decoder and printer, the chain a consumer builds.
    why: the capture has no metadata file. Its name carries the rate and the format, the two facts the chain needs. The
        directory arrives as GR4_RECORDINGS_DIR. With the directory or the file absent the run prints the path it looked
        for and exits 77, which CTest counts as a skip.
    measured: 60.7 million samples through seven blocks, a few seconds in a release build. The CTest timeout of 300 s
        and the graph's own 240 s watchdog are margin for a slower machine.
*/
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

#include <adsb/AdsbPrinter.hpp>
#include <adsb/ModeS.hpp>
#include <adsb/PpmFramer.hpp>
#include <gnuradio-4.0/basic/ConverterBlocks.hpp>
#include <gnuradio-4.0/fileio/BasicFileIo.hpp>
#include <gnuradio-4.0/math/Unary.hpp>

namespace qa_adsb_recording {

using Record = gr::DataSet<std::uint8_t>;

/*| frame: ADS-B at 1090 MHz, 2 MS/s, interleaved little-endian int16 I/Q, 30.4 seconds.
 */
constexpr std::string_view kCapture = "ADSB_20260812_182544_1090000000_2000000_cs16.sigmf-data";
constexpr float            kRate    = 2.0e6f;

/*| role: what the capture decodes to: the time in seconds, and the line without its time field.
    why: the two values are checked apart because they answer different questions. The tail is the message — the format,
        the address and every field read out of it — and is exact. The time is where the framer put the frame's first
        payload sample, which is a property of the framing and not of this family, so it is held to a millisecond.
*/
struct Expected {
    double           seconds = 0.0;
    std::string_view tail{};
};

/*| role: both position frames carry a clear Q bit, so their altitudes are the 100 ft code and not the 25 ft field, and
        both are even halves, so no pair completes and no position is stated. The velocity frame is subtype 1.
*/
constexpr std::array<Expected, 3UZ> kExpected{{
    {2.767, "DF17  AA1BAA  airpos  alt 3600 ft"},
    {4.713, "DF17  AA1BAA  airpos  alt 3700 ft"},
    {14.546, "DF17  A9A901  veloc   gs 349 kt  track 103 deg  vs -1024 ft/min"},
}};

struct LineSink : gr::Block<LineSink> {
    gr::PortIn<Record, gr::Async> in;
    GR_MAKE_REFLECTABLE(LineSink, in);
    std::vector<std::string> _lines{};

    [[nodiscard]] gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        for (std::size_t k = 0UZ; k < inSpan.size(); ++k) {
            _lines.emplace_back(inSpan[k].signal_values.begin(), inSpan[k].signal_values.end());
        }
        const std::size_t taken = inSpan.size();
        std::ignore             = inSpan.consume(taken);
        return taken == 0UZ ? gr::work::Status::INSUFFICIENT_INPUT_ITEMS : gr::work::Status::OK;
    }
};

} // namespace qa_adsb_recording

using namespace qa_adsb_recording;

int main() {
    const char* const           configured = std::getenv("GR4_RECORDINGS_DIR");
    const std::filesystem::path path       = std::filesystem::path(configured == nullptr ? "" : configured) / kCapture;
    if (configured == nullptr || *configured == '\0' || !std::filesystem::exists(path)) {
        std::println("SKIP: no capture at '{}' — set GR4_RECORDINGS_DIR to the directory holding {}", path.string(), kCapture);
        return 77;
    }

    gr::Graph flow;
    auto&     source    = flow.emplaceBlock<gr::blocks::fileio::BasicFileSource<std::int16_t>>({{"file_name", path.string()}});
    auto&     widen     = flow.emplaceBlock<gr::blocks::basic::Convert<std::int16_t, float>>();
    auto&     pairs     = flow.emplaceBlock<gr::blocks::basic::InterleavedToComplex<float, std::complex<float>>>();
    auto&     magnitude = flow.emplaceBlock<gr::blocks::math::Abs<std::complex<float>>>();
    auto&     framer    = flow.emplaceBlock<adsb::PpmFramer<float>>({{"sample_rate", kRate}, {"profile", std::string("mode_s")}});
    auto&     decoder   = flow.emplaceBlock<adsb::ModeSDecode>();
    auto&     printer   = flow.emplaceBlock<adsb::AdsbPrinter>();
    auto&     sink      = flow.emplaceBlock<LineSink>();

    bool wired = flow.connect<"out", "in">(source, widen).has_value();
    wired      = wired && flow.connect<"out", "interleaved">(widen, pairs).has_value();
    wired      = wired && flow.connect<"out", "in">(pairs, magnitude).has_value();
    wired      = wired && flow.connect<"out", "in">(magnitude, framer).has_value();
    wired      = wired && flow.connect<"out", "in">(framer, decoder).has_value();
    wired      = wired && flow.connect<"out", "in">(decoder, printer).has_value();
    wired      = wired && flow.connect<"out", "in">(printer, sink).has_value();
    if (!wired) {
        std::println(stderr, "the graph could not be connected");
        return 1;
    }

    gr::scheduler::Simple<> scheduler;
    if (!scheduler.exchange(std::move(flow)).has_value()) {
        std::println(stderr, "the scheduler would not take the graph");
        return 1;
    }

    std::atomic<bool> done{false};
    std::thread       runner([&scheduler, &done] {
        if (const auto result = scheduler.runAndWait(); !result.has_value()) {
            std::println(stderr, "the graph stopped: {}", result.error().message);
        }
        done = true;
    });
    const auto        start = std::chrono::steady_clock::now();
    while (!done.load() && std::chrono::steady_clock::now() - start < std::chrono::seconds(240)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const bool finished = done.load();
    if (!finished) {
        scheduler.requestStop();
    }
    runner.join();

    const std::vector<std::string> lines = sink._lines;
    std::println("[record] {} at 2 MS/s: {} samples, {} nominations, {} admitted, {} crc failed, {} short format", //
        kCapture, framer.nSamples, framer.nNominations, framer.nAdmitted, framer.nCrcFailed, framer.nShortFormat);
    std::println("[record] decoder: {} frames, {} decoded, {} positions; printer: {} lines", decoder.nFrames, decoder.nDecoded, decoder.nPositions, printer.nLines);
    for (const std::string& line : lines) {
        std::println("[record] {}", line);
    }

    bool passed = finished;
    if (!finished) {
        std::println(stderr, "the graph did not finish within 240 seconds");
    }
    if (lines.size() != kExpected.size()) {
        std::println(stderr, "expected {} lines, got {}", kExpected.size(), lines.size());
        passed = false;
    }
    for (std::size_t k = 0UZ; k < kExpected.size() && k < lines.size(); ++k) {
        // the line is `SSSSS.SSS` then two spaces then the message; the time field is nine characters wide
        constexpr std::size_t kTimeWidth = 9UZ;
        if (lines[k].size() < kTimeWidth + 2UZ) {
            std::println(stderr, "line {} is too short to be a rendered frame: '{}'", k, lines[k]);
            passed = false;
            continue;
        }
        const std::string_view tail(lines[k].data() + kTimeWidth + 2UZ, lines[k].size() - kTimeWidth - 2UZ);
        if (tail != kExpected[k].tail) {
            std::println(stderr, "line {}: expected '{}', got '{}'", k, kExpected[k].tail, tail);
            passed = false;
        }
        const double seconds = std::strtod(lines[k].substr(0UZ, kTimeWidth).c_str(), nullptr);
        if (std::abs(seconds - kExpected[k].seconds) > 1.0e-3) {
            std::println(stderr, "line {}: expected {:.3f} s, got {:.3f} s", k, kExpected[k].seconds, seconds);
            passed = false;
        }
    }
    if (decoder.nDecoded != kExpected.size() || decoder.nUnchecked != 0ULL) {
        std::println(stderr, "the decoder read {} frames and refused {}; the framer publishes only admitted frames here", decoder.nDecoded, decoder.nUnchecked);
        passed = false;
    }
    return passed ? 0 : 1;
}
