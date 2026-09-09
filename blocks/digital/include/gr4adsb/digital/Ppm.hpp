#ifndef GR4ADSB_PPM_HPP
#define GR4ADSB_PPM_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include <gr4adsb/digital/Crc.hpp>

/**
 * @brief Pulse-position framing: a burst read out of magnitude samples with no clock and no carrier to acquire.
 *
 * Pulse-position modulation carries one pulse per bit period, in the first half of the period for a one and in the
 * second for a zero, so a bit is decided by comparing the two halves of one period with each other. Nothing is
 * carried in phase, no amplitude has to be tracked, and the comparison is scale free — which is why a burst that
 * arrives out of noise, with nothing before it and nothing after it, needs no synchronization at all. The scanner
 * here is therefore a window that slides one sample at a time and decides each position on its own.
 *
 * A framing is a `PpmConfig`: which of the preamble's half-period slots carry a pulse, how many slots the preamble
 * spans, how many leading bits select the frame length, the two lengths, and the parity field's CRC six-tuple.
 * `modeS()` is the ICAO Annex 10 Volume IV reply format as a value of it. The config is an interoperability
 * constant rather than a default, so a scanner without one is inert.
 *
 * Every position is decided exactly once and its decision reads a fixed window after it, so the result does not
 * depend on how the input was chunked. `consume()` decides every position whose whole window lies inside the span
 * it was given and returns how many samples it is done with; the caller presents the remainder again, prefixed to
 * whatever arrived next. Nothing here allocates after `prepare()` except the slot-sum scratch, which grows once to
 * the largest span it has been shown.
 *
 * The parity is checked here rather than downstream because the remainder decides containment: an admitted frame
 * hides the positions inside it, and only a frame the parity vouches for may do that. The remainder travels on
 * every reported frame, so nothing is thrown away by checking early — for the Mode S formats that transmit the
 * parity XOR'd with an address, the remainder *is* that address.
 */
namespace gr4adsb {

/**
 * @brief One pulse-position framing: the preamble's shape, the length rule, and the parity field's CRC.
 *
 * Slots are half bit periods and every position is stated in them, so one integer — the samples per slot — converts
 * the whole geometry to a sample rate. The pulse width is one slot by construction: a slot *is* a half period.
 */
struct PpmConfig {
    std::vector<std::size_t> pulseSlots{};        ///< the preamble slots carrying a pulse, in slots from the frame's first sample
    std::size_t              preambleSlots = 0UZ; ///< slots the preamble spans; `0` is the unset state
    std::size_t              formatBits    = 0UZ; ///< leading data bits that select the length, at most 32 because the field is read into a 32-bit word
    std::size_t              longBits      = 0UZ; ///< frame length in bits when the length bit is set
    std::size_t              shortBits     = 0UZ; ///< frame length in bits otherwise

