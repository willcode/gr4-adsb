#ifndef GR4ADSB_ADSB_PPM_CONSUME_HPP
#define GR4ADSB_ADSB_PPM_CONSUME_HPP

#include <cstddef>
#include <expected>
#include <stdexcept>
#include <utility>

#include <gnuradio-4.0/Message.hpp>

/*| role: one spelling of a gr::digital::PpmScanner::consume() call, for the framer, the tests and the benchmark.
    trap: the algorithm library ships consume() in two forms. One returns std::expected<std::size_t, gr::Error> and
        reports a framing it refuses as the error; the other returns the bare count and throws std::invalid_argument
        for the same framing. A direct call compiles against one form only.
*/

namespace adsb::detail {

/*| contract: runs consume, a callable making one consume() call, and returns the count it reported, or a gr::Error
        naming the refused framing whichever way the library reported it. A std::invalid_argument thrown by the sink
        consume() calls comes back the same way, so a sink passed through here throws no std::invalid_argument.
*/
template<typename Consume>
[[nodiscard]] std::expected<std::size_t, gr::Error> consumed(Consume&& consume) {
    try {
        return std::forward<Consume>(consume)();
    } catch (const std::invalid_argument& refused) {
        return std::unexpected(gr::Error(refused));
    }
}

} // namespace adsb::detail

#endif // GR4ADSB_ADSB_PPM_CONSUME_HPP
