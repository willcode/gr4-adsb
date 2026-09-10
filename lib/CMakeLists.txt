# The algorithm headers: code that is not a block and would belong in gnuradio4-library rather than in a block
# family. The layout under include/gr4adsb/algorithm mirrors the library's own, so each header's place there is its
# place here: digital/ holds the PPM framing scanner and the parameterized CRC, sync/ the interpolator the framer's
# phase search reads through, and filter/ and fourier/ the filter design and window that build its tap bank.
#
# Header only: the target carries the include path and the SDK libraries the headers need.
add_library(gr4adsb-algorithm INTERFACE)
target_link_libraries(gr4adsb-algorithm INTERFACE gnuradio4::gnuradio-core gnuradio4::gnuradio-algorithm)
target_include_directories(gr4adsb-algorithm INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                                       $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/gr4adsb" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
