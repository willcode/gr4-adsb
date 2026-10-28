#ifndef GR4ADSB_ADSB_MODE_S_HPP
#define GR4ADSB_ADSB_MODE_S_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <memory_resource>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/DataSet.hpp>
#include <gnuradio-4.0/Port.hpp>
#include <gnuradio-4.0/annotated.hpp>

#include <adsb/BlockLabels.hpp>
#include <adsb/RecordFields.hpp>

/*| role: the message layer of the 1090 MHz Mode S downlink: the bit-field readers of the frame and the
        ModeSDecode block, a record adapter over DataSet<std::uint8_t>.
    frame: a frame is 7 or 14 whole octets from PpmFramer at the mode_s profile. Bit numbers follow the
        standard: bit 1 is the most significant bit of the first octet.
    origin: ICAO Annex 10 Volume IV for the framing and the AC altitude code; RTCA DO-260B for the extended
        squitter messages.
*/
namespace adsb {

using gr::Annotated;
using gr::Async;
using gr::Block;
using gr::DataSet;
using gr::Doc;
using gr::InputSpanLike;
using gr::OutputSpanLike;
using gr::PortIn;
using gr::PortOut;
using gr::property_map;
using gr::Unit;
using gr::Visible;
namespace pmt  = gr::pmt;
namespace work = gr::work;

namespace detail {

/*| contract: count bits of frame starting at one-based bit number first, most significant first. Bits past the
        frame's end read as zero.
*/
[[nodiscard]] inline std::uint32_t field(std::span<const std::uint8_t> frame, std::size_t first, std::size_t count) noexcept {
    std::uint32_t value = 0U;
    for (std::size_t i = 0UZ; i < count; ++i) {
        const std::size_t  bit   = first - 1UZ + i;
        const std::uint8_t octet = bit / 8UZ < frame.size() ? frame[bit / 8UZ] : std::uint8_t{0};
        value                    = (value << 1U) | ((octet >> (7UZ - bit % 8UZ)) & 1U);
    }
    return value;
}

/*| frame: the six-bit character set of the identification message. `#` marks an unassigned code, which
        identityOf() drops rather than renders.
    origin: RTCA DO-260B 2.2.3.2.
*/
constexpr std::string_view kIdentityCharacters = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";

/*| contract: the eight-character identification of a type code 1 to 4 message, bits 41 to 88, with
        unassigned codes dropped and the trailing space pad removed.
*/
[[nodiscard]] inline std::string identityOf(std::span<const std::uint8_t> frame) {
    std::string name;
    for (std::size_t i = 0UZ; i < 8UZ; ++i) {
        const char character = kIdentityCharacters[field(frame, 41UZ + 6UZ * i, 6UZ)];
        if (character != '#') {
            name.push_back(character);
        }
    }
    while (!name.empty() && name.back() == ' ') {
        name.pop_back();
    }
    return name;
}

/*| contract: the pressure altitude in feet of the AC field's 100 ft code (Q bit clear), or nothing for an
        unassigned C code or a result below the code floor of -1200 ft.
    frame: the twelve bits, most significant first: C1 A1 C2 A2 C4 A4 B1 Q B2 D2 B4 D4. A count of 500 ft
        steps is a reflected binary code on D2 D4 A1 A2 A4 B1 B2 B4. The 100 ft digit within the step is in
        the three C pulses and counts the other way on an odd step. Three of the eight C codes are unassigned.
    origin: ICAO Annex 10 Volume IV, the AC altitude code.
    verified-by: qa_ModeSDecode "the hundred foot code decodes where the Q bit is clear, which transponders in service still send"
*/
[[nodiscard]] inline std::optional<std::int32_t> gillhamAltitude(std::uint32_t ac12) noexcept {
    const auto pulse = [ac12](std::size_t position) { return (ac12 >> (12UZ - position)) & 1U; };

    /*| frame: the five assigned C codes, C1 C2 C4 read as a three-bit number, each with its 100 ft digit.
     */
    constexpr std::array<std::pair<std::uint32_t, std::int32_t>, 5UZ> kHundreds{{{0b001U, 1}, {0b011U, 2}, {0b010U, 3}, {0b110U, 4}, {0b100U, 5}}};
    const std::uint32_t                                               code  = (pulse(1UZ) << 2U) | (pulse(3UZ) << 1U) | pulse(5UZ);
    const auto                                                        digit = std::ranges::find(kHundreds, code, [](const auto& entry) { return entry.first; });
    if (digit == kHundreds.end()) {
        return std::nullopt;
    }

    /*| frame: the 500 ft step pulses D2 D4 A1 A2 A4 B1 B2 B4 by their field positions, most significant first.
     */
    constexpr std::array<std::size_t, 8UZ> kStepPulses{10UZ, 12UZ, 2UZ, 4UZ, 6UZ, 7UZ, 9UZ, 11UZ};
    std::int32_t                           steps   = 0;
    std::uint32_t                          running = 0U;
    for (const std::size_t position : kStepPulses) {
        /*| why: a reflected binary code decodes as the running parity of its leading bits.
         */
        running ^= pulse(position);
        steps = (steps << 1) | static_cast<std::int32_t>(running);
    }

    const std::int32_t hundreds = steps % 2 == 0 ? digit->second : 6 - digit->second;
    const std::int32_t feet     = 500 * steps + 100 * hundreds - 1300;
    return feet >= -1200 ? std::optional<std::int32_t>{feet} : std::nullopt;
}

/*| contract: the barometric altitude in feet of the twelve-bit AC field, or nothing where it states none. All
        zeros states no altitude.
    frame: the Q bit, field position 8, selects the coding. Set: the other eleven bits are 25 ft increments
        from -1000 ft. Clear: the 100 ft code of gillhamAltitude(), which transponders in service still send.
    origin: RTCA DO-260B 2.2.3.2.
    verified-by: qa_ModeSDecode "the twenty-five foot altitude field decodes where its Q bit is set"
*/
[[nodiscard]] inline std::optional<std::int32_t> baroAltitude(std::uint32_t ac12) noexcept {
    if (ac12 == 0U) {
        return std::nullopt;
    }
    if (((ac12 >> 4U) & 1U) != 0U) {
        const auto increments = static_cast<std::int32_t>(((ac12 >> 5U) << 4U) | (ac12 & 0x0FU));
        return increments * 25 - 1000;
    }
    return gillhamAltitude(ac12);
}

/*| contract: the number of longitude zones at latitude degrees, the NL function.
    trap: the two poleward cases and the cap of 59 are the defined values where the equation degenerates. At
        the equator the quotient is exactly 60, and the cap keeps the floor at 59.
    origin: RTCA DO-260B A.1.7.2.2.
*/
[[nodiscard]] inline std::int32_t longitudeZones(double degrees) noexcept {
    constexpr double kZones = 15.0; /*| frame: NZ, the geographic latitude zones in a quadrant */
    const double     from   = std::abs(degrees);
    if (from >= 87.0) {
        return from > 87.0 ? 1 : 2;
    }
    const double cosine    = std::cos(std::numbers::pi / 180.0 * from);
    const double numerator = 1.0 - std::cos(std::numbers::pi / (2.0 * kZones));
    return std::min(59, static_cast<std::int32_t>(std::floor(2.0 * std::numbers::pi / std::acos(1.0 - numerator / (cosine * cosine)))));
}

/*| contract: floored modulo, the remainder the compact position arithmetic is defined over.
    trap: std::fmod keeps the sign of its left operand and puts a negative zone index hundreds of degrees away
        rather than one zone over.
*/
[[nodiscard]] inline double flooredMod(double value, double modulus) noexcept { return value - modulus * std::floor(value / modulus); }

/*| role: one half of a compact position pair, held until its complement arrives.
    frame: latitude and longitude are the seventeen-bit encoded values; at is seconds into the stream from the
        frame's own sample position.
*/
struct CprHalf {
    std::uint32_t latitude  = 0U;
    std::uint32_t longitude = 0U;
    double        at        = 0.0;
    bool          have      = false;
};

/*| contract: the globally unambiguous latitude and longitude in degrees that an even and an odd half name,
        taken from the newer half. Nothing when the two decoded latitudes give different NL, as a pair straddling
        a latitude where the zone count changes does; the next pair resolves.
    origin: RTCA DO-260B A.1.7.4.
    verified-by: qa_ModeSDecode "an even and an odd half resolve the position they were encoded from, to the format's own resolution"
*/
[[nodiscard]] inline std::optional<std::pair<double, double>> globalPosition(const CprHalf& even, const CprHalf& odd) noexcept {
    constexpr double kScale = 131072.0; /*| frame: 2^17, the encoded fraction of a zone */

    const double evenLatitude = static_cast<double>(even.latitude) / kScale;
    const double oddLatitude  = static_cast<double>(odd.latitude) / kScale;
    const double index        = std::floor(59.0 * evenLatitude - 60.0 * oddLatitude + 0.5);

    double fromEven = (360.0 / 60.0) * (flooredMod(index, 60.0) + evenLatitude);
    double fromOdd  = (360.0 / 59.0) * (flooredMod(index, 59.0) + oddLatitude);
    if (fromEven >= 270.0) {
        /*| frame: the southern hemisphere, which the zone index states as the range above 270 degrees.
         */
        fromEven -= 360.0;
    }
    if (fromOdd >= 270.0) {
        fromOdd -= 360.0;
    }
    if (longitudeZones(fromEven) != longitudeZones(fromOdd)) {
        return std::nullopt;
    }

    const bool         evenNewer = even.at >= odd.at;
    const double       latitude  = evenNewer ? fromEven : fromOdd;
    const std::int32_t zones     = longitudeZones(latitude);

    const double evenLongitude = static_cast<double>(even.longitude) / kScale;
    const double oddLongitude  = static_cast<double>(odd.longitude) / kScale;
    const double zoneIndex     = std::floor(evenLongitude * static_cast<double>(zones - 1) - oddLongitude * static_cast<double>(zones) + 0.5);
    const auto   width         = static_cast<double>(std::max(evenNewer ? zones : zones - 1, 1));

    double longitude = (360.0 / width) * (flooredMod(zoneIndex, width) + (evenNewer ? evenLongitude : oddLongitude));
    if (longitude >= 180.0) {
        longitude -= 360.0;
    }
    return std::pair{latitude, longitude};
}

/*| contract: the adsb_message name of an extended squitter's type code.
    origin: RTCA DO-260B 2.2.3.2.
*/
[[nodiscard]] inline std::string_view messageOf(std::uint32_t typeCode) noexcept {
    if (typeCode == 0U) {
        return "no_position";
    }
    if (typeCode <= 4U) {
        return "identification";
    }
    if (typeCode <= 8U) {
        return "surface_position";
    }
    if (typeCode <= 18U) {
        return "airborne_position";
    }
    if (typeCode == 19U) {
        return "airborne_velocity";
    }
    if (typeCode <= 22U) {
        /*| frame: the airborne position layout with a height above the ellipsoid, left undecoded.
         */
        return "airborne_position_gnss";
    }
    if (typeCode == 28U) {
        return "aircraft_status";
    }
    if (typeCode == 29U) {
        return "target_state";
    }
    if (typeCode == 31U) {
        return "operational_status";
    }
    return "reserved";
}

/*| contract: true for the downlink formats that carry the address in bits 9 to 32: 11 (all-call reply), 17
        and 18 (the squitters). The other formats overlay the address on the parity, so they state none here.
*/
[[nodiscard]] inline bool announcesAddress(std::uint32_t downlinkFormat) noexcept { return downlinkFormat == 11U || downlinkFormat == 17U || downlinkFormat == 18U; }

} // namespace detail

GR_REGISTER_BLOCK(adsb::ModeSDecode)

/*| role: the block between PpmFramer and AdsbPrinter. Reads the downlink message of each Mode S frame record into
        typed metadata beside its octets.
    contract: the output items are the input's octets unchanged, so the framer's parity still covers them. The
        input's metadata carries through, with this block's keys written over it.
    contract: a frame is decoded only when crc_ok is true; the framer writes crc_ok on long frames alone. Otherwise
        it crosses undecoded with protocol mode_s/short (no crc_ok: a short-format nomination) or mode_s/unchecked
        (crc_ok false), and nUnchecked counts it.
    contract: protocol, general to specific: mode_s/adsb on a decoded extended squitter (formats 17, 18), mode_s
        on any other decoded frame. mode_s_format is rewritten from the octets; mode_s_icao is written on formats
        17 and 18; DF11 also announces its address but is a short frame and never reaches the decode. Extended squitters add adsb_type_code, adsb_message and that message's
        adsb_* fields.
    contract: a position needs an even and an odd half from one address. table_size bounds the per-address table of
        halves; when full, the address seen least recently is dropped (nEvicted). A pair whose halves are more than
        pair_seconds apart is refused (nPairsExpired). Every airborne position frame carries its own encoded half
        under adsb_cpr_*, resolved or not; a resolved pair adds adsb_latitude and adsb_longitude (nPositions). A half
        pairs only on a record carrying sample_start and sample_rate (detail::timeOf); without them it is
        published unpaired and uncounted.
    contract: a change to pair_seconds or table_size applies at once. A pair_seconds that is not finite and positive,
        or a table_size of zero, throws and keeps the previous value.
    contract: nFrames counts records in, nDecoded records decoded. The counters are plain members: read them on the
        block's thread or after the graph stops.
    verified-by: qa_ModeSDecode "an identification message names the aircraft, and the format is read from the octets not the key"
    verified-by: qa_ModeSDecode "a frame the parity did not vouch for crosses undecoded, and the next one decodes"
    verified-by: qa_ModeSDecode "a half with no complement names no position, and a pair older than the limit is refused"
    verified-by: qa_ModeSDecode "the pairing table is bounded, and the address it gives up is the one least recently seen"
    verified-by: qa_ModeSDecode "a setting outside its range is refused by name and leaves the block at the value it had"
    verified-by: qa_ModeSDecode "every decoded field survives the record to packet boundary and the metadata encoder, at its own type"
*/
struct ModeSDecode : Block<ModeSDecode> {
    using Description = Doc<R""(
@brief Reads the downlink message of one Mode S frame into typed metadata beside its octets.

The layer above `adsb::PpmFramer`. The framing is ICAO Annex 10 Volume IV and the extended squitter's message
formats are RTCA DO-260B. The block reads the downlink format, the announced address, and on formats 17 and 18 the
type code and the message it names: the aircraft identification, the barometric altitude in both of its codings,
the compact position halves and the position a pair of them resolves to, and the airborne velocity. The ME field
that carries the message occupies bits 33 to 88 of a long frame.

Every decoded field is a metadata key holding an integer, a double, a bool or a string, and the payload stays the
frame's octets, so a record crosses a process whole.
)"">;
#ifdef GR4ADSB_BLOCK_LABELS
    static constexpr auto attributes = gr::block::describe(1U, kFamilyLabel, gr::block::labels::role::processor);
#endif

