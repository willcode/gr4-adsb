#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

#include <gr4adsb/digital/PpmFramer.hpp>

#include "TestSpans.hpp"

namespace {

using gr4adsb::PpmCounters;
using gr4adsb::PpmFrame;
using gr4adsb::PpmFramer;
using gr4adsb::PpmOutcome;
using gr4adsb::PpmScanner;
using gr4adsb::test::InputSpan;
using gr4adsb::test::OutputSpan;

using Record = gr::DataSet<std::uint8_t>;

/// The five published 1090 MHz extended squitters, whose parity fields are the bare CRC.
constexpr std::array<std::string_view, 5> kPublished{"8D4840D6202CC371C32CE0576098", "8D40621D58C382D690C8AC2863A7", "8D40621D58C386435CC412692AD6", "8D485020994409940838175B284F", "8D40621D58C3862590C412D77427"};

/// The one this suite anchors on; downlink format 17, address 0x4840D6.
constexpr std::string_view kAnchor = kPublished[0UZ];

/// The preamble in half-microsecond slots: pulses at 0.0, 1.0, 3.5 and 4.5 microseconds.
constexpr std::array<int, 16> kPreamble{1, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0};

[[nodiscard]] std::vector<std::uint8_t> octetsOf(std::string_view hex) {
    std::vector<std::uint8_t> octets;
    octets.reserve(hex.size() / 2UZ);
    const auto nibble = [](char c) { return static_cast<std::uint8_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10); };
    for (std::size_t i = 0UZ; i + 1UZ < hex.size(); i += 2UZ) {
        octets.push_back(static_cast<std::uint8_t>((nibble(hex[i]) << 4U) | nibble(hex[i + 1UZ])));
    }
    return octets;
}

/// @brief The samples one frame occupies: sixteen preamble slots, then two slots a bit, each slot @p slot samples.
[[nodiscard]] std::vector<float> render(std::span<const std::uint8_t> frame, std::size_t slot = 1UZ) {
    const std::size_t  bits = frame.size() * 8UZ;
    std::vector<float> wave;
    wave.reserve((16UZ + 2UZ * bits) * slot);
    const auto emit = [&wave, slot](float value) { wave.insert(wave.end(), slot, value); };
    for (const int pulse : kPreamble) {
        emit(pulse != 0 ? 1.F : 0.F);
    }
    for (std::size_t bit = 0UZ; bit < bits; ++bit) {
        const bool one = ((frame[bit / 8UZ] >> (7U - bit % 8UZ)) & 1U) != 0U;
        emit(one ? 1.F : 0.F);
        emit(one ? 0.F : 1.F);
    }
    return wave;
}

void append(std::vector<float>& into, std::span<const float> more) { into.insert(into.end(), more.begin(), more.end()); }
void appendZeros(std::vector<float>& into, std::size_t count) { into.insert(into.end(), count, 0.F); }

/// @brief One frame followed by a long frame's worth of silence, which is what a position needs before it is decided.
[[nodiscard]] std::vector<float> padded(std::span<const std::uint8_t> frame, std::size_t slot = 1UZ) {
    std::vector<float> wave = render(frame, slot);
    appendZeros(wave, (16UZ + 224UZ) * slot);
    return wave;
}

/// @brief A frame whose trailing parity field is the bare CRC over its message octets, built from @p message.
[[nodiscard]] std::vector<std::uint8_t> withParity(std::vector<std::uint8_t> message, std::size_t octets, std::uint64_t addressXor = 0ULL) {
    PpmScanner scanner;
    scanner.prepare(gr4adsb::modeS(), 1UZ);
    message.resize(octets, std::uint8_t{0});
    const std::uint64_t parity = scanner.remainderOf(std::span<const std::uint8_t>(message)) ^ addressXor;
    message[octets - 3UZ]      = static_cast<std::uint8_t>(parity >> 16U);
    message[octets - 2UZ]      = static_cast<std::uint8_t>(parity >> 8U);
    message[octets - 1UZ]      = static_cast<std::uint8_t>(parity);
    return message;
}

/// One nomination as the kernel reported it, with the octets copied out of the scanner.
struct Seen {
    std::vector<std::uint8_t> octets{};
    std::size_t               bits      = 0UZ;
    std::uint32_t             format    = 0U;
    std::uint64_t             remainder = 0ULL;
    float                     strong    = 0.F;
    float                     weak      = 0.F;
    std::size_t               position  = 0UZ;
    PpmOutcome                outcome   = PpmOutcome::Admitted;
};

/// What one kernel run reported and counted.
struct Scanned {
    std::vector<Seen> frames{};
    PpmCounters       counters{};
};

/**
 * @brief Drives the kernel over @p data presenting @p chunk fresh samples at a time.
 *
 * The undecided tail is presented again with whatever arrives next, which is exactly the contract `consume()`
 * states and what the block does with its own working buffer.
 */
