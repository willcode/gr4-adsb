#ifndef GR4ADSB_ADSB_PPM_FRAMER_HPP
#define GR4ADSB_ADSB_PPM_FRAMER_HPP

#include <algorithm>
#include <cmath>
#include <complex>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/DataSet.hpp>
#include <gnuradio-4.0/Port.hpp>
#include <gnuradio-4.0/Tag.hpp>
#include <gnuradio-4.0/annotated.hpp>

#include <gnuradio-4.0/algorithm/digital/Ppm.hpp>

#include <adsb/BlockLabels.hpp>

namespace adsb {

// The blocks sit in namespace `adsb` rather than `gr`, and their registry keys follow from it. The framework
// names below are individual declarations rather than a using-directive: `adsb` re-exports these alone.
using gr::Annotated;
using gr::Async;
using gr::Block;
using gr::DataSet;
using gr::Doc;
using gr::InputSpanLike;
using gr::NoTagPropagation;
using gr::OutputSpanLike;
using gr::PortIn;
using gr::PortOut;
using gr::property_map;
using gr::Unit;
using gr::Visible;
namespace pmt  = gr::pmt;
namespace work = gr::work;

GR_REGISTER_BLOCK(adsb::PpmFramer, [T], [ float, std::complex<float> ])

/**
 * @brief Runs `gr::digital::PpmScanner` over the input stream and publishes one record per frame.
 *
 * The block derives the samples per slot from the rate, holds the samples the scanner has not yet decided, turns
 * each reported frame into a record, and carries onto that record the tags that fell inside the frame. The kernel
 * decides what a frame is.
 *
 * @tparam T `float` for a magnitude stream, `std::complex<float>` for the samples themselves. The sub-sample phase
 *         search runs on the complex instantiation alone, which interpolates the samples it reads.
 */
template<typename T>
requires(std::same_as<T, float> || std::same_as<T, std::complex<float>>)
struct PpmFramer : Block<PpmFramer<T>, NoTagPropagation> {
    /// @brief True on the complex instantiation, which searches sub-sample phase.
    static constexpr bool kAligns = std::same_as<T, std::complex<float>>;

    using Description = Doc<R""(
@brief Reads pulse-position frames from a magnitude or complex stream and publishes one `DataSet<uint8_t>` per frame.

`profile` selects the framing. `mode_s` is ICAO Annex 10 Volume IV's 1090 MHz reply: preamble pulses at 0.0, 1.0,
3.5 and 4.5 microseconds, data from 8.0, and a frame of 56 or 112 bits chosen by the downlink format's top bit.

At `float` the input is magnitude, `|x|` rather than `|x|^2`, and `threshold` is a ratio of magnitudes;
`gr::blocks::math::Abs<std::complex<float>>` produces such a stream on its `out` port.

At `std::complex<float>` the block decides every position at the sub-sample phase its preamble sits on. A position
first passes a coarse test at `threshold * coarse_ratio`, and the block re-tests it at `phase_steps` offsets
spanning one sample period. The offset whose least pulse slot stands highest above its greatest gap slot decides
both the nomination and the bits, and travels on the record as `sample_phase`: a frame's timing is
`sample_start + sample_phase`. Only this instantiation reads `phase_steps` and `coarse_ratio`.

`sample_rate` must be an even integer multiple of 1 MHz, so that a half-microsecond slot spans a whole number of
samples. A `sample_rate` tag overrides the setting for the samples after it; a tagged rate of any other value is a
counted refusal that leaves the previous rate standing.

The parity is checked here because the remainder decides containment: an admitted frame consumes its whole window
and the search nominates no position inside it, while every other nomination advances the search by one sample. The
remainder travels on every record: zero on an admitted frame, and the aircraft address on the formats that transmit
the parity XORed with one. `emit_short` and `emit_unchecked` ask for the two kinds the block otherwise holds back:
short frames, which carry no self-checking parity, and CRC-failed long frames.

The samples not yet decided sit in a buffer whose bound `start()` fixes from the rate and the framing. Samples past
that bound are dropped and counted, oldest first.
)"">;
#ifdef GR4ADSB_BLOCK_LABELS
    static constexpr auto attributes = gr::block::describe(1U, kFamilyLabel, gr::block::labels::role::processor);
#endif