    PortIn<DataSet<std::uint8_t>, Async>  in;
    PortOut<DataSet<std::uint8_t>, Async> out;

    Annotated<float, "pair_seconds", Unit<"s">, Doc<"greatest age difference within a position pair">, Visible> pair_seconds{10.f};
    Annotated<gr::Size_t, "table_size", Doc<"aircraft addresses the pairing table holds">, Visible>             table_size{256U};

    GR_MAKE_REFLECTABLE(ModeSDecode, in, out, pair_seconds, table_size);

    std::uint64_t nFrames       = 0ULL;
    std::uint64_t nDecoded      = 0ULL;
    std::uint64_t nPositions    = 0ULL;
    std::uint64_t nPairsExpired = 0ULL;
    std::uint64_t nUnchecked    = 0ULL;
    std::uint64_t nEvicted      = 0ULL;

    struct Aircraft {
        detail::CprHalf even{};
        detail::CprHalf odd{};
        std::uint64_t   touched = 0ULL;
    };

    std::map<std::uint32_t, Aircraft> _aircraft{};
    std::uint64_t                     _touch = 0ULL; /*| invariant: monotonic; the least recently seen address holds the smallest touched */
    float                             _pair  = 10.f;
    std::size_t                       _bound = 256UZ;

    void settingsChanged(const property_map& /*oldSettings*/, const property_map& /*newSettings*/) { rebuild(); }

