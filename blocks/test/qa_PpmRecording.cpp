/*| role: the pulse-position scanner over a recorded 1090 MHz capture.
    why: the test has an executable of its own because its exit status is the whole binary's: an absent recording skips
        this test alone, and the synthetic tests still run. The capture has no metadata file, so the test reads the raw
        interleaved int16 pairs itself. The file name carries the rate and the format, and the receive rule needs only
        magnitudes and the rate.
    why: the directory arrives as GR4_RECORDINGS_DIR. With the directory or the file absent the run prints the path it
        looked for and exits 77, which CTest counts as a skip.
*/
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <gnuradio-4.0/algorithm/digital/Ppm.hpp>

namespace {

/*| frame: ADS-B at 1090 MHz, 2 MS/s, interleaved little-endian int16 I/Q, 30.4 seconds.
 */
constexpr std::string_view kCapture = "ADSB_20260812_182544_1090000000_2000000_cs16.sigmf-data";

/*| role: samples converted and handed to the scanner at a time; the undecided tail stays at the front of the next
        batch.
*/
constexpr std::size_t kBatch = 1UZ << 16U;

/*| role: what one pass over the capture at one threshold found.
 */
struct Sweep {
    std::uint64_t           samples     = 0ULL;
    std::uint64_t           nominations = 0ULL;
    std::uint64_t           admitted    = 0ULL;
    std::uint64_t           crcFailed   = 0ULL;
    std::uint64_t           shortFormat = 0ULL;
    std::uint64_t           format17    = 0ULL;
    std::uint64_t           format18    = 0ULL;
    std::uint64_t           refused17   = 0ULL; /*| role: refused long frames whose format field still reads 17 */
    std::uint64_t           refusedNear = 0ULL; /*| role: refused long frames whose remainder is one or two bit flips from zero */
    std::set<std::uint32_t> addresses{};
    std::set<std::uint32_t> refusedAddresses{}; /*| role: the addresses the refused long frames name, which the flips may have moved */
};

/*| role: remainders of the frames that differ from a correct one in one or two bits, mapped to nothing but themselves.
    why: the reduction is linear, so the remainder of a frame carrying errors is the sum of the remainders of those
        single bit errors alone; a refused frame whose remainder is in this set is that far from reducing to zero. It
        says how much of the capture the bit rule nearly reads, which is the number a change to that rule would be
        judged by.
*/
[[nodiscard]] const std::set<std::uint64_t>& nearMisses() {
    static const std::set<std::uint64_t> table = [] {
        gr::digital::PpmScanner scanner;
        scanner.prepare(gr::digital::modeS(), 1UZ);
        constexpr std::size_t      kBits = 112UZ;
        std::vector<std::uint64_t> single(kBits);
        for (std::size_t bit = 0UZ; bit < kBits; ++bit) {
            std::vector<std::uint8_t> frame(kBits / 8UZ, std::uint8_t{0});
            frame[bit / 8UZ] = static_cast<std::uint8_t>(0x80U >> (bit % 8UZ));
            single[bit]      = scanner.remainderOf(std::span<const std::uint8_t>(frame));
        }
        std::set<std::uint64_t> out(single.begin(), single.end());
        for (std::size_t i = 0UZ; i < kBits; ++i) {
            for (std::size_t j = i + 1UZ; j < kBits; ++j) {
                out.insert(single[i] ^ single[j]);
            }
        }
        return out;
    }();
    return table;
}

[[nodiscard]] Sweep sweep(const std::filesystem::path& path, float threshold) {
    std::ifstream capture(path, std::ios::binary);
    if (!capture) {
        std::println(stderr, "cannot read {}", path.string());
        return {};
    }

    gr::digital::PpmScanner scanner;
    scanner.prepare(gr::digital::modeS(), 1UZ); // 2 MS/s is one sample a half-microsecond slot
    scanner.threshold = threshold;

    Sweep                     result;
    std::vector<std::int16_t> raw(2UZ * kBatch);
    std::vector<float>        work;
    work.reserve(scanner.windowSamples() + kBatch);
    std::size_t workStart = 0UZ;

    const auto onFrame = [&result](const gr::digital::PpmFrame& frame) {
        const auto address = [&frame] { return (static_cast<std::uint32_t>(frame.octets[1UZ]) << 16U) | (static_cast<std::uint32_t>(frame.octets[2UZ]) << 8U) | static_cast<std::uint32_t>(frame.octets[3UZ]); };
        if (frame.outcome == gr::digital::PpmOutcome::CrcFailed) {
            if (frame.format == 17U) {
                ++result.refused17;
            }
            if (nearMisses().contains(frame.remainder)) {
                ++result.refusedNear;
            }
            result.refusedAddresses.insert(address());
            return;
        }
        if (!frame.admitted()) {
            return;
        }
        if (frame.format == 17U) {
            ++result.format17;
        } else if (frame.format == 18U) {
            ++result.format18;
        }
        result.addresses.insert(address());
    };

    while (capture) {
        capture.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size() * sizeof(std::int16_t)));
        const auto pairs = static_cast<std::size_t>(capture.gcount()) / (2UZ * sizeof(std::int16_t));
        if (pairs == 0UZ) {
            break;
        }
        for (std::size_t k = 0UZ; k < pairs; ++k) {
            const auto real = static_cast<float>(raw[2UZ * k]);
            const auto imag = static_cast<float>(raw[2UZ * k + 1UZ]);
            work.push_back(std::sqrt(real * real + imag * imag));
        }
        result.samples += pairs;

