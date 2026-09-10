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

option(GR4ADSB_ENABLE_TESTING "Build the block tests" ON)
option(GR4ADSB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

# gnuradio4      core: Block, Graph, Scheduler, DataSet, the block registry
# gnuradio4Library    the algorithm headers the SDR and file blocks pull in
# gnuradio4Blocks     the GNU Radio 4 block families the application's chain uses
# GnuRadioBlockLib    gr_add_block_library, the registration generator this module registers through
find_package(gnuradio4 CONFIG REQUIRED)
find_package(gnuradio4Library CONFIG REQUIRED)
find_package(gnuradio4Blocks CONFIG REQUIRED)
find_package(GnuRadioBlockLib CONFIG REQUIRED)

# The installed gnuradio4::gnuradio-options keeps its warning flags behind $<BUILD_INTERFACE:...>, so they do not
# reach a consuming project. gr_add_block_library links a target literally named `gnuradio-options` into the object
# library it creates when one exists, so that is the name this project's own flags go on.
add_library(gnuradio-options INTERFACE)
target_compile_options(
  gnuradio-options
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
            # GCC 16 at -O3 reports `std::vector`'s own destructor, inlined out of a block's, as reading an
            # uninitialized `_M_start` — of a member that has a default member initializer and is always
            # constructed. It fires here on `PpmScanner::config.pulseSlots` once the block is large enough for the
            # inliner to reach that far, on both instantiations of PpmFramer. GNU Radio 4 carries the same
            # suppression in its own cmake/CompilerWarnings.cmake, for the same reason: it is not a warning about
            # code that can be written differently.
            $<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized>
            $<$<BOOL:${GR4ADSB_WARNINGS_AS_ERRORS}>:-Werror>)

if(GR4ADSB_ENABLE_TESTING)
  include(CTest)
  enable_testing()
endif()

add_subdirectory(blocks)
add_subdirectory(app)