    void start() { rebuild(); }

    void reset() {
        _aircraft.clear();
        _touch = 0ULL;
    }

    void rebuild() {
        if (!std::isfinite(pair_seconds.value) || pair_seconds.value <= 0.f) {
            const float rejected = pair_seconds.value;
            pair_seconds.value   = _pair;
            throw gr::exception(std::format("pair_seconds is {}; it must be finite and greater than zero", rejected));
        }
        if (table_size.value == 0U) {
            table_size.value = static_cast<gr::Size_t>(_bound);
            throw gr::exception("table_size is zero; a pairing table that holds no aircraft can resolve no position");
        }
        _pair  = pair_seconds.value;
        _bound = static_cast<std::size_t>(table_size.value);
        while (_aircraft.size() > _bound) {
            evictOldest();
        }
    }

    [[nodiscard]] work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t moved = std::min(inSpan.size(), outSpan.size());
        for (std::size_t i = 0UZ; i < moved; ++i) {
            outSpan[i] = decode(inSpan[i]);
        }
        std::ignore = inSpan.consume(moved);
        outSpan.publish(moved);
        if (moved == 0UZ) {
            return outSpan.size() == 0UZ ? work::Status::INSUFFICIENT_OUTPUT_ITEMS : work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        return work::Status::OK;
    }