[[nodiscard]] Scanned scan(std::span<const float> data, std::size_t chunk = 0UZ, std::size_t slot = 1UZ, float threshold = 2.F) {
    PpmScanner scanner;
    scanner.prepare(gr4adsb::modeS(), slot);
    scanner.threshold = threshold;

    Scanned           result;
    const std::size_t stride = chunk == 0UZ ? std::max(data.size(), 1UZ) : chunk;
    std::size_t       base   = 0UZ;
    std::size_t       fed    = std::min(data.size(), stride);
    while (true) {
        scanner.seek(base);
        const std::size_t done = scanner.consume(data.subspan(base, fed - base), [&result](const PpmFrame& frame) { result.frames.push_back({std::vector<std::uint8_t>(frame.octets.begin(), frame.octets.end()), frame.bits, frame.format, frame.remainder, frame.strong, frame.weak, frame.position, frame.outcome}); });
        base += done;
        if (fed == data.size()) {
            if (done == 0UZ) {
                break;
            }
        } else {
            fed = std::min(data.size(), fed + stride);
        }
    }
    result.counters = scanner.counters;
    return result;
}

template<typename TBlock>
[[nodiscard]] TBlock make(gr::property_map settings) {
    TBlock block(std::move(settings));
    block.settings().init();
    std::ignore = block.settings().applyStagedParameters();
    block.start();
    return block;
}

/// The settings every block case shares, at the 2 MS/s the canonical receivers deliver.
[[nodiscard]] gr::property_map modeSSettings(float rate = 2.0e6F) { return {{"profile", std::string("mode_s")}, {"sample_rate", rate}}; }

/// What one drive of the block produced, with the counters it reported.
struct Framed {
    std::vector<Record> records{};
    std::uint64_t       samples = 0ULL, nominations = 0ULL, admitted = 0ULL, crcFailed = 0ULL, shortFormat = 0ULL, published = 0ULL, tagsDropped = 0ULL, rateRefused = 0ULL, tailDropped = 0ULL, overrunDropped = 0ULL;
    std::size_t         held = 0UZ; ///< samples still in the working buffer when the run ended, before stop() drops them
};

/// @brief Drives the block over @p input in windows of @p chunk samples, with @p tags planted at absolute offsets.
///
/// @p room is the records the sink offers per call, and @p connected drives the block with its output port left
/// unconnected, which is what a graph does when nothing is wired to `out`.
[[nodiscard]] Framed feed(PpmFramer& block, std::span<const float> input, std::size_t chunk = 0UZ, std::span<const gr::Tag> tags = {}, std::size_t room = 64UZ, bool connected = true) {
    Framed              result;
    const std::size_t   stride = chunk == 0UZ ? std::max(input.size(), 1UZ) : chunk;
    std::vector<Record> scratch(room);

    for (std::size_t base = 0UZ; base < input.size();) {
        const std::size_t count = std::min(stride, input.size() - base);
        const auto        first = std::ranges::lower_bound(tags, base, std::ranges::less{}, &gr::Tag::index);
        const auto        last  = std::ranges::lower_bound(tags, base + count, std::ranges::less{}, &gr::Tag::index);

        InputSpan<float>   inSpan(input.subspan(base, count), base, std::span<const gr::Tag>(first, last));
        OutputSpan<Record> outSpan(connected ? std::span<Record>(scratch) : std::span<Record>{}, 0UZ, nullptr, connected);
        std::ignore = block.processBulk(inSpan, outSpan);
        for (std::size_t k = 0UZ; k < outSpan.count; ++k) {
            result.records.push_back(std::move(scratch[k]));
        }
        if (inSpan.consumed == 0UZ && outSpan.count == 0UZ) {
            break;
        }
        base += inSpan.consumed;
    }
    result.held = block._work.size();
    block.stop();

    result.samples        = block.nSamples;
    result.nominations    = block.nNominations;
    result.admitted       = block.nAdmitted;
    result.crcFailed      = block.nCrcFailed;
    result.shortFormat    = block.nShortFormat;
    result.published      = block.nPublished;
    result.tagsDropped    = block.nTagsDropped;
    result.rateRefused    = block.nRateRefused;
    result.tailDropped    = block.nTailDropped;
    result.overrunDropped = block.nOverrunDropped;
    return result;
}

[[nodiscard]] const gr::property_map& metaOf(const Record& record) {
    static const gr::property_map kEmpty{};
    return record.meta_information.empty() ? kEmpty : record.meta_information.front();
}

[[nodiscard]] std::uint64_t metaU64(const Record& record, std::string_view key) {
    const auto& map   = metaOf(record);
    const auto  entry = map.find(gr::property_map::key_type(key));
    return entry == map.end() ? ~0ULL : entry->second.value_or(std::uint64_t{~0ULL});
}

[[nodiscard]] gr::Size_t metaSize(const Record& record, std::string_view key) {
    const auto& map   = metaOf(record);
    const auto  entry = map.find(gr::property_map::key_type(key));
    return entry == map.end() ? std::numeric_limits<gr::Size_t>::max() : entry->second.value_or(std::numeric_limits<gr::Size_t>::max());
}

[[nodiscard]] std::string metaString(const Record& record, std::string_view key) {
    const auto& map   = metaOf(record);
    const auto  entry = map.find(gr::property_map::key_type(key));
    return entry == map.end() ? std::string{} : std::string(entry->second.value_or(std::string_view{}));
}

