# PpmFramer and the two kernels it is built on. Header only: the target carries the include path and the SDK
# libraries the headers need.
add_library(gr4adsb-digital INTERFACE)
target_link_libraries(gr4adsb-digital INTERFACE gnuradio4::gnuradio-core gnuradio4::gnuradio-algorithm)
target_include_directories(gr4adsb-digital INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                                     $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/gr4adsb" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