    [[nodiscard]] DataSet<std::uint8_t> decode(const DataSet<std::uint8_t>& record) {
        ++nFrames;

        DataSet<std::uint8_t> made;
        made.signal_values = record.signal_values;
        made.extents.push_back(static_cast<std::int32_t>(made.signal_values.size()));
        made.signal_names.emplace_back(record.signal_names.empty() ? std::string("mode_s") : record.signal_names[0UZ]);
        made.signal_quantities.emplace_back("");
        made.signal_units.emplace_back("");
        made.timing_events = record.timing_events;
        made.timing_events.resize(1UZ);
        made.meta_information.resize(1UZ);
        made.timestamp = record.timestamp;

        property_map& meta = made.meta_information[0UZ];
        if (!record.meta_information.empty()) {
            meta = record.meta_information[0UZ];
        }

        const std::optional<bool> checked = detail::read<bool>(meta, "crc_ok");
        if (!checked.has_value() || !*checked) {
            ++nUnchecked;
            meta.insert_or_assign(property_map::key_type("protocol"), pmt::Value(std::string(checked.has_value() ? "mode_s/unchecked" : "mode_s/short")));
            return made;
        }

        const std::span<const std::uint8_t> frame          = std::span<const std::uint8_t>(made.signal_values);
        const std::uint32_t                 downlinkFormat = detail::field(frame, 1UZ, 5UZ);
        ++nDecoded;
        meta.insert_or_assign(property_map::key_type("mode_s_format"), pmt::Value(gr::Size_t{downlinkFormat}));
        if (detail::announcesAddress(downlinkFormat)) {
            meta.insert_or_assign(property_map::key_type("mode_s_icao"), pmt::Value(std::format("{:06X}", detail::field(frame, 9UZ, 24UZ))));
        }
        if (downlinkFormat != 17U && downlinkFormat != 18U) {
            meta.insert_or_assign(property_map::key_type("protocol"), pmt::Value(std::string("mode_s")));
            return made;
        }

        meta.insert_or_assign(property_map::key_type("protocol"), pmt::Value(std::string("mode_s/adsb")));
        readExtendedSquitter(frame, meta);
        return made;
    }