    std::uint8_t  crcWidth           = 0U;    ///< the parity field's width in bits, a multiple of eight
    std::uint64_t crcPoly            = 0ULL;  ///< generator polynomial, most significant bit first, the `x^w` term implicit
    std::uint64_t crcInit            = 0ULL;  ///< seed, in the unreflected domain
    std::uint64_t crcFinalXor        = 0ULL;  ///< XORed into the result last
    bool          crcInputReflected  = false; ///< each message byte enters least significant bit first
    bool          crcResultReflected = false; ///< the register is bit-reversed before the final XOR
};

/**
 * @brief Validates @p cfg, refusing every geometry the scanner cannot read.
 *
 * The refusals are structural rather than protocol specific: a preamble needs a pulse and a gap for the shape test
 * to compare, the format field has to fit both in the shorter frame and in the 32-bit word that reads it, and both
 * frame lengths have to be whole octets because the output is octets. The parity field is a whole number of octets for
 * the same reason — the remainder is the byte-wise CRC of the message octets XOR'd with the parity octets, which
 * is a division of the whole frame only when the split falls on an octet boundary.
 *
 * @throws std::invalid_argument naming the member and its value.
 */
inline void configure(const PpmConfig& cfg) {
    if (cfg.pulseSlots.empty()) {
        throw std::invalid_argument("gr4adsb::configure: a pulse-position preamble needs at least one pulse slot, and none was given");
    }
    if (cfg.preambleSlots == 0UZ) {
        throw std::invalid_argument("gr4adsb::configure: preambleSlots is the preamble's length in slots and must be at least one, got 0");
    }
    for (const std::size_t slot : cfg.pulseSlots) {
        if (slot >= cfg.preambleSlots) {
            throw std::invalid_argument(std::format("gr4adsb::configure: pulse slot {} lies outside the {} slot preamble", slot, cfg.preambleSlots));
        }
    }
    std::vector<std::size_t> sorted(cfg.pulseSlots);
    std::ranges::sort(sorted);
    if (std::ranges::adjacent_find(sorted) != sorted.end()) {
        throw std::invalid_argument("gr4adsb::configure: a pulse slot is named twice, and the shape test reads each slot once");
    }
    if (sorted.size() == cfg.preambleSlots) {
        throw std::invalid_argument("gr4adsb::configure: every preamble slot carries a pulse, so there is no gap to measure the pulses against");
    }
    if (cfg.formatBits < 1UZ) {
        throw std::invalid_argument("gr4adsb::configure: formatBits selects the length and must be at least one, got 0");
    }
    if (cfg.formatBits > 32UZ) {
        throw std::invalid_argument(std::format("gr4adsb::configure: the format field is read into a 32-bit word and its top bit is the length bit, so formatBits must be at most 32, got {}", cfg.formatBits));
    }
    if (cfg.shortBits > cfg.longBits) {
        throw std::invalid_argument(std::format("gr4adsb::configure: shortBits {} exceeds longBits {}", cfg.shortBits, cfg.longBits));
    }
    if (cfg.shortBits == 0UZ || cfg.shortBits % 8UZ != 0UZ || cfg.longBits % 8UZ != 0UZ) {
        throw std::invalid_argument(std::format("gr4adsb::configure: both frame lengths are emitted as octets and must be non-zero multiples of eight bits, got {} and {}", cfg.shortBits, cfg.longBits));
    }
    if (cfg.formatBits > cfg.shortBits) {
        throw std::invalid_argument(std::format("gr4adsb::configure: the {} bit format field does not fit in the {} bit short frame", cfg.formatBits, cfg.shortBits));
    }
    if (cfg.crcWidth < 3U || cfg.crcWidth > 64U) {
        throw std::invalid_argument(std::format("gr4adsb::configure: crcWidth must be in [3, 64], got {}", static_cast<unsigned>(cfg.crcWidth)));
    }
    if (cfg.crcWidth % 8U != 0U) {
        throw std::invalid_argument(std::format("gr4adsb::configure: the parity field is a trailing run of octets, so crcWidth must be a multiple of eight, got {}", static_cast<unsigned>(cfg.crcWidth)));
    }
    if (static_cast<std::size_t>(cfg.crcWidth) >= cfg.shortBits) {
        throw std::invalid_argument(std::format("gr4adsb::configure: a {} bit parity field leaves no message in a {} bit frame", static_cast<unsigned>(cfg.crcWidth), cfg.shortBits));
    }
}

/**
 * @brief ICAO Annex 10 Volume IV chapter 3: the 1090 MHz Mode S reply, as a framing.
 *
 * The preamble is four half-microsecond pulses with leading edges at 0.0, 1.0, 3.5 and 4.5 microseconds, which at
 * two slots per microsecond are slots 0, 2, 7 and 9 of sixteen; data begins at 8.0 microseconds. The first five
 * bits are the downlink format and formats 16 and above are 112 bits long, the rest 56 — equivalently the format
 * field's most significant bit is the length bit. The last 24 bits are a parity field over the preceding 88 or 32,
 * generated by `x^24 + x^23 + ... + x^12 + x^10 + x^3 + 1`.
 */
[[nodiscard]] inline PpmConfig modeS() {
    PpmConfig cfg;
    cfg.pulseSlots    = {0UZ, 2UZ, 7UZ, 9UZ};
    cfg.preambleSlots = 16UZ;
    cfg.formatBits    = 5UZ;
    cfg.longBits      = 112UZ;
    cfg.shortBits     = 56UZ;
    cfg.crcWidth      = 24U;
    cfg.crcPoly       = 0xFFF409ULL;
    configure(cfg);
    return cfg;
}

/// @brief What the parity said about a nomination, and therefore whether it may hide the positions inside it.
enum class PpmOutcome : std::uint8_t {
    Admitted,    ///< a long frame whose whole-frame remainder is zero
    CrcFailed,   ///< a long frame whose remainder is not zero
    ShortFormat, ///< a short frame, which carries no self-checking parity and can be neither admitted nor refused
};

/// @brief One nominated position, decided. `octets` points into the scanner and is valid until the next call.
struct PpmFrame {
    std::span<const std::uint8_t> octets{};         ///< the frame, first bit received in the first octet's most significant bit
    std::size_t                   bits      = 0UZ;  ///< the frame's length in bits, one of the config's two
    std::uint32_t                 format    = 0U;   ///< the leading `formatBits` bits, as a number
    std::uint64_t                 remainder = 0ULL; ///< the whole frame's remainder: zero when admitted, the address when the parity was XOR'd with one
    float                         strong    = 0.F;  ///< the least pulse-slot mean
    float                         weak      = 0.F;  ///< the greatest gap-slot mean
    std::size_t                   position  = 0UZ;  ///< absolute sample index of the nominated position, the preamble's first sample
    PpmOutcome                    outcome   = PpmOutcome::Admitted;