    PortIn<T>                             in;
    PortOut<DataSet<std::uint8_t>, Async> out;

    Annotated<float, "sample_rate", Unit<"Hz">, Doc<"required input sample rate, an even integer multiple of 1 MHz">, Visible>  sample_rate = 0.F;
    Annotated<std::string, "profile", Doc<"required pulse-position framing; 'mode_s'">, Visible>                                profile{};
    Annotated<float, "threshold", Doc<"nomination ratio, least pulse slot to greatest gap slot; above one">, Visible>           threshold      = 2.F;
    Annotated<gr::Size_t, "phase_steps", Doc<"sub-sample offsets per candidate; complex instantiation only">, Visible>          phase_steps    = 8U;
    Annotated<float, "coarse_ratio", Doc<"threshold fraction for the integer-position test, in (0, 1]; complex only">, Visible> coarse_ratio   = 0.5F;
    Annotated<bool, "emit_short", Doc<"publish short-format nominations">>                                                      emit_short     = false;
    Annotated<bool, "emit_unchecked", Doc<"publish long frames whose remainder is nonzero">>                                    emit_unchecked = false;
    Annotated<std::string, "frame_label", Doc<"frame label written under the record's trigger_name key">>                       frame_label    = std::string("mode_s");

    GR_MAKE_REFLECTABLE(PpmFramer, in, out, sample_rate, profile, threshold, phase_steps, coarse_ratio, emit_short, emit_unchecked, frame_label);

    // Totals and counted drops, read by the owning thread and reported at stop().
    std::uint64_t nSamples        = 0ULL; ///< input samples consumed
    std::uint64_t nCandidates     = 0ULL; ///< positions whose preamble shape passed the coarse test
    std::uint64_t nNominations    = 0ULL; ///< positions whose preamble shape passed the threshold
    std::uint64_t nAdmitted       = 0ULL; ///< long frames whose whole-frame remainder was zero
    std::uint64_t nCrcFailed      = 0ULL; ///< long frames whose remainder was nonzero
    std::uint64_t nShortFormat    = 0ULL; ///< short-format frames, which carry no self-checking parity
    std::uint64_t nPublished      = 0ULL; ///< records produced for `out`, counted even while the port is unconnected
    std::uint64_t nTagsDropped    = 0ULL; ///< input tags that fell outside every published frame, including the ones still waiting
    std::uint64_t nRateRefused    = 0ULL; ///< sample_rate tags whose value is not an even multiple of 1 MHz
    std::uint64_t nTailDropped    = 0ULL; ///< samples whose window never completed, including those still held
    std::uint64_t nOverrunDropped = 0ULL; ///< samples dropped from the working buffer at its bound

    /// @brief One input tag waiting for a frame that might contain it, at its absolute input index.
    struct Pending {
        std::size_t  at   = 0UZ;
        bool         used = false; ///< whether some published frame's span already carried it
        property_map map{};
    };

    /// @brief One arriving tag, as a position in the current span and a borrowed map.
    struct Arriving {
        std::size_t         at  = 0UZ;
        const property_map* map = nullptr;
    };

    /// @brief Bound on the tags held waiting for a frame; several tags may share one index.
    static constexpr std::size_t kPendingCap = 4096UZ;
    /// @brief Room the working buffer keeps beyond one window; a whole input span appends without growing it.
    static constexpr std::size_t kWorkReserve = 1UZ << 16U;

