#ifndef GR4ADSB_ADSB_RECORD_FIELDS_HPP
#define GR4ADSB_ADSB_RECORD_FIELDS_HPP

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include <gnuradio-4.0/Tag.hpp>

namespace adsb::detail {

using gr::property_map;

/*| role: typed reads of a frame record's metadata, shared by the blocks that consume PpmFramer's records.
    contract: every read takes the exact declared type. A key that is absent or holds another type reads as
        nothing, never as a converted value.
*/
template<typename T>
[[nodiscard]] inline std::optional<T> read(const property_map& meta, std::string_view key) {
    const auto entry = meta.find(property_map::key_type(key));
    if (entry == meta.end()) {
        return std::nullopt;
    }
    const T* value = entry->second.template get_if<T>();
    return value == nullptr ? std::nullopt : std::optional<T>{*value};
}

/*| frame: a metadata map holds a string as std::pmr::string; this returns a std::string copy.
 */
[[nodiscard]] inline std::optional<std::string> readText(const property_map& meta, std::string_view key) {
    const std::optional<std::pmr::string> value = read<std::pmr::string>(meta, key);
    return value.has_value() ? std::optional<std::string>{std::string(*value)} : std::nullopt;
}

/*| contract: seconds into the stream at which the frame started, sample_start / sample_rate, the two keys
        PpmFramer writes. Nothing when either key is missing or the rate is not positive.
*/
[[nodiscard]] inline std::optional<double> timeOf(const property_map& meta) {
    const std::optional<std::uint64_t> start = read<std::uint64_t>(meta, "sample_start");
    const std::optional<float>         rate  = read<float>(meta, "sample_rate");
    if (!start.has_value() || !rate.has_value() || !(*rate > 0.f)) {
        return std::nullopt;
    }
    return static_cast<double>(*start) / static_cast<double>(*rate);
}

} // namespace adsb::detail

#endif // GR4ADSB_ADSB_RECORD_FIELDS_HPP
