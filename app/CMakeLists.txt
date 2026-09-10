add_executable(adsb adsb.cpp)

# The application instantiates every block by type, so it needs the include paths rather than the registry. It links
# the module's static block library anyway, because that is what makes the blocks resolvable by name too — see
# `gr::blocklib::initGr4AdsbBlocks` in the generated <gnuradio-4.0/Gr4AdsbBlocks.hpp>.
target_link_libraries(
  adsb
  PRIVATE gr4adsb-digital
          gr4adsb-adsb
          Gr4AdsbBlocksStatic
          gnuradio4::gr-basic
          gnuradio4::gr-fileio
          gnuradio-options)

# The SDR family is optional in the SDK, and when it is there it reaches a consumer in one of two shapes. An SDK that
# exports gnuradio4::gr-sdr carries the sdr include path, the algorithm headers SoapySource.hpp pulls in and SoapySDR
# itself on that one target, so linking it is the whole live path. An SDK that installs the sdr headers but exports no
# target for them leaves the header and SoapySDR to be found by hand; with neither, the application offers --file
# replay alone. The configure line says which of the three this build got.
if(TARGET gnuradio4::gr-sdr)
  message(STATUS "gr4-adsb: the SDK exports gnuradio4::gr-sdr, building the live receive path")
  target_compile_definitions(adsb PRIVATE GR4ADSB_HAVE_SOAPY)
  target_link_libraries(adsb PRIVATE gnuradio4::gr-sdr)
else()
  find_package(SoapySDR CONFIG QUIET)
  find_path(GR4ADSB_SDR_INCLUDE_DIR gnuradio-4.0/sdr/SoapySource.hpp)

  if(SoapySDR_FOUND AND GR4ADSB_SDR_INCLUDE_DIR)
    message(STATUS "gr4-adsb: the SDK exports no sdr target; SoapySDR and the sdr headers were found, building the live receive path by hand")
    target_compile_definitions(adsb PRIVATE GR4ADSB_HAVE_SOAPY)
    target_include_directories(adsb PRIVATE "${GR4ADSB_SDR_INCLUDE_DIR}")
    target_link_libraries(adsb PRIVATE SoapySDR gnuradio4::gnuradio-algorithm)

    # SoapySource.hpp includes <gnuradio-4.0/sdr/NamespaceCompatibility.hpp>, and an SDK old enough not to install
    # that header cannot compile it. When the prefix does not have the header, supply exactly that one file from a
    # build-tree directory holding nothing else, so every other gnuradio-4.0 header still comes from the prefix.
    # The test is on the file, so a prefix that has it keeps its own.
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
      target_include_directories(adsb PRIVATE "${shim}")
    endif()
  else()
    message(STATUS "gr4-adsb: SoapySDR or the installed sdr headers were not found, building --file replay only")
  endif()
endif()

install(TARGETS adsb RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
