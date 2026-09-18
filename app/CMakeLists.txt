add_executable(adsb adsb.cpp)

# The application names every block type and builds its chain with emplaceBlock<T>, so what it takes is include
# paths: gr4adsb-blocks for this package's three ADS-B blocks, the SDK's families for the rest. --verbose opens
# shared block libraries through gr::PluginLoader, whose process-wide registries are defined in
# gnuradio-blocklib-core.
target_link_libraries(
  adsb
  PRIVATE gr4adsb-blocks
          gnuradio4::gr-basic
          gnuradio4::gr-fileio
          gnuradio4::gnuradio-blocklib-core
          gr4adsb-options)

# The two directories `adsb --verbose` loads block libraries from when --plugin-dir is not given.
#
# The first is this package's own, where the install puts the ADS-B block library. It follows
# CMAKE_INSTALL_PREFIX, so an installed program reads the installation it belongs to; a run from the build tree
# names that tree with --plugin-dir.
#
# The second is the SDK's, which holds every other family, derived from where its package configuration was found
# (lib64 on a distribution that puts libraries there). That is not where the program will read it from for a build
# that installs somewhere other than it runs, such as a cross build against a staging tree, so
# GR4ADSB_SDK_PLUGIN_DIR names that directory instead.
set(GR4ADSB_PLUGIN_DIR "${CMAKE_INSTALL_FULL_LIBDIR}/gnuradio-4/plugins")
get_filename_component(GR4ADSB_SDK_LIBDIR "${gnuradio4Blocks_DIR}/../.." ABSOLUTE)
set(GR4ADSB_SDK_PLUGIN_DIR
    "${GR4ADSB_SDK_LIBDIR}/gnuradio-4/plugins"
    CACHE PATH "The SDK's plugin directory `adsb --verbose` loads from when --plugin-dir is not given")
target_compile_definitions(
  adsb PRIVATE "GR4ADSB_PLUGIN_DIR=\"${GR4ADSB_PLUGIN_DIR}\"" "GR4ADSB_SDK_PLUGIN_DIR=\"${GR4ADSB_SDK_PLUGIN_DIR}\"")
message(STATUS "gr4-adsb: --verbose loads from ${GR4ADSB_PLUGIN_DIR} and ${GR4ADSB_SDK_PLUGIN_DIR}")

# The SDR family is optional in the SDK: gnuradio4::gr-sdr is exported only where it was built. It carries the sdr
# include path, the algorithm headers SoapySource.hpp pulls in and SoapySDR itself, so linking it is the whole live
# path; without it the application offers --file replay alone.
if(TARGET gnuradio4::gr-sdr)
  message(STATUS "gr4-adsb: the SDK exports gnuradio4::gr-sdr, building the live receive path")
  target_compile_definitions(adsb PRIVATE GR4ADSB_HAVE_SOAPY)
  target_link_libraries(adsb PRIVATE gnuradio4::gr-sdr)
else()
  message(STATUS "gr4-adsb: the SDK exports no gnuradio4::gr-sdr, building --file replay only")
endif()

install(TARGETS adsb RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
