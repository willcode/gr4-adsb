add_executable(adsb_print adsb_print.cpp)

# The application instantiates every block by type, so it needs the include paths rather than the registry. It links
# the module's static block library anyway, because that is what makes the blocks resolvable by name too — see
# `gr::blocklib::initGr4AdsbBlocks` in the generated <gnuradio-4.0/Gr4AdsbBlocks.hpp>.
target_link_libraries(
  adsb_print
  PRIVATE gr4adsb-digital
          gr4adsb-adsb
          Gr4AdsbBlocksStatic
          gnuradio4::gr-basic
          gnuradio4::gr-fileio
          gnuradio-options)

# The SDR family is optional in the SDK and, unlike every other family, exports no CMake target: it installs its
# headers and nothing else. So the SoapySource path is compiled in only when both the header and SoapySDR are found,
# and the application falls back to --file when they are not.
find_package(SoapySDR CONFIG QUIET)
find_path(GR4ADSB_SDR_INCLUDE_DIR gnuradio-4.0/sdr/SoapySource.hpp)

if(SoapySDR_FOUND AND GR4ADSB_SDR_INCLUDE_DIR)
  message(STATUS "gr4-adsb: SoapySDR found, building the live receive path")
  target_compile_definitions(adsb_print PRIVATE GR4ADSB_HAVE_SOAPY)
  target_include_directories(adsb_print PRIVATE "${GR4ADSB_SDR_INCLUDE_DIR}")
  target_link_libraries(adsb_print PRIVATE SoapySDR gnuradio4::gnuradio-algorithm)

  # SoapySource.hpp includes <gnuradio-4.0/sdr/NamespaceCompatibility.hpp>, which the SDR family does not install
  # (fixed in gnuradio4-blocks #26). Until that reaches the SDK, supply exactly that one header from a build-tree
  # directory holding nothing else, so every other gnuradio-4.0 header still comes from the prefix.
  if(NOT EXISTS "${GR4ADSB_SDR_INCLUDE_DIR}/gnuradio-4.0/sdr/NamespaceCompatibility.hpp")
    set(shim "${CMAKE_CURRENT_BINARY_DIR}/sdr-shim")
    file(
      WRITE "${shim}/gnuradio-4.0/sdr/NamespaceCompatibility.hpp"
      "#ifndef GR4_SDR_NAMESPACE_COMPATIBILITY_HPP\n"
      "#define GR4_SDR_NAMESPACE_COMPATIBILITY_HPP\n"
      "namespace gr::blocks::sdr {}\n"
      "namespace gr {\n"
      "namespace sdr = blocks::sdr;\n"
      "}\n"
      "#endif\n")
    message(STATUS "gr4-adsb: the SDK does not install sdr/NamespaceCompatibility.hpp; supplying it from ${shim}")
    target_include_directories(adsb_print PRIVATE "${shim}")
  endif()
else()
  message(STATUS "gr4-adsb: SoapySDR or the installed sdr headers were not found, building --file replay only")
endif()

install(TARGETS adsb_print RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
