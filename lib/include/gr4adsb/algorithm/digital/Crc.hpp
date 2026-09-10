#ifndef GR4ADSB_CRC_HPP
#define GR4ADSB_CRC_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

/**
 * @brief One parameterized cyclic redundancy check, from the six-tuple that names essentially all of them.
 *
 * `(width, polynomial, initialValue, finalXor, inputReflected, resultReflected)` is the Rocksoft model
 * [Williams 1993], and the RevEng catalog [Cook] publishes a check value, the CRC of the nine ASCII
 * bytes `"123456789"`, for each named set, so a parameter set can be checked against it. Every table
 * here is generated from the polynomial at construction.
 *
 * Two conventions decide whether a ported parameter set reproduces its published check value:
 *
 * - `initialValue` is in the unreflected domain, the domain the catalog's `INIT` column is in. A
 *   reflected set runs on a mirrored register, which the constructor seeds with
 *   `reverseBits(initialValue, width)`; a seed that is already reflected answers differently. The
 *   difference is invisible for a palindrome, and `0x0000`, `0xFFFF` and `0xFFFFFFFF` all reverse to
 *   themselves, which covers the seeds in common use.
 * - `inputReflected` reflects the message byte, not the polynomial. Reflecting the polynomial and
 *   mirroring the register is the equivalent implementation used here, and it does not inherit the
 *   `INIT` domain automatically.
 *
 * One of three table forms is chosen at construction, so neither direction reflects a byte on the hot
 * path: MSB-first for an unreflected `width >= 8`, mirrored for a reflected one, and register and
 * polynomial left-aligned into a byte for `width < 8`, which has fewer than 8 bits to shift out. A
 * mirrored register already holds the reflected result, so `resultReflected` is applied to it only
 * when it differs from `inputReflected`.
 *
 * `width` is in `[3, 64]` and is validated before anything derived from it is computed, since
 * `(1 << width) - 1` is undefined for `width >= 65`. `polynomial`, `initialValue` and `finalXor` are
 * masked to `width` bits on every branch, so an over-wide polynomial cannot mean one thing reflected
 * and another not.
 *
 * The kernel is immutable after construction and `compute` is `const` and `noexcept`, so one instance
 * is safe to call concurrently. Each instance carries its own 2 KiB table by value.
 *
 * The model is byte-oriented. A message that is not a whole number of bytes, and a CRC over a
 * non-byte-aligned bit field, are outside it and are not offered. `width` below 8 refers to the
 * register, not the message.
 */
namespace gr4adsb {

/// @brief The low `width` bits of `value`, reversed.
[[nodiscard]] inline constexpr std::uint64_t reverseBits(std::uint64_t value, std::uint8_t width) noexcept {
    std::uint64_t result = 0ULL;
    for (std::uint8_t bit = 0U; bit < width; ++bit) {
        result = (result << 1U) | ((value >> bit) & 1ULL);
    }
    return result;
}

class Crc {
public:
    /// @brief Which table the parameters admit; see the file comment. Resolved once, never per byte.
    enum class TableForm { MsbFirst, Mirrored, LeftAligned };

    Crc(std::uint8_t width, std::uint64_t polynomial, std::uint64_t initialValue = 0ULL, std::uint64_t finalXor = 0ULL, bool inputReflected = false, bool resultReflected = false) {
        if (width < 3U || width > 64U) {
            throw std::invalid_argument("gr4adsb::Crc: width must be in [3, 64], got " + std::to_string(static_cast<unsigned>(width)));
        }
        width_           = width;
        mask_            = width == 64U ? ~0ULL : ((1ULL << width) - 1ULL);
        polynomial_      = polynomial & mask_;
        initialValue_    = initialValue & mask_;
        finalXor_        = finalXor & mask_;
        inputReflected_  = inputReflected;
        resultReflected_ = resultReflected;
        form_            = width_ < 8U ? TableForm::LeftAligned : (inputReflected ? TableForm::Mirrored : TableForm::MsbFirst);

        switch (form_) {
        case TableForm::Mirrored: buildMirrored(); break;
        case TableForm::LeftAligned: buildLeftAligned(); break;
        default: buildMsbFirst(); break;
        }
    }

