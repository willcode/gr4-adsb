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

option(GR4ADSB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

# gnuradio4         core: Block, Graph, Scheduler, DataSet, the block registry and the plugin loader
# gnuradio4Library  the algorithm headers the block families pull in
# gnuradio4Blocks   the families the chain is built from: digital, adsb, basic, fileio, and sdr for a live device
find_package(gnuradio4 CONFIG REQUIRED)
find_package(gnuradio4Library CONFIG REQUIRED)
find_package(gnuradio4Blocks CONFIG REQUIRED)

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

# app/ holds the whole package: the application with the ConsoleSink it ends its chain with, and the graph runner the
# two halves of the split receiver are run by. Every other block in the chain is GNU Radio 4's own and is used as
# installed.
add_subdirectory(app)
