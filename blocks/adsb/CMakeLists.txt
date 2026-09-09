# ModeSDecode and AdsbPrinter, the message layer above the framer. Header only, like the framer's family; the two
# do not include each other, they are joined by the record type that passes between them.
add_library(gr4adsb-adsb INTERFACE)
target_link_libraries(gr4adsb-adsb INTERFACE gnuradio4::gnuradio-core gnuradio4::gnuradio-algorithm)
target_include_directories(gr4adsb-adsb INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                                  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/gr4adsb" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
