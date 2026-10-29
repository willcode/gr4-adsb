# The three blocks this package supplies, built as a GNU Radio 4 block library: one shared object that registers them
# when it is loaded, installed under <prefix>/lib64/gnuradio-4/plugins beside whatever else is there. They are in the
# namespace `adsb`, and the registry keys follow from it: adsb::PpmFramer<complex<float32>>, adsb::PpmFramer<float32>,
# adsb::ModeSDecode and adsb::AdsbPrinter.
#
# PpmFramer runs on gr::digital::PpmScanner, the pulse-position kernel of the framework's algorithm layer, and
# gnuradio-algorithm is on the link line for it. RecordFields.hpp registers nothing and is read by the two message
# blocks; PpmConsume.hpp registers nothing either and gives the scanner's consume() one return type across the library's
# two forms.

gr_add_block_library(
  AdsbDemoBlocks
  MAKE_SHARED_LIBRARY
  HEADERS
  include/adsb/PpmFramer.hpp
  include/adsb/ModeS.hpp
  include/adsb/AdsbPrinter.hpp
  LINK_LIBRARIES
  gnuradio4::gnuradio-algorithm)

# The same headers used directly rather than through the registry, for a chain that names its block types and is built
# with emplaceBlock<T>.
add_library(gr4adsb-blocks INTERFACE)
target_include_directories(gr4adsb-blocks INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(gr4adsb-blocks INTERFACE gnuradio4::gnuradio-core gnuradio4::gnuradio-algorithm)
if(GR4ADSB_BLOCK_LABELS)
  target_compile_definitions(gr4adsb-blocks INTERFACE GR4ADSB_BLOCK_LABELS)
endif()

# The warning set goes on the object library alone, where it applies to the generated translation units that compile the
# headers and stays out of anything the shared library exports. The include directory and the label definition go with
# it through gr4adsb-blocks: the generated units reach a registering header by its absolute path, and nothing else puts
# this module's own include directory on the search path, so one header of this module could not include another.
if(TARGET AdsbDemoBlocksObject)
  target_link_libraries(AdsbDemoBlocksObject PRIVATE gr4adsb-options gr4adsb-blocks)
endif()

if(BUILD_TESTING)
  if(TARGET gr4adsb-ut)
    add_subdirectory(test)
  endif()
  add_subdirectory(benchmarks)
endif()