[[nodiscard]] float metaFloat(const Record& record, std::string_view key) {
    const auto& map   = metaOf(record);
    const auto  entry = map.find(gr::property_map::key_type(key));
    return entry == map.end() ? -1.F : entry->second.value_or(-1.F);
}

[[nodiscard]] bool metaBool(const Record& record, std::string_view key) {
    const auto& map   = metaOf(record);
    const auto  entry = map.find(gr::property_map::key_type(key));
    return entry != map.end() && entry->second.value_or(false);
}

[[nodiscard]] bool metaHas(const Record& record, std::string_view key) { return metaOf(record).contains(gr::property_map::key_type(key)); }

struct FiniteSource : gr::Block<FiniteSource> {
    gr::PortOut<float> out;
    GR_MAKE_REFLECTABLE(FiniteSource, out);
    std::vector<float> _data{};
    std::size_t        _pos = 0UZ;

    [[nodiscard]] gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) noexcept {
        const std::size_t n = std::min(outSpan.size(), _data.size() - _pos);
        std::copy_n(_data.begin() + static_cast<std::ptrdiff_t>(_pos), n, outSpan.begin());
        outSpan.publish(n);
        _pos += n;
        return _pos == _data.size() ? gr::work::Status::DONE : gr::work::Status::OK;
    }
};

struct RecordSink : gr::Block<RecordSink> {
    gr::PortIn<Record, gr::Async> in;
    GR_MAKE_REFLECTABLE(RecordSink, in);
    std::vector<Record> _records{};

    [[nodiscard]] gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        for (const auto& record : inSpan) {
            _records.push_back(record);
        }
        std::ignore = inSpan.consume(inSpan.size());
        return gr::work::Status::OK;
    }
};

/// @brief Runs a graph to completion under the simple scheduler, stopping it rather than hanging if it wedges.
template<typename TCollect>
void runGraph(gr::Graph flow, TCollect&& collect) {
    gr::scheduler::Simple<> scheduler;
    boost::ut::expect(scheduler.exchange(std::move(flow)).has_value());
    std::atomic<bool> done{false};
    std::thread       runner([&scheduler, &done] {
        std::ignore = scheduler.runAndWait();
        done        = true;
    });
    const auto        start = std::chrono::steady_clock::now();
    while (!done.load() && std::chrono::steady_clock::now() - start < std::chrono::seconds(30)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!done.load()) {
        scheduler.requestStop();
        boost::ut::expect(false) << "the graph did not finish within thirty seconds";
    }
    runner.join();
    collect();
}

/// @brief Runs @p samples through a `PpmFramer` under the scheduler and hands back what the sink saw.
[[nodiscard]] std::vector<Record> scheduled(std::vector<float> samples, gr::property_map settings) {
    gr::Graph flow;
    auto&     source = flow.emplaceBlock<FiniteSource>();
    source._data     = std::move(samples);
    auto& framer     = flow.emplaceBlock<PpmFramer>(std::move(settings));
    auto& sink       = flow.emplaceBlock<RecordSink>();
    boost::ut::expect(flow.connect<"out", "in">(source, framer).has_value());
    boost::ut::expect(flow.connect<"out", "in">(framer, sink).has_value());

    std::vector<Record> records;
    runGraph(std::move(flow), [&records, &sink] { records = sink._records; });
    return records;
}

struct Rng {
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;

    [[nodiscard]] std::uint64_t next() noexcept {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        return state;
    }
    /// A number in (0, 1].
    [[nodiscard]] double uniform() noexcept { return static_cast<double>((next() >> 11U) + 1ULL) * 0x1p-53; }
};

/// @brief Rayleigh magnitudes of unit mean power, which is what a complex Gaussian channel presents to the framer.
[[nodiscard]] std::vector<float> rayleigh(std::size_t count, std::uint64_t seed) {
    Rng                rng{seed};
    std::vector<float> noise(count);
    for (float& value : noise) {
        value = static_cast<float>(std::sqrt(-std::log(rng.uniform())));
    }
    return noise;
}

} // namespace