    /*| frame: the ME field of a long frame, bits 33 to 88; its first five bits are the type code.
     */
    void readExtendedSquitter(std::span<const std::uint8_t> frame, property_map& meta) {
        const std::uint32_t typeCode = detail::field(frame, 33UZ, 5UZ);
        meta.insert_or_assign(property_map::key_type("adsb_type_code"), pmt::Value(gr::Size_t{typeCode}));
        meta.insert_or_assign(property_map::key_type("adsb_message"), pmt::Value(std::string(detail::messageOf(typeCode))));

        if (typeCode >= 1U && typeCode <= 4U) {
            /*| frame: aircraft identification: an emitter category, bits 38 to 40, and eight six-bit characters.
             */
            meta.insert_or_assign(property_map::key_type("adsb_emitter_category"), pmt::Value(gr::Size_t{detail::field(frame, 38UZ, 3UZ)}));
            meta.insert_or_assign(property_map::key_type("adsb_callsign"), pmt::Value(detail::identityOf(frame)));
            return;
        }
        if (typeCode >= 9U && typeCode <= 18U) {
            readAirbornePosition(frame, meta);
            return;
        }
        if (typeCode == 19U) {
            readAirborneVelocity(frame, meta);
        }
    }

    /*| frame: the airborne position message: the AC altitude field, bits 41 to 52, and one half of a position pair.
        origin: RTCA DO-260B 2.2.3.2.
    */
    void readAirbornePosition(std::span<const std::uint8_t> frame, property_map& meta) {
        const std::uint32_t altitudeCode = detail::field(frame, 41UZ, 12UZ);
        meta.insert_or_assign(property_map::key_type("adsb_altitude_q"), pmt::Value(((altitudeCode >> 4U) & 1U) != 0U));
        if (const std::optional<std::int32_t> feet = detail::baroAltitude(altitudeCode); feet.has_value()) {
            meta.insert_or_assign(property_map::key_type("adsb_altitude_ft"), pmt::Value(*feet));
        }

        /*| frame: bit 54 is the CPR format, 0 even and 1 odd; the seventeen-bit latitude and longitude follow it.
         */
        const std::uint32_t format = detail::field(frame, 54UZ, 1UZ);
        detail::CprHalf     half{.latitude = detail::field(frame, 55UZ, 17UZ), .longitude = detail::field(frame, 72UZ, 17UZ), .at = 0.0, .have = true};
        meta.insert_or_assign(property_map::key_type("adsb_cpr_format"), pmt::Value(gr::Size_t{format}));
        meta.insert_or_assign(property_map::key_type("adsb_cpr_latitude"), pmt::Value(gr::Size_t{half.latitude}));
        meta.insert_or_assign(property_map::key_type("adsb_cpr_longitude"), pmt::Value(gr::Size_t{half.longitude}));

        const std::optional<std::string> address = detail::readText(meta, "mode_s_icao");
        const std::optional<double>      when    = detail::timeOf(meta);
        if (!address.has_value() || !when.has_value()) {
            /*| contract: a half with no address to pair against, or no time to age it by, is published unpaired.
             */
            return;
        }
        half.at = *when;

        Aircraft& state                         = touch(detail::field(frame, 9UZ, 24UZ));
        (format != 0U ? state.odd : state.even) = half;
        if (!state.even.have || !state.odd.have) {
            return;
        }
        if (std::abs(state.even.at - state.odd.at) > static_cast<double>(_pair)) {
            ++nPairsExpired;
            return;
        }
        if (const auto position = detail::globalPosition(state.even, state.odd); position.has_value()) {
            ++nPositions;
            meta.insert_or_assign(property_map::key_type("adsb_latitude"), pmt::Value(position->first));
            meta.insert_or_assign(property_map::key_type("adsb_longitude"), pmt::Value(position->second));
        }
    }

