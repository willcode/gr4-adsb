# The three blocks this package supplies, built as a GNU Radio 4 block library: one shared object that registers
# them when it is loaded, installed under <prefix>/lib64/gnuradio-4/plugins beside whatever else is there. They are
# in the namespace `adsb`, and the registry keys follow from it: adsb::PpmFramer<complex<float32>>,
# adsb::PpmFramer<float32>, adsb::ModeSDecode and adsb::AdsbPrinter.
#
# PpmFramer runs on gr::digital::PpmScanner, the pulse-position kernel of the framework's algorithm layer, which is
# what gnuradio-algorithm is for here. RecordFields.hpp registers nothing and is read by the two message blocks.

gr_add_block_library(
  AdsbDemoBlocks
  MAKE_SHARED_LIBRARY
  HEADERS
  include/adsb/PpmFramer.hpp
  include/adsb/ModeS.hpp
  include/adsb/AdsbPrinter.hpp
  LINK_LIBRARIES
  gnuradio4::gnuradio-algorithm)

# The same headers used directly rather than through the registry, for a chain that names its block types and is
# built with emplaceBlock<T>.
add_library(gr4adsb-blocks INTERFACE)
target_include_directories(gr4adsb-blocks INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(gr4adsb-blocks INTERFACE gnuradio4::gnuradio-core gnuradio4::gnuradio-algorithm)

# The warning set goes on the object library alone, where it applies to the generated translation units that compile
# the headers and stays out of anything the shared library exports. The include directory goes with it: the
# generated units reach a registering header by its absolute path, so nothing else puts this module's own include
# directory on the search path, and a header of this module that includes another of it would not be found.
if(TARGET AdsbDemoBlocksObject)
  target_link_libraries(AdsbDemoBlocksObject PRIVATE gr4adsb-options)
  target_include_directories(AdsbDemoBlocksObject PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
endif()

if(BUILD_TESTING)
  if(TARGET gr4adsb-ut)
    add_subdirectory(test)
  endif()
  add_subdirectory(benchmarks)
endif()