        scanner.seek(workStart);
        const std::size_t done = scanner.consume(std::span<const float>(work), onFrame).value();
        work.erase(work.begin(), work.begin() + static_cast<std::ptrdiff_t>(done));
        workStart += done;
    }

    result.nominations = scanner.counters.nominations;
    result.admitted    = scanner.counters.admitted;
    result.crcFailed   = scanner.counters.crcFailed;
    result.shortFormat = scanner.counters.shortFormat;
    return result;
}

void report(float threshold, const Sweep& found) {
    std::println("[record] threshold {:.2f}: nominations {}, admitted {} (format 17: {}, format 18: {}, other: {}), distinct addresses {}, crc failed {}, short format {}", //
        threshold, found.nominations, found.admitted, found.format17, found.format18, found.admitted - found.format17 - found.format18, found.addresses.size(), found.crcFailed, found.shortFormat);
    std::println("[record] threshold {:.2f}: of the {} refused long frames, {} read format 17, {} are within two bit flips of a zero remainder, and they name {} distinct addresses", //
        threshold, found.crcFailed, found.refused17, found.refusedNear, found.refusedAddresses.size());
}

} // namespace

int main() {
    const char* const           configured = std::getenv("GR4_RECORDINGS_DIR");
    const std::filesystem::path path       = std::filesystem::path(configured == nullptr ? "" : configured) / kCapture;
    if (configured == nullptr || *configured == '\0' || !std::filesystem::exists(path)) {
        std::println("SKIP: no capture at '{}' — set GR4_RECORDINGS_DIR to the directory holding {}", path.string(), kCapture);
        return 77;
    }

    const Sweep atDefault = sweep(path, 2.0F);
    std::println("[record] {} at 2 MS/s: {} samples ({:.1f} s)", kCapture, atDefault.samples, static_cast<double>(atDefault.samples) / 2.0e6);
    report(2.0F, atDefault);
    for (const float threshold : {1.2F, 1.5F, 3.0F}) {
        report(threshold, sweep(path, threshold));
    }

    bool passed = true;
    if (atDefault.admitted == 0ULL) {
        std::println(stderr, "a capture named for ADS-B at the canonical rate admitted no frame");
        passed = false;
    }
    if (atDefault.addresses.empty()) {
        std::println(stderr, "no admitted frame carried an address");
        passed = false;
    }
    return passed ? 0 : 1;
}
