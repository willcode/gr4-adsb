#ifndef GR4ADSB_TEST_SPANS_HPP
#define GR4ADSB_TEST_SPANS_HPP

#include <cstddef>
#include <functional>
#include <span>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Tag.hpp>

// Minimal ReaderSpanLike/WriterSpanLike pair, so a processBulk can be driven at an exact chunk size and from an exact
// absolute stream position without standing up a graph and a scheduler.
namespace gr4adsb::test {

struct TagReaderSpan : std::span<const gr::Tag> {
    using value_type          = gr::Tag;
    constexpr TagReaderSpan() = default;
    constexpr TagReaderSpan(std::span<const gr::Tag> tags) : std::span<const gr::Tag>(tags) {}
    constexpr bool consume(std::size_t) const noexcept { return true; }
};

struct TagWriterSpan : std::span<gr::Tag> {
    using value_type          = gr::Tag;
    constexpr TagWriterSpan() = default;
    constexpr TagWriterSpan(std::span<gr::Tag> tags) : std::span<gr::Tag>(tags) {}
    constexpr void publish(std::size_t) const noexcept {}
};

using TagView = std::pair<std::ptrdiff_t, std::reference_wrapper<const gr::property_map>>;

template<typename T>
struct InputSpan : std::span<const T> {
    using value_type = T;

    TagReaderSpan rawTags{};
    std::size_t   streamIndex = 0UZ;
    std::size_t   consumed    = 0UZ;
    bool          isConnected = true;
    bool          isSync      = true;

    InputSpan(std::span<const T> samples, std::size_t at = 0UZ, std::span<const gr::Tag> incoming = {}) : std::span<const T>(samples), rawTags(incoming), streamIndex(at) {}

    constexpr bool consume(std::size_t nSamples) noexcept {
        consumed = nSamples;
        return true;
    }
    constexpr void consumeTags(std::size_t) noexcept {}

    [[nodiscard]] std::vector<TagView> tags() const { return tags(this->size()); }

    [[nodiscard]] std::vector<TagView> tags(std::size_t window) const {
        std::vector<TagView> view;
        for (const gr::Tag& tag : rawTags) {
            const std::ptrdiff_t relIndex = static_cast<std::ptrdiff_t>(tag.index) - static_cast<std::ptrdiff_t>(streamIndex);
            if (relIndex < static_cast<std::ptrdiff_t>(window)) {
                view.emplace_back(relIndex, std::cref(tag.map));
            }
        }
        return view;
    }
};

template<typename T>
struct OutputSpan : std::span<T> {
    using value_type = T;

    std::vector<gr::Tag>* sink = nullptr;
    TagWriterSpan         tags{};
    std::size_t           streamIndex = 0UZ;
    std::size_t           count       = 0UZ;
    bool                  isConnected = true;
    bool                  isSync      = true;

    OutputSpan(std::span<T> samples, std::size_t at = 0UZ, std::vector<gr::Tag>* published = nullptr, bool connected = true) : std::span<T>(samples), sink(published), streamIndex(at), isConnected(connected) {}

    constexpr void publish(std::size_t nSamples) noexcept { count = nSamples; }

    void publishTag(const gr::property_map& tagData, std::size_t tagOffset = 0UZ) {
        if (!isConnected || sink == nullptr) {
            return;
        }
        sink->push_back(gr::Tag{streamIndex + tagOffset, tagData});
    }
};

} // namespace gr4adsb::test

#endif // GR4ADSB_TEST_SPANS_HPP
