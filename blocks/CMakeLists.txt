add_subdirectory(digital)
add_subdirectory(adsb)

# One block library for the whole module. The SDK's registration generator reads the headers named here, writes a
# translation unit per registered block plus the registration units that enter them into a BlockRegistry, and emits
# <gnuradio-4.0/Gr4AdsbBlocks.hpp> with the gr::blocklib::initGr4AdsbBlocks() initializer the application calls.
#
# MAKE_SHARED_LIBRARY produces the loadable plugin, installed under lib/gnuradio-4/plugins; MAKE_STATIC_LIBRARY
# produces the archive the application links. Only headers that carry GR_REGISTER_BLOCK belong in HEADERS — the
# kernel headers under digital/, sync/, filter/ and fourier/ define no block and are pulled in by PpmFramer.hpp.
gr_add_block_library(
  Gr4AdsbBlocks
  MAKE_SHARED_LIBRARY
  MAKE_STATIC_LIBRARY
  HEADERS
  ${CMAKE_CURRENT_SOURCE_DIR}/digital/include/gr4adsb/digital/PpmFramer.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/adsb/include/gr4adsb/adsb/ModeS.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/adsb/include/gr4adsb/adsb/AdsbPrinter.hpp
  LINK_LIBRARIES
  gr4adsb-digital
  gr4adsb-adsb)

if(GR4ADSB_ENABLE_TESTING)
  add_subdirectory(test)
endif()