    [[nodiscard]] std::uint64_t compute(std::span<const std::uint8_t> message) const noexcept {
        switch (form_) {
        case TableForm::Mirrored: {
            std::uint64_t reg = seed_;
            for (const std::uint8_t byte : message) {
                reg = table_[(reg ^ byte) & 0xFFULL] ^ (reg >> 8U);
            }
            return ((inputReflected_ != resultReflected_ ? reverseBits(reg, width_) : reg) ^ finalXor_) & mask_;
        }
        case TableForm::LeftAligned: {
            std::uint64_t reg = seed_;
            if (inputReflected_) {
                for (const std::uint8_t byte : message) {
                    reg = table_[(reg ^ reverseByte(byte)) & 0xFFULL];
                }
            } else {
                for (const std::uint8_t byte : message) {
                    reg = table_[(reg ^ byte) & 0xFFULL];
                }
            }
            reg >>= 8U - width_;
            return ((resultReflected_ ? reverseBits(reg, width_) : reg) ^ finalXor_) & mask_;
        }
        default: {
            std::uint64_t      reg   = seed_;
            const std::uint8_t shift = static_cast<std::uint8_t>(width_ - 8U);
            for (const std::uint8_t byte : message) {
                reg = ((reg << 8U) & mask_) ^ table_[((reg >> shift) ^ byte) & 0xFFULL];
            }
            return ((resultReflected_ ? reverseBits(reg, width_) : reg) ^ finalXor_) & mask_;
        }
        }
    }

    [[nodiscard]] std::uint8_t                   width() const noexcept { return width_; }
    [[nodiscard]] std::uint64_t                  polynomial() const noexcept { return polynomial_; }
    [[nodiscard]] std::uint64_t                  initialValue() const noexcept { return initialValue_; }
    [[nodiscard]] std::uint64_t                  finalXor() const noexcept { return finalXor_; }
    [[nodiscard]] bool                           inputReflected() const noexcept { return inputReflected_; }
    [[nodiscard]] bool                           resultReflected() const noexcept { return resultReflected_; }
    [[nodiscard]] std::uint64_t                  mask() const noexcept { return mask_; }
    [[nodiscard]] TableForm                      tableForm() const noexcept { return form_; }
    [[nodiscard]] std::span<const std::uint64_t> table() const noexcept { return table_; }

private:
    [[nodiscard]] static constexpr std::uint64_t reverseByte(std::uint8_t byte) noexcept { return reverseBits(byte, 8U); }

    void buildMsbFirst() {
        const std::uint64_t top = 1ULL << (width_ - 1U);
        for (std::size_t i = 0UZ; i < 256UZ; ++i) {
            std::uint64_t remainder = static_cast<std::uint64_t>(i) << (width_ - 8U);
            for (int step = 0; step < 8; ++step) {
                remainder = (remainder & top) != 0ULL ? (((remainder << 1U) ^ polynomial_) & mask_) : ((remainder << 1U) & mask_);
            }
            table_[i] = remainder;
        }
        seed_ = initialValue_;
    }

    void buildMirrored() {
        const std::uint64_t reflected = reverseBits(polynomial_, width_);
        for (std::size_t i = 0UZ; i < 256UZ; ++i) {
            std::uint64_t remainder = static_cast<std::uint64_t>(i);
            for (int step = 0; step < 8; ++step) {
                remainder = (remainder & 1ULL) != 0ULL ? ((remainder >> 1U) ^ reflected) : (remainder >> 1U);
            }
            table_[i] = remainder & mask_;
        }
        seed_ = reverseBits(initialValue_, width_);
    }

    void buildLeftAligned() {
        const std::uint64_t aligned = (polynomial_ << (8U - width_)) & 0xFFULL;
        for (std::size_t i = 0UZ; i < 256UZ; ++i) {
            std::uint64_t remainder = static_cast<std::uint64_t>(i);
            for (int step = 0; step < 8; ++step) {
                remainder = (remainder & 0x80ULL) != 0ULL ? (((remainder << 1U) ^ aligned) & 0xFFULL) : ((remainder << 1U) & 0xFFULL);
            }
            table_[i] = remainder;
        }
        seed_ = (initialValue_ << (8U - width_)) & 0xFFULL;
    }

    alignas(64) std::array<std::uint64_t, 256> table_{};
    std::uint64_t seed_{};
    std::uint64_t mask_{};
    std::uint64_t polynomial_{};
    std::uint64_t initialValue_{};
    std::uint64_t finalXor_{};
    std::uint8_t  width_{};
    bool          inputReflected_{};
    bool          resultReflected_{};
    TableForm     form_{TableForm::MsbFirst};
};

} // namespace gr4adsb

#endif // GR4ADSB_CRC_HPP