    gr::digital::PpmConfig  _framing{};
    gr::digital::PpmScanner _scanner{};
    /// @brief The kernel's counts from the earlier rates, banked because `prepare()` zeroes its own counters.
    gr::digital::PpmCounters _retired{};
    std::vector<T>           _work{};          ///< input not yet decided, followed by whatever arrived last
    std::vector<float>       _mags{};          ///< `_work`'s magnitudes, index for index; empty on the magnitude instantiation
    std::size_t              _workStart = 0UZ; ///< absolute input index of `_work`'s first sample
    std::size_t              _slot      = 0UZ; ///< samples per half-bit-period slot, at the rate in force
    float                    _rate      = 0.F; ///< the rate in force, which a tag may replace
    std::vector<Pending>     _pending{};
    std::vector<Arriving>    _arriving{};
    std::uint64_t            _sequence = 0ULL;
    /// @brief The two drop totals already retired; the counters add the samples and tags still held.
    std::uint64_t _tailBanked = 0ULL;
    std::uint64_t _tagsBanked = 0ULL;

    void settingsChanged(const property_map& /*oldSettings*/, const property_map& /*newSettings*/) { rebuild(); }

    void start() {
        if (profile.value.empty()) {
            throw gr::exception("profile is required and has no default; 'mode_s' is the one defined");
        }
        if (slotsFor(sample_rate.value) == 0UZ) {
            throw gr::exception(std::format("sample_rate must be an even integer multiple of 1 MHz, got {} Hz", sample_rate.value));
        }
        rebuild();
        _work.reserve(_scanner.phasedWindowSamples() + kWorkReserve);
        if constexpr (kAligns) {
            _mags.reserve(_scanner.phasedWindowSamples() + kWorkReserve);
        }
        _pending.reserve(kPendingCap);
    }

    void reset() { rebuild(); }

    /// @brief Rebuilds the scanner from the settings, refusing any the framing cannot take.
    void rebuild() {
        if (!profile.value.empty() && profile.value != std::string_view("mode_s")) {
            throw gr::exception(std::format("profile must be 'mode_s', got '{}'", profile.value));
        }
        if (threshold <= 1.F) {
            throw gr::exception(std::format("threshold is the ratio of the least pulse slot to the greatest gap and must exceed one, got {}", threshold));
        }
        if (phase_steps == 0U) {
            throw gr::exception(std::format("phase_steps is the number of sub-sample offsets a candidate is tested at and must be at least one, got {}", phase_steps.value));
        }
        if (!(coarse_ratio > 0.F) || coarse_ratio > 1.F) {
            throw gr::exception(std::format("coarse_ratio scales the threshold the coarse test admits a candidate at and must lie in (0, 1], got {}", coarse_ratio));
        }

        _work.clear();
        _mags.clear();
        _pending.clear();
        _workStart = 0UZ;
        _sequence  = 0ULL;
        nSamples = nCandidates = nNominations = nAdmitted = nCrcFailed = nShortFormat = nPublished = 0ULL;
        nTagsDropped = nRateRefused = nTailDropped = nOverrunDropped = 0ULL;
        _tailBanked = _tagsBanked = 0ULL;
        _retired                  = gr::digital::PpmCounters{};

        if (profile.value.empty()) {
            return;
        }
        _framing               = gr::digital::modeS();
        const std::size_t slot = slotsFor(sample_rate.value);
        arm();
        if (slot != 0UZ) {
            _slot = slot;
            _rate = sample_rate.value;
            _scanner.prepare(_framing, slot);
        }
    }

    /// @brief Puts the nomination settings onto the kernel, before `prepare()` sizes the interpolator from them.
    void arm() {
        // A magnitude stream carries nothing to interpolate. That instantiation therefore searches one offset,
        // the integer positions themselves.
        _scanner.threshold   = threshold;
        _scanner.phaseSteps  = kAligns ? static_cast<std::size_t>(phase_steps.value) : 1UZ;
        _scanner.coarseRatio = kAligns ? coarse_ratio.value : 1.F;
    }