    /*| frame: the airborne velocity message, type code 19. Subtypes 1 and 2 are ground referenced: east and north
            components, written as a ground speed and a true track. Subtypes 3 and 4 are air referenced: an
            airspeed and a magnetic heading. The supersonic subtypes, 2 and 4, count in units of four knots. All
            four carry the same vertical rate, in units of 64 ft/min.
        origin: RTCA DO-260B 2.2.3.2.
        verified-by: qa_ModeSDecode "the ground referenced velocity message gives a speed, a track and a vertical rate"
        verified-by: qa_ModeSDecode "the air referenced velocity message gives an airspeed and a heading, and the supersonic subtype scales both"
    */
    void readAirborneVelocity(std::span<const std::uint8_t> frame, property_map& meta) {
        const std::uint32_t subtype = detail::field(frame, 38UZ, 3UZ);
        meta.insert_or_assign(property_map::key_type("adsb_velocity_subtype"), pmt::Value(gr::Size_t{subtype}));
        const double knot = subtype == 2U || subtype == 4U ? 4.0 : 1.0;

        if (subtype == 1U || subtype == 2U) {
            const std::uint32_t east  = detail::field(frame, 47UZ, 10UZ);
            const std::uint32_t north = detail::field(frame, 58UZ, 10UZ);
            /*| frame: a component of zero is the message's "velocity unavailable".
             */
            if (east != 0U && north != 0U) {
                const double eastward  = (detail::field(frame, 46UZ, 1UZ) != 0U ? -1.0 : 1.0) * (static_cast<double>(east) - 1.0) * knot;
                const double northward = (detail::field(frame, 57UZ, 1UZ) != 0U ? -1.0 : 1.0) * (static_cast<double>(north) - 1.0) * knot;
                meta.insert_or_assign(property_map::key_type("adsb_ground_speed_kt"), pmt::Value(std::hypot(eastward, northward)));
                meta.insert_or_assign(property_map::key_type("adsb_track_deg"), pmt::Value(detail::flooredMod(std::atan2(eastward, northward) * 180.0 / std::numbers::pi, 360.0)));
            }
        } else if (subtype == 3U || subtype == 4U) {
            if (const std::uint32_t airspeed = detail::field(frame, 58UZ, 10UZ); airspeed != 0U) {
                meta.insert_or_assign(property_map::key_type("adsb_airspeed_kt"), pmt::Value((static_cast<double>(airspeed) - 1.0) * knot));
                meta.insert_or_assign(property_map::key_type("adsb_airspeed_type"), pmt::Value(std::string(detail::field(frame, 57UZ, 1UZ) != 0U ? "tas" : "ias")));
            }
            /*| frame: bit 46 is the heading status; clear, the heading field states nothing.
             */
            if (detail::field(frame, 46UZ, 1UZ) != 0U) {
                meta.insert_or_assign(property_map::key_type("adsb_heading_deg"), pmt::Value(static_cast<double>(detail::field(frame, 47UZ, 10UZ)) * 360.0 / 1024.0));
            }
        }

        if (const std::uint32_t rate = detail::field(frame, 70UZ, 9UZ); rate != 0U) {
            const std::int32_t feetPerMinute = (detail::field(frame, 69UZ, 1UZ) != 0U ? -1 : 1) * (static_cast<std::int32_t>(rate) - 1) * 64;
            meta.insert_or_assign(property_map::key_type("adsb_vertical_rate_fpm"), pmt::Value(feetPerMinute));
            meta.insert_or_assign(property_map::key_type("adsb_vertical_rate_source"), pmt::Value(std::string(detail::field(frame, 68UZ, 1UZ) != 0U ? "barometric" : "geometric")));
        }
    }

    /*| contract: the table entry for address, marked as just seen. A new address at the bound first drops the
            address seen least recently.
    */
    [[nodiscard]] Aircraft& touch(std::uint32_t address) {
        const auto found = _aircraft.find(address);
        if (found == _aircraft.end() && _aircraft.size() >= _bound) {
            evictOldest();
        }
        Aircraft& state = _aircraft[address];
        state.touched   = ++_touch;
        return state;
    }

    void evictOldest() {
        const auto oldest = std::ranges::min_element(_aircraft, {}, [](const auto& entry) { return entry.second.touched; });
        if (oldest != _aircraft.end()) {
            _aircraft.erase(oldest);
            ++nEvicted;
        }
    }
};

} // namespace adsb

#endif // include guard