const boost::ut::suite<"ppm framer"> ppmFramerTests = [] {
    using namespace boost::ut;

    "the anchor decodes identically at every chunking"_test = [] {
        const std::vector<std::uint8_t> anchor = octetsOf(kAnchor);
        std::vector<float>              stream;
        appendZeros(stream, 3UZ); // the first chunk boundaries then fall inside the preamble and inside a bit
        append(stream, render(std::span<const std::uint8_t>(anchor)));
        appendZeros(stream, 37UZ);
        expect(eq(stream.size(), 3UZ + 240UZ + 37UZ));

        for (const std::size_t chunk : {1UZ, 7UZ, 240UZ, 241UZ, 0UZ}) {
            const Scanned run = scan(std::span<const float>(stream), chunk);
            const auto    at  = std::format("chunk {}", chunk);
            expect(eq(run.frames.size(), 1UZ)) << at;
            expect(eq(run.counters.nominations, 1ULL)) << at;
            expect(eq(run.counters.admitted, 1ULL)) << at;
            expect(eq(run.counters.nominations, run.counters.admitted + run.counters.crcFailed + run.counters.shortFormat)) << at;
            if (run.frames.size() != 1UZ) {
                continue;
            }
            expect(that % (run.frames[0UZ].octets == anchor)) << at;
            expect(eq(run.frames[0UZ].bits, 112UZ)) << at;
            expect(eq(run.frames[0UZ].format, 17U)) << at;
            expect(eq(run.frames[0UZ].remainder, 0ULL)) << at;
            expect(eq(run.frames[0UZ].position, 3UZ)) << at;
            expect(run.frames[0UZ].outcome == PpmOutcome::Admitted) << at;
            expect(eq(run.frames[0UZ].strong, 1.F)) << at;
            expect(eq(run.frames[0UZ].weak, 0.F)) << at;
        }

        // and the block over the same stream reports the same identity
        PpmFramer    block  = make<PpmFramer>(modeSSettings());
        const Framed framed = feed(block, std::span<const float>(stream), 13UZ);
        expect(eq(framed.records.size(), 1UZ));
        expect(eq(framed.nominations, framed.admitted + framed.crcFailed + framed.shortFormat));
        expect(eq(framed.nominations, 1ULL));
        expect(eq(framed.published, 1ULL));
        expect(eq(framed.samples, stream.size()));
        expect(eq(framed.tailDropped, std::uint64_t{37})) << "the positions whose window never completed are dropped and counted";
    };

    "an unconnected output scans and counts, and does not hoard the stream"_test = [] {
        const std::vector<std::uint8_t> anchor = octetsOf(kAnchor);
        std::vector<float>              stream;
        appendZeros(stream, 3UZ);
        append(stream, render(std::span<const std::uint8_t>(anchor)));
        appendZeros(stream, 37UZ);

        PpmFramer    block = make<PpmFramer>(modeSSettings());
        const Framed seen  = feed(block, std::span<const float>(stream), 13UZ, {}, 64UZ, false);
        expect(eq(seen.records.size(), 0UZ)) << "nothing is written where nothing is connected";
        expect(eq(seen.samples, stream.size()));
        expect(eq(seen.nominations, 1ULL)) << "the scan runs whether or not anyone reads it";
        expect(eq(seen.admitted, 1ULL));
        expect(eq(seen.published, 1ULL));
        expect(eq(seen.held, 37UZ)) << "only the positions whose window never completed stay in the working buffer";
        expect(eq(seen.tailDropped, std::uint64_t{37}));
    };

    "the working buffer is bounded when the sink stops taking records"_test = [] {
        // three hundred anchors end to end, with one record of room a call: the sink saturates on the first frame and
        // the rest of the span has nowhere to go, so the buffer would grow with the stream if nothing bounded it
        const std::vector<std::uint8_t> anchor = octetsOf(kAnchor);
        const std::vector<float>        one    = render(std::span<const std::uint8_t>(anchor));
        std::vector<float>              stream;
        for (std::size_t k = 0UZ; k < 300UZ; ++k) {
            append(stream, std::span<const float>(one));
        }
        expect(eq(stream.size(), 72000UZ));

        PpmFramer         block    = make<PpmFramer>(modeSSettings());
        const Framed      seen     = feed(block, std::span<const float>(stream), 0UZ, {}, 1UZ);
        const std::size_t capacity = 240UZ + (1UZ << 16U); // one window and the reserve, both fixed at start()
        expect(eq(seen.records.size(), 1UZ)) << "the sink had room for one";
        expect(eq(seen.samples, stream.size()));
        expect(le(seen.held, capacity)) << "the buffer holds one window and its reserve, and no more of the stream";
        expect(eq(seen.overrunDropped, std::uint64_t{stream.size() - 240UZ - capacity})) << "the samples past the bound are dropped and counted";
        expect(eq(seen.nominations, seen.admitted + seen.crcFailed + seen.shortFormat));
    };

    "the five published frames admit in sequence"_test = [] {
        std::vector<float>       stream;
        std::vector<std::size_t> starts;
        for (const std::string_view hex : kPublished) {
            appendZeros(stream, 80UZ); // 40 us of silence between bursts
            starts.push_back(stream.size());
            const std::vector<std::uint8_t> frame = octetsOf(hex);
            append(stream, render(std::span<const std::uint8_t>(frame)));
        }
        appendZeros(stream, 80UZ);

        const std::vector<Record> records = scheduled(stream, modeSSettings());
        expect(eq(records.size(), 5UZ)) << "every published frame is a bare-CRC extended squitter";
        for (std::size_t k = 0UZ; k < std::min(records.size(), starts.size()); ++k) {
            const Record& record = records[k];
            const auto    at     = std::format("frame {}", k);
            expect(that % (record.signal_values == octetsOf(kPublished[k]))) << at;
            expect(that % (record.signal_names == std::vector<std::string>{"payload"})) << at;
            expect(eq(static_cast<std::size_t>(record.extents.at(0)), 14UZ)) << at;
            expect(eq(record.timestamp, std::int64_t{0})) << at;
            expect(eq(metaString(record, "trigger_name"), std::string("mode_s"))) << at;
            expect(eq(metaString(record, "protocol"), std::string("mode_s"))) << at << ": the framing the record was read under";
            expect(eq(metaSize(record, "mode_s_format"), gr::Size_t{17})) << at;
            expect(eq(metaSize(record, "crc_remainder"), gr::Size_t{0})) << at;
            expect(metaBool(record, "crc_ok")) << at;
            expect(eq(metaU64(record, "sample_start"), starts[k] + 16UZ)) << at << ": the first data bit's first sample";
            expect(eq(metaU64(record, "sequence"), k)) << at;
            expect(eq(metaFloat(record, "sample_rate"), 2.0e6F)) << at;
            expect(eq(metaFloat(record, "preamble_strong"), 1.F)) << at;
            expect(eq(metaFloat(record, "preamble_weak"), 0.F)) << at;
        }
    };

    "both gates are real: the preamble's shape and the parity"_test = [] {
        const std::vector<std::uint8_t> anchor = octetsOf(kAnchor);

        for (const std::size_t pulse : {0UZ, 2UZ, 7UZ, 9UZ}) {
            std::vector<float> stream = render(std::span<const std::uint8_t>(anchor));
            stream[pulse]             = 0.F;
            for (const float threshold : {1.01F, 2.F, 100.F}) {
                const Scanned run = scan(std::span<const float>(stream), 0UZ, 1UZ, threshold);
                expect(eq(run.counters.nominations, 0ULL)) << std::format("pulse slot {} zeroed at threshold {}", pulse, threshold);
            }
        }

        // exchanging one bit's two half periods inverts that bit, and the remainder leaves zero
        std::vector<float> flipped = render(std::span<const std::uint8_t>(anchor));
        std::swap(flipped[16UZ + 2UZ * 40UZ], flipped[16UZ + 2UZ * 40UZ + 1UZ]);
        const Scanned damaged = scan(std::span<const float>(flipped));
        expect(eq(damaged.counters.nominations, 1ULL));
        expect(eq(damaged.counters.crcFailed, 1ULL));
        expect(eq(damaged.counters.admitted, 0ULL));

        PpmFramer    silent    = make<PpmFramer>(modeSSettings());
        const Framed byDefault = feed(silent, std::span<const float>(flipped));
        expect(eq(byDefault.records.size(), 0UZ)) << "a frame the parity refuses is not a record";
        expect(eq(byDefault.crcFailed, 1ULL));

        gr::property_map unchecked  = modeSSettings();
        unchecked["emit_unchecked"] = true;
        PpmFramer    loud           = make<PpmFramer>(unchecked);
        const Framed published      = feed(loud, std::span<const float>(flipped));
        expect(eq(published.records.size(), 1UZ));
        if (published.records.size() == 1UZ) {
            expect(eq(published.records[0UZ].signal_values.size(), 14UZ));
            expect(!metaBool(published.records[0UZ], "crc_ok"));
            expect(metaHas(published.records[0UZ], "crc_ok"));
            expect(neq(metaSize(published.records[0UZ], "crc_remainder"), gr::Size_t{0}));
        }

        std::vector<float> blinded = render(std::span<const std::uint8_t>(anchor));
        blinded[7UZ]               = 0.F;
        PpmFramer    stillSilent   = make<PpmFramer>(unchecked);
        const Framed nothing       = feed(stillSilent, std::span<const float>(blinded));
        expect(eq(nothing.records.size(), 0UZ)) << "emit_unchecked publishes refused parity, not an absent preamble";
        expect(eq(nothing.nominations, 0ULL));
    };

    "an admitted frame contains its own window and a refused one yields a sample"_test = [] {
        const std::vector<std::uint8_t> anchor = octetsOf(kAnchor);

        std::vector<float> pair = render(std::span<const std::uint8_t>(anchor));
        append(pair, render(std::span<const std::uint8_t>(anchor)));
        const Scanned back = scan(std::span<const float>(pair));
        expect(eq(back.counters.nominations, 2ULL));
        expect(eq(back.counters.admitted, 2ULL));
        expect(eq(back.frames.size(), 2UZ));
        if (back.frames.size() == 2UZ) {
            expect(eq(back.frames[0UZ].position, 0UZ));
            expect(eq(back.frames[1UZ].position, 240UZ));
            std::vector<std::uint8_t> both = back.frames[0UZ].octets;
            both.insert(both.end(), back.frames[1UZ].octets.begin(), back.frames[1UZ].octets.end());
            expect(eq(both.size(), 28UZ));
        }

        // a nomination whose parity refuses it must not hide the real frame inside its window: its data region is the
        // anchor's own preamble pattern repeated, which reads as a long frame and reduces to a non-zero remainder
        std::vector<float> decoy;
        for (const int pulse : kPreamble) {
            decoy.push_back(pulse != 0 ? 1.F : 0.F);
        }
        for (std::size_t k = 0UZ; k < 24UZ; ++k) { // 24 half-slot pairs read as bits and set the long-format bit
            decoy.push_back(1.F);
            decoy.push_back(0.F);
        }
        std::vector<float> stream = decoy;
        const std::size_t  at     = stream.size();
        append(stream, render(std::span<const std::uint8_t>(anchor)));
        expect(lt(at, 240UZ)) << "the decoy's window still covers the anchor's first sample";

        const Scanned run = scan(std::span<const float>(stream));
        expect(ge(run.counters.crcFailed, 1ULL)) << "the decoy nominates and its parity refuses it";
        expect(eq(run.counters.admitted, 1ULL));
        bool found = false;
        for (const Seen& frame : run.frames) {
            if (frame.outcome == PpmOutcome::Admitted) {
                found = frame.position == at && frame.octets == anchor;
            }
        }
        expect(found) << "the refused nomination advanced one sample and did not consume the real frame";
    };

    "a short frame is counted, published only on request, and carries no crc_ok"_test = [] {
        // downlink format 11, all-call reply, with a zero interrogator identifier: its parity is the bare CRC
        const std::vector<std::uint8_t> allCall = withParity({0x58U, 0x40U, 0xD6U, 0x11U}, 7UZ);
        const std::vector<float>        stream  = padded(std::span<const std::uint8_t>(allCall));
        expect(eq(render(std::span<const std::uint8_t>(allCall)).size(), 16UZ + 112UZ)) << "a short frame is 56 bits over two slots each";

        PpmFramer    quiet  = make<PpmFramer>(modeSSettings());
        const Framed silent = feed(quiet, std::span<const float>(stream));
        expect(eq(silent.records.size(), 0UZ));
        expect(eq(silent.shortFormat, 1ULL));
        expect(eq(silent.nominations, 1ULL));

        gr::property_map settings = modeSSettings();
        settings["emit_short"]    = true;
        PpmFramer    loud         = make<PpmFramer>(settings);
        const Framed shown        = feed(loud, std::span<const float>(stream));
        expect(eq(shown.records.size(), 1UZ));
        if (shown.records.size() == 1UZ) {
            expect(eq(shown.records[0UZ].signal_values.size(), 7UZ));
            expect(that % (shown.records[0UZ].signal_values == allCall));
            expect(eq(metaSize(shown.records[0UZ], "mode_s_format"), gr::Size_t{11}));
            expect(eq(metaSize(shown.records[0UZ], "crc_remainder"), gr::Size_t{0}));
            expect(!metaHas(shown.records[0UZ], "crc_ok")) << "the question has no answer on a short frame";
        }

        // downlink format 4, whose parity is the CRC XORed with the aircraft address
        for (const std::uint64_t address : {0x4840D6ULL, 0xABCDEFULL}) {
            const std::vector<std::uint8_t> reply  = withParity({0x20U, 0x00U, 0x11U, 0x22U}, 7UZ, address);
            PpmFramer                       framer = make<PpmFramer>(settings);
            const Framed                    seen   = feed(framer, std::span<const float>(padded(std::span<const std::uint8_t>(reply))));
            expect(eq(seen.records.size(), 1UZ)) << std::format("address {:06X}", address);
            if (seen.records.size() == 1UZ) {
                expect(eq(metaSize(seen.records[0UZ], "mode_s_format"), gr::Size_t{4}));
                expect(eq(metaSize(seen.records[0UZ], "crc_remainder"), static_cast<gr::Size_t>(address))) << "the reduction is linear, so the XORed address is the remainder";
            }
        }
    };

    "the format field's top bit selects the length"_test = [] {
        gr::property_map settings = modeSSettings();
        settings["emit_short"]    = true;

        struct Case {
            std::uint8_t lead;
            std::size_t  octets;
            gr::Size_t   format;
        };
        for (const Case& sample : {Case{0x80U, 14UZ, 16U}, Case{0x78U, 7UZ, 15U}, Case{0xC0U, 14UZ, 24U}}) {
            std::vector<std::uint8_t> message(sample.octets, std::uint8_t{0});
            message[0UZ]                          = sample.lead;
            message[1UZ]                          = 0x5AU;
            const std::vector<std::uint8_t> frame = withParity(std::move(message), sample.octets);
            PpmFramer                       block = make<PpmFramer>(settings);
            const Framed                    seen  = feed(block, std::span<const float>(padded(std::span<const std::uint8_t>(frame))));
            const auto                      at    = std::format("format {}", sample.format);
            expect(eq(seen.records.size(), 1UZ)) << at;
            if (seen.records.size() != 1UZ) {
                continue;
            }
            expect(eq(seen.records[0UZ].signal_values.size(), sample.octets)) << at;
            expect(eq(metaSize(seen.records[0UZ], "mode_s_format"), sample.format)) << at;
            expect(eq(metaSize(seen.records[0UZ], "crc_remainder"), gr::Size_t{0})) << at;
        }
    };

    "the rate sets the slot width, and a rate that is not a whole slot is refused"_test = [] {
        const std::vector<std::uint8_t> anchor = octetsOf(kAnchor);
        for (const std::size_t slot : {2UZ, 4UZ}) {
            const float               rate    = 2.0e6F * static_cast<float>(slot);
            const std::vector<Record> records = scheduled(render(std::span<const std::uint8_t>(anchor), slot), modeSSettings(rate));
            const auto                at      = std::format("{} samples a slot", slot);
            expect(eq(records.size(), 1UZ)) << at;
            if (records.size() == 1UZ) {
                expect(that % (records[0UZ].signal_values == anchor)) << at;
                expect(eq(metaU64(records[0UZ], "sample_start"), 16UZ * slot)) << at;
                expect(eq(metaFloat(records[0UZ], "sample_rate"), rate)) << at;
            }
        }

        for (const float rate : {2.4e6F, 3.0e6F, 0.F}) {
            PpmFramer block = PpmFramer(modeSSettings(rate));
            block.settings().init();
            std::ignore = block.settings().applyStagedParameters();
            expect(throws([&block] { block.start(); })) << std::format("{} Hz is not an even multiple of 1 MHz", rate);
        }
        PpmFramer bare = PpmFramer({{"sample_rate", 2.0e6F}});
        bare.settings().init();
        std::ignore = bare.settings().applyStagedParameters();
        expect(throws([&bare] { bare.start(); })) << "profile is required and has no default";

        // a rate tag mid-stream moves the geometry for the samples after it; one that is not a whole slot is refused
        const std::vector<float> first = render(std::span<const std::uint8_t>(anchor), 1UZ);
        std::vector<float>       stream(first);
        appendZeros(stream, 300UZ);
        const std::size_t        change = stream.size();
        const std::vector<float> second = render(std::span<const std::uint8_t>(anchor), 2UZ);
        append(stream, second);
        appendZeros(stream, 600UZ);

        const auto                 rateTag = [](std::size_t at, float rate) { return gr::Tag{at, gr::property_map{{gr::property_map::key_type(gr::tag::SAMPLE_RATE.shortKey()), gr::pmt::Value(rate)}}}; };
        const std::vector<gr::Tag> tags{rateTag(100UZ, 3.0e6F), rateTag(change, 4.0e6F)};
        PpmFramer                  block = make<PpmFramer>(modeSSettings());
        const Framed               seen  = feed(block, std::span<const float>(stream), 64UZ, std::span<const gr::Tag>(tags));
        expect(eq(seen.rateRefused, 1ULL)) << "3 MS/s leaves the previous rate standing";
        expect(eq(seen.records.size(), 2UZ));
        expect(eq(seen.admitted, 2ULL)) << "one anchor under each geometry, and the change does not forget the first";
        expect(eq(seen.published, 2ULL));
        expect(eq(seen.nominations, seen.admitted + seen.crcFailed + seen.shortFormat)) << "the counter identity holds across a rate change";
        if (seen.records.size() == 2UZ) {
            expect(eq(metaFloat(seen.records[0UZ], "sample_rate"), 2.0e6F));
            expect(eq(metaFloat(seen.records[1UZ], "sample_rate"), 4.0e6F));
            expect(eq(metaU64(seen.records[1UZ], "sample_start"), change + 32UZ));
        }
    };

    "noise nominates rarely and publishes nothing"_test = [] {
        const std::vector<float> noise = rayleigh(2000000UZ, 0x5EEDULL);

        const Scanned atDefault = scan(std::span<const float>(noise));
        expect(eq(atDefault.counters.admitted, 0ULL)) << "no false frame survives the parity";
        std::println("[record] 2e6 Rayleigh magnitudes, S = 1: nominations {} at threshold 2.0", atDefault.counters.nominations);
        expect(lt(atDefault.counters.nominations, 20ULL)) << "the section 8 envelope";

        const Scanned atLow = scan(std::span<const float>(noise), 0UZ, 1UZ, 1.5F);
        std::println("[record] the same noise: nominations {} at threshold 1.5", atLow.counters.nominations);
        expect(gt(atLow.counters.nominations, atDefault.counters.nominations)) << "a lower threshold nominates more often";
        expect(eq(atLow.counters.admitted, 0ULL));

        PpmFramer    block = make<PpmFramer>(modeSSettings());
        const Framed seen  = feed(block, std::span<const float>(noise), 65536UZ);
        expect(eq(seen.published, 0ULL));
        expect(eq(seen.nominations, atDefault.counters.nominations)) << "the block counts what the kernel nominated";
    };

    "tags inside a frame become timing events, and the rest are counted dropped"_test = [] {
        const std::vector<std::uint8_t> anchor = octetsOf(kAnchor);
        std::vector<float>              stream;
        appendZeros(stream, 20UZ);
        append(stream, render(std::span<const std::uint8_t>(anchor)));
        appendZeros(stream, 300UZ);

        const std::vector<gr::Tag> tags{
            gr::Tag{5UZ, {{"before", std::string("dropped")}}},       //
            gr::Tag{20UZ + 4UZ, {{"preamble", std::string("kept")}}}, //
            gr::Tag{20UZ + 40UZ, {{"payload", std::string("kept")}}}, //
        };
        PpmFramer    block = make<PpmFramer>(modeSSettings());
        const Framed seen  = feed(block, std::span<const float>(stream), 32UZ, std::span<const gr::Tag>(tags));
        expect(eq(seen.records.size(), 1UZ));
        expect(eq(seen.tagsDropped, 1ULL)) << "the tag in the leading zeros belongs to no frame";
        if (seen.records.size() != 1UZ) {
            return;
        }
        const auto& events = seen.records[0UZ].timing_events.at(0UZ);
        expect(eq(events.size(), 2UZ));
        if (events.size() != 2UZ) {
            return;
        }
        expect(eq(events[0UZ].first, std::ptrdiff_t{-12})) << "a preamble tag precedes the payload, and is not clamped";
        expect(events[0UZ].second.contains(gr::property_map::key_type("preamble"))) << "every key verbatim";
        expect(eq(events[1UZ].first, std::ptrdiff_t{24}));
        expect(events[1UZ].second.contains(gr::property_map::key_type("payload")));
    };

    "the counters are complete after every call, and stop() only reports them"_test = [] {
        // GR4 runs a block's stop() on the thread that asked for the stop: requestStop() reaches
        // SchedulerBase::stop(), which walks the graph calling changeStateTo(REQUESTED_STOP), and that invokes this
        // block's stop() while its own worker may still be inside processBulk. So stop() may read the counters and
        // nothing else, which means the counters have to be complete before it is called.
        const std::vector<std::uint8_t> anchor = octetsOf(kAnchor);
        std::vector<float>              stream;
        appendZeros(stream, 20UZ);
        append(stream, render(std::span<const std::uint8_t>(anchor)));
        appendZeros(stream, 37UZ);
        const std::vector<gr::Tag> tags{gr::Tag{5UZ, {{"before", std::string("dropped")}}}};

        PpmFramer           block = make<PpmFramer>(modeSSettings());
        std::vector<Record> scratch(64UZ);
        for (std::size_t base = 0UZ; base < stream.size();) {
            const std::size_t count = std::min(32UZ, stream.size() - base);
            const auto        first = std::ranges::lower_bound(tags, base, std::ranges::less{}, &gr::Tag::index);
            const auto        last  = std::ranges::lower_bound(tags, base + count, std::ranges::less{}, &gr::Tag::index);

            InputSpan<float>   inSpan(std::span<const float>(stream).subspan(base, count), base, std::span<const gr::Tag>(first, last));
            OutputSpan<Record> outSpan(std::span<Record>(scratch), 0UZ, nullptr, true);
            std::ignore = block.processBulk(inSpan, outSpan);
            base += inSpan.consumed;
        }

        expect(eq(block.nTailDropped, std::uint64_t{37})) << "the undecided remainder is counted by the thread that owns the buffer";
        expect(eq(block.nTagsDropped, std::uint64_t{1})) << "the tag no frame carried is counted by the thread that owns the list";
        const std::size_t held    = block._work.size();
        const std::size_t pending = block._pending.size();
        block.stop();
        expect(eq(block._work.size(), held)) << "stop() does not touch the working buffer";
        expect(eq(block._pending.size(), pending)) << "stop() does not touch the pending list";
        expect(eq(block.nTailDropped, std::uint64_t{37})) << "and it counts nothing twice";
        expect(eq(block.nTagsDropped, std::uint64_t{1}));
    };

    "a stop arriving while the working thread scans does not read past the working buffer"_test = [] {
        // The same stop, taken at the moment it is actually taken. At 6 MS/s a slot is three samples, so the kernel
        // sums every slot before it decides anything and touches every sample of the span it was handed: a span one
        // element too long faults on the spot under AddressSanitizer, where at one sample a slot it may not.
        const std::vector<std::uint8_t> anchor = octetsOf(kAnchor);
        const std::vector<float>        one    = render(std::span<const std::uint8_t>(anchor), 3UZ);
        std::vector<float>              stream;
        while (stream.size() < 300000UZ) {
            append(stream, std::span<const float>(one));
            appendZeros(stream, 240UZ);
        }

        for (const int wait : {2, 6, 14}) {
            PpmFramer         block = make<PpmFramer>(modeSSettings(6.0e6F));
            std::atomic<bool> scanning{false};
            std::atomic<bool> halt{false};

            std::thread worker([&block, &stream, &scanning, &halt] {
                std::vector<Record> scratch(4096UZ);
                for (std::size_t base = 0UZ; base < stream.size() && !halt.load(std::memory_order_acquire);) {
                    const std::size_t  count = std::min(1UZ << 16U, stream.size() - base);
                    InputSpan<float>   inSpan(std::span<const float>(stream).subspan(base, count), base, {});
                    OutputSpan<Record> outSpan(std::span<Record>(scratch), 0UZ, nullptr, true);
                    scanning.store(true, std::memory_order_release);
                    std::ignore = block.processBulk(inSpan, outSpan);
                    base += inSpan.consumed;
                }
            });

            while (!scanning.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(wait));
            block.stop();
            halt.store(true, std::memory_order_release);
            worker.join();
            expect(ge(block.nSamples, std::uint64_t{1UZ << 16U})) << std::format("the scan ran before the stop at {} ms", wait);
        }
    };
};

int main() { /* not needed for UT */ }