    [[nodiscard]] bool admitted() const noexcept { return outcome == PpmOutcome::Admitted; }
};

/// @brief Every nomination the scanner made, split by what the parity said. The three kinds sum to `nominations`.
struct PpmCounters {
    std::uint64_t nominations = 0ULL;
    std::uint64_t admitted    = 0ULL;
    std::uint64_t crcFailed   = 0ULL;
    std::uint64_t shortFormat = 0ULL;
};

/**
 * @brief The pulse-position scanner: a shape test at every position, and a frame read out of the ones that pass.
 *
 * A slot's strength is the mean magnitude over its `samplesPerSlot()` samples, and the shape test compares the
 * *least* of the pulse slots with the *greatest* of the gaps. Taking the extremes on both sides makes it a test of
 * shape and not of energy: one loud sample cannot buy a nomination for a position whose other pulses are absent.
 * Because every slot averages the same number of samples, the test and the bit decisions are made on the sums
 * directly and the division by the slot width is done only for what is reported.
 *
 * A nomination reads the format field, takes the frame length from it, packs the bits into octets and computes the
 * whole frame's remainder. A long frame with a zero remainder is admitted and consumes its whole window, so no
 * position inside it is nominated; every other nomination advances the search by one sample. That asymmetry is
 * what the parity buys, and it is the reason a false nomination one sample before a real preamble cannot swallow
 * the real frame.
 */
struct PpmScanner {
    PpmConfig   config{};
    PpmCounters counters{};

    /// @brief The nomination ratio: a position passes when its least pulse slot is at least this many times its greatest gap.
    float threshold = 2.0F;

    std::size_t _slot   = 1UZ; ///< samples per half-bit-period slot
    std::size_t _offset = 0UZ; ///< the absolute input index of the next span's first sample

    std::vector<std::size_t>  _pulseOffsets{}; ///< pulse slots as sample offsets from a candidate position
    std::vector<std::size_t>  _gapOffsets{};   ///< the remaining preamble slots, likewise
    std::vector<std::uint8_t> _octets{};       ///< the frame being read, sized once to the long length
    std::vector<float>        _sums{};         ///< one slot-wide sum per input index, used only above one sample per slot

    /// @brief The parity register, rebuilt by `prepare()` from the config's six-tuple.
    Crc _crc{24U, 0xFFF409ULL, 0ULL, 0ULL, false, false};

    /// @brief Adopts an already validated framing at @p samplesPerSlot samples a slot, sizes every buffer, and clears the counters.
    void prepare(PpmConfig cfg, std::size_t samplesPerSlot) {
        configure(cfg);
        if (samplesPerSlot == 0UZ) {
            throw std::invalid_argument("gr4adsb::PpmScanner::prepare: a slot spans at least one sample, got 0");
        }
        config = std::move(cfg);
        _slot  = samplesPerSlot;

        _pulseOffsets.clear();
        _gapOffsets.clear();
        for (std::size_t slot = 0UZ; slot < config.preambleSlots; ++slot) {
            const bool pulse = std::ranges::find(config.pulseSlots, slot) != config.pulseSlots.end();
            (pulse ? _pulseOffsets : _gapOffsets).push_back(slot * _slot);
        }
        _octets.assign(config.longBits / 8UZ, std::uint8_t{0});
        _sums.clear();
        _crc     = Crc(config.crcWidth, config.crcPoly, config.crcInit, config.crcFinalXor, config.crcInputReflected, config.crcResultReflected);
        counters = PpmCounters{};
        reset();
    }

    /// @brief Returns the scanner to its initial state; the counters belong to the caller and survive.
    void reset() noexcept { _offset = 0UZ; }

    /// @brief States where the next span's first sample sits in the stream, so reported positions are absolute.
    void seek(std::size_t absolute) noexcept { _offset = absolute; }

    [[nodiscard]] bool        configured() const noexcept { return config.preambleSlots != 0UZ; }
    [[nodiscard]] std::size_t samplesPerSlot() const noexcept { return _slot; }
    [[nodiscard]] std::size_t offset() const noexcept { return _offset; }

    /// @brief The samples one candidate needs before it can be decided: the preamble plus the longest frame.
    [[nodiscard]] std::size_t windowSamples() const noexcept { return (config.preambleSlots + 2UZ * config.longBits) * _slot; }

