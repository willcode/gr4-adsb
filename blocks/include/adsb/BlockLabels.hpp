#ifndef GR4ADSB_ADSB_BLOCK_LABELS_HPP
#define GR4ADSB_ADSB_BLOCK_LABELS_HPP

// The labels the blocks of this package declare to the core: the family `adsb` and each block's role. A block
// declares them through gr::block::describe() of <gnuradio-4.0/BlockAttributes.hpp>. A core may carry an earlier
// form of that header under the same name and include guard, so the configure compiles a probe against the core it
// found and defines GR4ADSB_BLOCK_LABELS where the probe compiles. Every declaration sits under that macro, and the
// blocks build without labels against any other core.

#ifdef GR4ADSB_BLOCK_LABELS
#include <gnuradio-4.0/BlockAttributes.hpp>

namespace adsb {

//! The family label of every block in this package.
inline constexpr gr::block::Label kFamilyLabel = gr::block::labels::family("adsb", "1090 MHz Mode S and ADS-B receive: frames, decoded messages and console lines.");

} // namespace adsb
#endif

#endif // GR4ADSB_ADSB_BLOCK_LABELS_HPP
