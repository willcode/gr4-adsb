# PpmFramer, the block over the framing scanner. The scanner and the interpolation it uses are algorithm headers and
# live in lib/, which is what gr4adsb-algorithm carries. Header only: the target carries the include path and the
# libraries the headers need.
add_library(gr4adsb-digital INTERFACE)
target_link_libraries(gr4adsb-digital INTERFACE gr4adsb-algorithm gnuradio4::gnuradio-core
                                                gnuradio4::gnuradio-algorithm)
target_include_directories(gr4adsb-digital INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                                     $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/gr4adsb" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
