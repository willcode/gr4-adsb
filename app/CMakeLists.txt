add_executable(adsb adsb.cpp)

# The application names every block type and builds its chain with emplaceBlock<T>, so what it takes from the SDK is
# the block families' include paths. --verbose opens the SDK's shared block libraries through gr::PluginLoader, which
# uses the process-wide block and scheduler registries; those are defined in gnuradio-blocklib-core.
target_link_libraries(
  adsb
  PRIVATE gnuradio4::gr-adsb
          gnuradio4::gr-basic
          gnuradio4::gr-fileio
          gnuradio4::gnuradio-blocklib-core
          gr4adsb-options)

# Where `adsb --verbose` and `adsb_run` load block libraries from when --plugin-dir is not given. The SDK writes its
# own under its library directory, which is lib64 on a distribution that puts libraries there, so the path is derived
# from where the package configuration was found rather than from this build's own GNUInstallDirs.
get_filename_component(GR4ADSB_SDK_LIBDIR "${gnuradio4Blocks_DIR}/../.." ABSOLUTE)
set(GR4ADSB_SDK_PLUGIN_DIR "${GR4ADSB_SDK_LIBDIR}/gnuradio-4/plugins")
target_compile_definitions(adsb PRIVATE "GR4ADSB_SDK_PLUGIN_DIR=\"${GR4ADSB_SDK_PLUGIN_DIR}\"")

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

# The graph runner. It names no block, no setting and no chain, so it links the core, the two registries and the
# plugin loader and no block family at all: what a graph file names is supplied by the plugin directories when the
# file is loaded. That is what lets one program run either half of the split receiver, and any other graph file.
add_executable(adsb_run adsb_run.cpp)
target_link_libraries(adsb_run PRIVATE gnuradio4::gnuradio-core gnuradio4::gnuradio-blocklib-core gr4adsb-options)
target_compile_definitions(adsb_run PRIVATE "GR4ADSB_SDK_PLUGIN_DIR=\"${GR4ADSB_SDK_PLUGIN_DIR}\"")

install(TARGETS adsb adsb_run RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
