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

# Where --verbose loads from when --plugin-dir is not given. The SDK writes its block libraries under its own library
# directory, which is lib64 on a distribution that puts libraries there, so the path is derived from where the
# package configuration was found rather than from this build's own GNUInstallDirs.
get_filename_component(GR4ADSB_SDK_LIBDIR "${gnuradio4Blocks_DIR}/../.." ABSOLUTE)
target_compile_definitions(adsb PRIVATE "GR4ADSB_SDK_PLUGIN_DIR=\"${GR4ADSB_SDK_LIBDIR}/gnuradio-4/plugins\"")

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
