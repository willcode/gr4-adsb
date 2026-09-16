add_executable(adsb adsb.cpp)

# The application names every block type and builds its chain with emplaceBlock<T>, so what it takes is include
# paths: gr4adsb-blocks for this package's three ADS-B blocks, the SDK's families for the rest. --verbose opens
# shared block libraries through gr::PluginLoader, which uses the process-wide block and scheduler registries; those
# are defined in gnuradio-blocklib-core.
target_link_libraries(
  adsb
  PRIVATE gr4adsb-blocks
          gnuradio4::gr-basic
          gnuradio4::gr-fileio
          gnuradio4::gnuradio-blocklib-core
          gr4adsb-options)

# The two directories `adsb --verbose` loads block libraries from when --plugin-dir is not given.
#
# The first is this package's own, where the install puts the ADS-B block library. It is this build's install
# directory rather than the build tree's, which is the shape the framework's own tools use, so that an installed
# program reads the installation it belongs to; a run from the build tree names that tree with --plugin-dir.
#
# The second is the SDK's, which holds every other family. The SDK writes its libraries under its library directory,
# which is lib64 on a distribution that puts libraries there, so the default is derived from where the package
# configuration was found rather than from this build's own GNUInstallDirs. Where the SDK was found is not where it
# will be read from for a build that installs somewhere other than it runs: a cross build finds a staging tree and
# the program runs on a target that has the SDK at a prefix of its own, so GR4ADSB_SDK_PLUGIN_DIR names that
# directory instead. The first needs no such option, because it follows CMAKE_INSTALL_PREFIX.
set(GR4ADSB_PLUGIN_DIR "${CMAKE_INSTALL_FULL_LIBDIR}/gnuradio-4/plugins")
get_filename_component(GR4ADSB_SDK_LIBDIR "${gnuradio4Blocks_DIR}/../.." ABSOLUTE)
set(GR4ADSB_SDK_PLUGIN_DIR
    "${GR4ADSB_SDK_LIBDIR}/gnuradio-4/plugins"
    CACHE PATH "The SDK's plugin directory `adsb --verbose` loads from when --plugin-dir is not given")
target_compile_definitions(
  adsb PRIVATE "GR4ADSB_PLUGIN_DIR=\"${GR4ADSB_PLUGIN_DIR}\"" "GR4ADSB_SDK_PLUGIN_DIR=\"${GR4ADSB_SDK_PLUGIN_DIR}\"")
message(STATUS "gr4-adsb: --verbose loads from ${GR4ADSB_PLUGIN_DIR} and ${GR4ADSB_SDK_PLUGIN_DIR}")

# The SDR family is optional in the SDK. gnuradio4::gr-sdr is exported only when that family was built, and it carries
# the sdr include path, the algorithm headers SoapySource.hpp pulls in and SoapySDR itself, so linking it is the whole
# live path. Without it the application offers --file replay alone. The configure line says which of the two this
# build got.
if(TARGET gnuradio4::gr-sdr)
  message(STATUS "gr4-adsb: the SDK exports gnuradio4::gr-sdr, building the live receive path")
  target_compile_definitions(adsb PRIVATE GR4ADSB_HAVE_SOAPY)
  target_link_libraries(adsb PRIVATE gnuradio4::gr-sdr)
else()
  message(STATUS "gr4-adsb: the SDK exports no gnuradio4::gr-sdr, building --file replay only")
endif()

install(TARGETS adsb RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