    [[nodiscard]] work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        if (!_scanner.configured()) { // unconfigured: inert rather than scanning something arbitrary
            std::ignore = inSpan.consume(0UZ);
            outSpan.publish(0UZ);
            return work::Status::ERROR;
        }
        const bool        outConnected = outSpan.isConnected;
        const std::size_t room         = outConnected ? outSpan.size() : std::numeric_limits<std::size_t>::max();
        if (room == 0UZ) {
            std::ignore = inSpan.consume(0UZ);
            outSpan.publish(0UZ);
            return work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }

        _arriving.clear();
        for (const auto& [relIndex, tagMap] : inSpan.tags()) {
            // a block holding undecided samples across calls is shown an earlier call's tag again at a negative index
            if (relIndex >= 0 && static_cast<std::size_t>(relIndex) < inSpan.size()) {
                _arriving.push_back({static_cast<std::size_t>(relIndex), &tagMap.get()});
            }
        }
        std::ranges::stable_sort(_arriving, std::ranges::less{}, &Arriving::at);
        for (const Arriving& tag : _arriving) {
            if (_pending.size() >= kPendingCap) {
                ++_tagsBanked;
                continue;
            }
            _pending.push_back({inSpan.streamIndex + tag.at, false, *tag.map});
        }

        std::size_t onOut = 0UZ;
        const auto  sink  = [&](const gr::digital::PpmFrame& frame) {
            const bool publish = frame.admitted()                                                           //
                                 || (frame.outcome == gr::digital::PpmOutcome::CrcFailed && emit_unchecked) //
                                 || (frame.outcome == gr::digital::PpmOutcome::ShortFormat && emit_short);
            if (!publish || onOut >= room) {
                return;
            }
            DataSet<std::uint8_t> record = buildRecord(frame);
            if (outConnected) {
                outSpan[onOut] = std::move(record);
            }
            ++onOut;
            ++nPublished;
        };

        // one scan pass over everything undecided plus `arriving`, in slices small enough that the sink cannot overrun
        const auto scan = [&](std::span<const T> arriving) -> std::expected<void, gr::Error> {
            _work.insert(_work.end(), arriving.begin(), arriving.end());
            if constexpr (kAligns) {
                // the kernel decides on magnitude and interpolates on the samples. Both grow together here, the
                // only place either grows; the reserve taken at start() keeps that off the heap
                const std::size_t known = _mags.size();
                _mags.resize(_work.size());
                for (std::size_t j = 0UZ; j < arriving.size(); ++j) {
                    _mags[known + j] = std::abs(arriving[j]);
                }
            }
            // the span one position needs, guard included
            const std::size_t window = _scanner.phasedWindowSamples();
            std::size_t       base   = 0UZ;
            while (onOut < room && _work.size() - base >= window) {
                // a slice of `window - 1 + k` samples holds k decidable positions, hence at most k frames; with
                // room for as many frames as the buffer has samples, nothing binds before its end
                const std::size_t available = _work.size() - base;
                const std::size_t headroom  = room - onOut;
                const std::size_t slice     = headroom >= available ? available : std::min(available, window - 1UZ + headroom);
                _scanner.seek(_workStart + base);
                std::expected<std::size_t, gr::Error> done{0UZ};
                if constexpr (kAligns) {
                    done = _scanner.consume(std::span<const float>(_mags).subspan(base, slice), std::span<const T>(_work).subspan(base, slice), sink);
                } else {
                    done = _scanner.consume(std::span<const float>(_work).subspan(base, slice), sink);
                }
                if (!done) {
                    return std::unexpected(std::move(done.error()));
                }
                if (*done == 0UZ) {
                    break;
                }
                base += *done;
            }
            // the buffer holds one window and the reserve, and drops from the oldest end: the newest samples are
            // the ones still worth deciding
            const std::size_t capacity = window + kWorkReserve;
            if (_work.size() - base > capacity) {
                const std::size_t excess = _work.size() - base - capacity;
                nOverrunDropped += excess;
                base += excess;
            }
            _work.erase(_work.begin(), _work.begin() + static_cast<std::ptrdiff_t>(base));
            if constexpr (kAligns) {
                _mags.erase(_mags.begin(), _mags.begin() + static_cast<std::ptrdiff_t>(base));
            }
            _workStart += base;
            prunePending(_workStart);
            return {};
        };
        // a framing the kernel refuses on its re-check stops the block; the records already built are published
        const auto fail = [&](gr::Error error) {
            this->emitErrorMessage("processBulk", std::move(error));
            std::ignore = inSpan.consume(inSpan.size());
            outSpan.publish(outConnected ? onOut : 0UZ);
            return work::Status::ERROR;
        };

