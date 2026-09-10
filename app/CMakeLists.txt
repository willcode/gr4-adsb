add_executable(adsb adsb.cpp)

# The application names every block type and builds its chain with emplaceBlock<T>, so it needs the block families'
# include paths and the SDK, not the module's block library. --verbose opens the shared block library through
# gr::PluginLoader, which uses the process-wide block and scheduler registries; those are defined in
# gnuradio-blocklib-core.
target_link_libraries(
  adsb
  PRIVATE gr4adsb-digital
          gr4adsb-adsb
          gnuradio4::gnuradio-blocklib-core
          gnuradio4::gr-basic
          gnuradio4::gr-fileio
          gnuradio-options)

# The two directories --verbose can read when --plugin-dir is not given: the build tree's, where the shared block
# library is written, and the install prefix's. Both are compiled in, and the application selects between them by its
# own directory at run time. When the SDK builds no shared block library the first is empty and only the installed
# path remains.
if(TARGET Gr4AdsbBlocksShared)
  set(GR4ADSB_BUILD_PLUGIN_DIR "$<TARGET_FILE_DIR:Gr4AdsbBlocksShared>")
else()
  set(GR4ADSB_BUILD_PLUGIN_DIR "")
endif()
target_compile_definitions(
  adsb
  PRIVATE "GR4ADSB_BUILD_APP_DIR=\"$<TARGET_FILE_DIR:adsb>\""
          "GR4ADSB_BUILD_PLUGIN_DIR=\"${GR4ADSB_BUILD_PLUGIN_DIR}\""
          "GR4ADSB_INSTALL_PLUGIN_DIR=\"${CMAKE_INSTALL_FULL_LIBDIR}/gnuradio-4/plugins\"")

# The SDR family is optional in the SDK and reaches a consumer in one of two shapes. An SDK that exports
# gnuradio4::gr-sdr carries the sdr include path, the algorithm headers SoapySource.hpp pulls in and SoapySDR itself on
# that one target, so linking it is the whole live path. An SDK that installs the sdr headers but exports no target for
# them leaves the header and SoapySDR to be found by hand. With neither, the application offers --file replay alone.
# The configure line says which of the three this build got.
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