    /**
     * @brief Decides every position whose whole window lies inside @p magnitudes, reporting each nomination to @p onFrame.
     *
     * @param magnitudes non-negative magnitude samples at two slots per bit period; a ratio and a comparison are
     *        both invariant to positive scaling, so the amplitude is free but the quantity is not — a threshold
     *        stated on `|x|` is a different number on `|x|^2`.
     * @return how many leading samples the scanner is done with. The remainder is undecided and is presented again.
     */
    template<typename Sink>
    [[nodiscard]] std::size_t consume(std::span<const float> magnitudes, Sink&& onFrame) {
        if (!configured()) {
            return 0UZ;
        }
        const std::size_t window = windowSamples();
        if (magnitudes.size() < window) {
            return 0UZ;
        }

        // a slot's sum over a fixed run of samples in a fixed order, so the same position reads the same number
        // whatever span it arrived in; at one sample a slot the input is already that sum
        const float* sums = magnitudes.data();
        if (_slot > 1UZ) {
            const std::size_t count = magnitudes.size() - _slot + 1UZ;
            if (_sums.size() < count) {
                _sums.resize(count);
            }
            for (std::size_t j = 0UZ; j < count; ++j) {
                float total = 0.F;
                for (std::size_t k = 0UZ; k < _slot; ++k) {
                    total += magnitudes[j + k];
                }
                _sums[j] = total;
            }
            sums = _sums.data();
        }

        const std::size_t dataBase = config.preambleSlots * _slot;
        const auto        slotSpan = static_cast<float>(_slot);
        std::size_t       position = 0UZ;
        while (position + window <= magnitudes.size()) {
            float strong = std::numeric_limits<float>::max();
            for (const std::size_t offset : _pulseOffsets) {
                strong = std::min(strong, sums[position + offset]);
            }
            float weak = 0.F;
            for (const std::size_t offset : _gapOffsets) {
                weak = std::max(weak, sums[position + offset]);
            }
            if (strong <= 0.F || strong < threshold * weak) {
                ++position;
                continue;
            }

            const float*  data   = sums + position + dataBase;
            std::uint32_t format = 0U;
            for (std::size_t bit = 0UZ; bit < config.formatBits; ++bit) {
                const bool one = data[2UZ * bit * _slot] > data[(2UZ * bit + 1UZ) * _slot];
                format         = (format << 1U) | (one ? 1U : 0U);
            }
            const bool        isLong = (format >> (config.formatBits - 1UZ)) != 0U;
            const std::size_t bits   = isLong ? config.longBits : config.shortBits;

            const std::size_t             octets = bits / 8UZ;
            const std::span<std::uint8_t> frame(_octets.data(), octets);
            std::ranges::fill(frame, std::uint8_t{0});
            for (std::size_t bit = 0UZ; bit < bits; ++bit) {
                if (data[2UZ * bit * _slot] > data[(2UZ * bit + 1UZ) * _slot]) {
                    frame[bit / 8UZ] = static_cast<std::uint8_t>(frame[bit / 8UZ] | (0x80U >> (bit % 8UZ)));
                }
            }

            const std::uint64_t remainder = remainderOf(frame);
            const PpmFrame      reported{.octets = frame,
                     .bits                       = bits,
                     .format                     = format,
                     .remainder                  = remainder, //
                     .strong                     = strong / slotSpan,
                     .weak                       = weak / slotSpan,
                     .position                   = _offset + position, //
                     .outcome                    = isLong ? (remainder == 0ULL ? PpmOutcome::Admitted : PpmOutcome::CrcFailed) : PpmOutcome::ShortFormat};

            ++counters.nominations;
            switch (reported.outcome) {
            case PpmOutcome::Admitted: ++counters.admitted; break;
            case PpmOutcome::CrcFailed: ++counters.crcFailed; break;
            case PpmOutcome::ShortFormat: ++counters.shortFormat; break;
            }
            onFrame(reported);

            // only a frame the parity vouches for hides what follows it; every other nomination yields a sample
            position += reported.admitted() ? window : 1UZ;
        }

        _offset += position;
        return position;
    }

    /**
     * @brief The whole frame's remainder: the message octets' CRC XOR'd with the parity field as it arrived.
     *
     * A frame is the message followed by the remainder of the message shifted up by the register width, so the
     * whole frame divides by the generator exactly when the parity field is that remainder, and the difference is
     * what a division of the whole frame leaves. Writing it as one CRC and one XOR reads the parity field where it
     * lies rather than dividing the frame twice, and it is what makes an XOR'd address fall out unchanged.
     */
    [[nodiscard]] std::uint64_t remainderOf(std::span<const std::uint8_t> frame) const noexcept {
        const std::size_t parityBytes  = static_cast<std::size_t>(config.crcWidth) / 8UZ;
        const std::size_t messageBytes = frame.size() - parityBytes;
        std::uint64_t     transmitted  = 0ULL;
        for (const std::uint8_t octet : frame.subspan(messageBytes)) {
            transmitted = (transmitted << 8U) | octet;
        }
        return _crc.compute(frame.first(messageBytes)) ^ transmitted;
    }
};

} // namespace gr4adsb

#endif // GR4ADSB_PPM_HPP
