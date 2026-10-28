#ifndef GR4ADSB_ADSB_BLOCK_LABELS_HPP
#define GR4ADSB_ADSB_BLOCK_LABELS_HPP

/*| role: the labels this package's blocks declare to the core through gr::block::describe() of
        <gnuradio-4.0/BlockAttributes.hpp>: the family `adsb`, and each block's role in its own header.
    trap: a core may carry an earlier form of BlockAttributes.hpp under the same name and include guard.
        The configure compiles a probe against the core it found and defines GR4ADSB_BLOCK_LABELS only where
        the probe compiles. Every label declaration sits under that macro, so the blocks build without
        labels against any other core.
*/

#ifdef GR4ADSB_BLOCK_LABELS
#include <gnuradio-4.0/BlockAttributes.hpp>

namespace adsb {

inline constexpr gr::block::Label kFamilyLabel = gr::block::labels::family("adsb", "1090 MHz Mode S and ADS-B receive: frames, decoded messages and console lines.");

} // namespace adsb
#endif

#endif // GR4ADSB_ADSB_BLOCK_LABELS_HPP
