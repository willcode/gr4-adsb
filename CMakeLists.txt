cmake_minimum_required(VERSION 3.28)

project(
  gr4-adsb
  VERSION 0.1.0
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include(GNUInstallDirs)
include(CTest)

option(GR4ADSB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

# gnuradio4         core: Block, Graph, Scheduler, DataSet, the block registry and the plugin loader
# gnuradio4Library  the algorithm layer, the pulse-position scanner this package's framer runs on among it
# gnuradio4Blocks   the families the rest of the chain is built from: basic, fileio, and sdr for a live device
# GnuRadioBlockLib  gr_add_block_library, which builds blocks/ into this package's plugin
find_package(gnuradio4 CONFIG REQUIRED)
find_package(gnuradio4Library CONFIG REQUIRED)
find_package(gnuradio4Blocks CONFIG REQUIRED)
find_package(GnuRadioBlockLib CONFIG REQUIRED)

# The installed gnuradio4::gnuradio-options keeps its warning flags behind $<BUILD_INTERFACE:...>, so they do not
# reach a consuming project; this is the set the application is compiled with instead.
add_library(gr4adsb-options INTERFACE)
target_compile_options(
  gr4adsb-options
  INTERFACE -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
            -Wsign-conversion
            -Wold-style-cast
            -Wcast-align
            -Wnon-virtual-dtor
            -Woverloaded-virtual
            -Wdouble-promotion
            -Wformat=2
            -Wno-unknown-pragmas
            # the SDK's own headers trip these, so they are off here rather than in the code that includes them
            $<$<CXX_COMPILER_ID:GNU>:-Wno-dangling-reference>
            $<$<CXX_COMPILER_ID:GNU>:-Wno-interference-size>
            $<$<CXX_COMPILER_ID:GNU>:-Wno-missing-field-initializers>
            # GCC 16 at -O3 reports `std::vector`'s own destructor as reading an uninitialized `_M_start`, on
            # `PpmScanner::config.pulseSlots`, a member that has a default member initializer and is always
            # constructed. GNU Radio 4 carries the same suppression in its own cmake/CompilerWarnings.cmake, for the
            # same reason: the warning is not about code that can be written differently.
            $<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized>
            $<$<BOOL:${GR4ADSB_WARNINGS_AS_ERRORS}>:-Werror>)

# Boost.UT is what the block tests are written in. GNU Radio 4 installs none of it, so it is looked for the way the
# SDK's own build looks for it, and -DGR_BOOST_UT_INCLUDE_DIR=<dir> names a source-built copy. Without it the tests
# are not built and nothing else in the package changes.
if(BUILD_TESTING)
  find_package(ut CONFIG QUIET)
  set(GR4ADSB_UT_LIBRARY "")
  if(TARGET Boost::ut)
    set(GR4ADSB_UT_LIBRARY Boost::ut)
  elseif(TARGET boost::ut)
    set(GR4ADSB_UT_LIBRARY boost::ut)
  else()
    find_path(GR_BOOST_UT_INCLUDE_DIR boost/ut.hpp)
  endif()
  if(GR4ADSB_UT_LIBRARY OR GR_BOOST_UT_INCLUDE_DIR)
    add_library(gr4adsb-ut INTERFACE)
    if(GR4ADSB_UT_LIBRARY)
      target_link_libraries(gr4adsb-ut INTERFACE ${GR4ADSB_UT_LIBRARY})
    else()
      target_include_directories(gr4adsb-ut SYSTEM INTERFACE "${GR_BOOST_UT_INCLUDE_DIR}")
    endif()
    # boost.ut offers itself as a module as well as a header, and the header is what these tests include.
    target_compile_definitions(gr4adsb-ut INTERFACE BOOST_UT_DISABLE_MODULE)
  else()
    message(STATUS "gr4-adsb: Boost.UT was not found, so the block tests are not built")
  endif()
endif()

# blocks/ holds the framer, the Mode S message layer and the printer, in the namespace `adsb`: GNU Radio 4 carries
# no ADS-B family, so this package builds them itself, as one block library a graph runner loads from a plugin
# directory. app/ holds the one-process application and the ConsoleSink it ends its chain with. Everything else in
# either chain is the SDK's and is used as installed, and so is rungraph, which runs the graph files under graphs/.
add_subdirectory(blocks)
add_subdirectory(app)

# The graph files and the two Python programs are text, and the README's commands name them by path, so an install
# has to put them somewhere. They are data rather than programs of the build: nothing compiles them and nothing in
# the package reads them.
install(DIRECTORY graphs/ DESTINATION "${CMAKE_INSTALL_DATADIR}/gr4-adsb/graphs")
install(PROGRAMS tools/adsb_listen.py tools/adsb_control.py DESTINATION "${CMAKE_INSTALL_DATADIR}/gr4-adsb/tools")