        const std::span<const T> items(inSpan.begin(), inSpan.size());
        std::size_t              cut = 0UZ;
        for (const Arriving& tag : _arriving) {
            const auto entry = tag.map->find(property_map::key_type(gr::tag::SAMPLE_RATE.shortKey()));
            if (entry == tag.map->end()) {
                continue;
            }
            const float* value = entry->second.template get_if<float>();
            if (value == nullptr) {
                continue;
            }
            const std::size_t slot = slotsFor(*value);
            if (slot == 0UZ) {
                ++nRateRefused; // the previous rate stands, because a fractional slot has no bit rule
                continue;
            }
            if (*value == _rate) {
                continue;
            }
            if (auto scanned = scan(items.subspan(cut, tag.at - cut)); !scanned) {
                return fail(std::move(scanned.error()));
            }
            cut = tag.at;
            // the geometry changes at this sample; nothing spanning it can be decided under either rate
            _tailBanked += _work.size();
            _work.clear();
            _mags.clear();
            _workStart = inSpan.streamIndex + tag.at;
            _slot      = slot;
            _rate      = *value;
            // preparing the kernel zeroes its counters; its counts under the old rate are banked first
            _retired.candidates += _scanner.counters.candidates;
            _retired.nominations += _scanner.counters.nominations;
            _retired.admitted += _scanner.counters.admitted;
            _retired.crcFailed += _scanner.counters.crcFailed;
            _retired.shortFormat += _scanner.counters.shortFormat;
            arm();
            _scanner.prepare(_framing, slot);
            prunePending(_workStart);
        }
        if (auto scanned = scan(items.subspan(cut)); !scanned) {
            return fail(std::move(scanned.error()));
        }

        nSamples += inSpan.size();
        nCandidates  = _retired.candidates + _scanner.counters.candidates;
        nNominations = _retired.nominations + _scanner.counters.nominations;
        nAdmitted    = _retired.admitted + _scanner.counters.admitted;
        nCrcFailed   = _retired.crcFailed + _scanner.counters.crcFailed;
        nShortFormat = _retired.shortFormat + _scanner.counters.shortFormat;
        // a stream ending here drops what the buffer and the list still hold
        nTailDropped = _tailBanked + std::uint64_t{_work.size()};
        nTagsDropped = _tagsBanked + static_cast<std::uint64_t>(std::ranges::count(_pending, false, &Pending::used));

        const std::size_t consumed = inSpan.size();
        std::ignore                = inSpan.consume(consumed);
        outSpan.publish(outConnected ? onOut : 0UZ);
        return consumed == 0UZ && onOut == 0UZ ? work::Status::INSUFFICIENT_INPUT_ITEMS : work::Status::OK;
    }

