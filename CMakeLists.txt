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

# The directory holding the recorded capture the two recording tests read. An empty or absent directory is not an error:
# each test prints the path it wanted and exits 77, which CTest registers as a skip.
set(GR4_RECORDINGS_DIR
    ""
    CACHE PATH "Directory holding the recorded capture the recording tests read; empty makes those tests skip")

# * gnuradio4: core, with Block, Graph, Scheduler, DataSet, the block registry and the plugin loader
# * gnuradio4Library: the algorithm layer, the pulse-position scanner this package's framer runs on among it
# * gnuradio4Blocks: the families the rest of the chain is built from: basic, fileio, and sdr for a live device
# * GnuRadioBlockLib: gr_add_block_library, which builds blocks/ into this package's plugin
find_package(gnuradio4 CONFIG REQUIRED)
find_package(gnuradio4Library CONFIG REQUIRED)
find_package(gnuradio4Blocks CONFIG REQUIRED)
find_package(GnuRadioBlockLib CONFIG REQUIRED)

# gnuradio4::gnuradio-options keeps its warning flags behind $<BUILD_INTERFACE:...>, so they do not reach a consuming
# project; this is the set this package compiles with instead.
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
            # `PpmScanner::config.pulseSlots`, a member that has a default member initializer and is always constructed.
            # GNU Radio 4 carries the same suppression in its own cmake/CompilerWarnings.cmake.
            $<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized>
            $<$<BOOL:${GR4ADSB_WARNINGS_AS_ERRORS}>:-Werror>)

# A block describes itself to the core with labels, declared through gr::block::describe() of
# <gnuradio-4.0/BlockAttributes.hpp>. A core may carry an earlier form of the header under the same name and include
# guard, and __has_include cannot tell that form from the label form. The configure therefore compiles a probe against
# the core it found, anew at each run, and defines GR4ADSB_BLOCK_LABELS where the probe compiles. The three block
# headers declare their labels under that macro alone, so they build without labels against any other core.
include(CheckCXXSourceCompiles)
include(CMakePushCheckState)
cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_LIBRARIES gnuradio4::gnuradio-core)
set(CMAKE_REQUIRED_QUIET ON)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
unset(GR4ADSB_CORE_HAS_BLOCK_LABELS CACHE)
check_cxx_source_compiles(
  [[
#include <gnuradio-4.0/BlockAttributes.hpp>
struct Probe {
    static constexpr auto attributes = gr::block::describe(1U, gr::block::labels::role::processor);
};
static_assert(gr::block::attributesOf<Probe>().labels.size() == 1U);
int main() { return 0; }
]]
  GR4ADSB_CORE_HAS_BLOCK_LABELS)
cmake_pop_check_state()
set(GR4ADSB_BLOCK_LABELS ${GR4ADSB_CORE_HAS_BLOCK_LABELS})
if(GR4ADSB_BLOCK_LABELS)
  message(STATUS "gr4-adsb: block labels DECLARED (the core has gr::block::describe)")
else()
  message(STATUS "gr4-adsb: block labels NOT declared (the core has no gr::block::describe)")
endif()

# The block tests are written in Boost.UT, which GNU Radio 4 does not install: an installed `ut` package, or
# -DGR_BOOST_UT_INCLUDE_DIR=<dir> naming a source-built copy. Without it the tests are not built.
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
    # boost.ut ships as a module as well as a header; these tests include the header.
    target_compile_definitions(gr4adsb-ut INTERFACE BOOST_UT_DISABLE_MODULE)
  else()
    message(STATUS "gr4-adsb: Boost.UT was not found, so the block tests are not built")
  endif()
endif()

# blocks/ holds the framer, the Mode S message layer and the printer, in the namespace `adsb`, as one block library a
# graph runner loads from a plugin directory. app/ holds the one-process application and the ConsoleSink it ends its
# chain with. Everything else in either chain is the SDK's and is used as installed, rungraph among it.
add_subdirectory(blocks)
add_subdirectory(app)

# The graph files and the two Python programs are data: nothing in the build compiles or reads them.
install(DIRECTORY graphs/ DESTINATION "${CMAKE_INSTALL_DATADIR}/gr4-adsb/graphs")
install(PROGRAMS tools/adsb_listen.py tools/adsb_control.py DESTINATION "${CMAKE_INSTALL_DATADIR}/gr4-adsb/tools")