    /// @brief The record a published frame becomes: its octets, its provenance, and the tags that fell inside it.
    [[nodiscard]] DataSet<std::uint8_t> buildRecord(const gr::digital::PpmFrame& frame) {
        const std::size_t preamble = _framing.preambleSlots * _slot;
        const std::size_t start    = frame.position + preamble;
        const std::size_t span     = preamble + 2UZ * frame.bits * _slot;

        DataSet<std::uint8_t> record;
        record.signal_values.assign(frame.octets.begin(), frame.octets.end());
        record.extents.push_back(static_cast<std::int32_t>(frame.octets.size()));
        record.signal_names.emplace_back("payload");
        record.signal_quantities.emplace_back("");
        record.signal_units.emplace_back("");
        record.meta_information.emplace_back();
        record.timing_events.emplace_back();
        record.timestamp = 0;

        property_map& meta = record.meta_information[0UZ];
        meta.insert_or_assign(property_map::key_type(gr::tag::TRIGGER_NAME.shortKey()), pmt::Value(std::string_view(frame_label.value)));
        // the framing this record was read under; a decoder downstream names its own sub-kind over this one
        meta.insert_or_assign(property_map::key_type("protocol"), pmt::Value(std::string_view(profile.value)));
        meta.insert_or_assign(property_map::key_type("sample_start"), pmt::Value(static_cast<std::uint64_t>(start)));
        meta.insert_or_assign(property_map::key_type("sequence"), pmt::Value(_sequence));
        meta.insert_or_assign(property_map::key_type(gr::tag::SAMPLE_RATE.shortKey()), pmt::Value(_rate));
        meta.insert_or_assign(property_map::key_type("mode_s_format"), pmt::Value(static_cast<gr::Size_t>(frame.format)));
        meta.insert_or_assign(property_map::key_type("crc_remainder"), pmt::Value(static_cast<gr::Size_t>(frame.remainder)));
        if (frame.outcome != gr::digital::PpmOutcome::ShortFormat) {
            // a short frame carries no self-checking parity, and a false there would read as a failure
            meta.insert_or_assign(property_map::key_type("crc_ok"), pmt::Value(frame.admitted()));
        }
        meta.insert_or_assign(property_map::key_type("preamble_strong"), pmt::Value(frame.strong));
        meta.insert_or_assign(property_map::key_type("preamble_weak"), pmt::Value(frame.weak));
        // an admitted frame takes its window from the first position that passed, which can be the sample before
        // the one its preamble sits nearest. This offset in [-0.5, 0.5) compensates, and is zero where nothing was
        // searched; `sample_start + sample_phase` is therefore the timing on either instantiation
        meta.insert_or_assign(property_map::key_type("sample_phase"), pmt::Value(frame.phase));

        auto& events = record.timing_events[0UZ];
        for (Pending& event : _pending) {
            if (event.at < frame.position || event.at >= frame.position + span) {
                continue;
            }
            event.used = true;
            // a tag on a preamble sample precedes the payload; its index is negative and is left unclamped
            events.emplace_back(static_cast<std::ptrdiff_t>(event.at) - static_cast<std::ptrdiff_t>(start), event.map);
        }

        ++_sequence;
        return record;
    }

    /// @brief Retires tags no later frame can carry, because every frame still to come starts at @p before or after.
    void prunePending(std::size_t before) {
        std::size_t retired = 0UZ;
        while (retired < _pending.size() && _pending[retired].at < before) {
            if (!_pending[retired].used) {
                ++_tagsBanked;
            }
            ++retired;
        }
        _pending.erase(_pending.begin(), _pending.begin() + static_cast<std::ptrdiff_t>(retired));
    }

    /// @brief The samples a half-microsecond slot spans at @p rate, or zero when the rate is not an even multiple of 1 MHz.
    [[nodiscard]] static std::size_t slotsFor(float rate) noexcept {
        if (!(rate > 0.F)) {
            return 0UZ;
        }
        const auto slots = static_cast<std::size_t>(std::llround(static_cast<double>(rate) / 2.0e6));
        if (slots == 0UZ) {
            return 0UZ;
        }
        return static_cast<float>(2.0e6 * static_cast<double>(slots)) == rate ? slots : 0UZ;
    }
};

} // namespace adsb

#endif // GR4ADSB_ADSB_PPM_FRAMER_HPP
