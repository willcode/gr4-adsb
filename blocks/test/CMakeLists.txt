# Boost.UT is the framework the GNU Radio 4 suites are written in. It is not part of the SDK, so it is looked for as
# a CMake package first and as a header on the include path second.
if(NOT TARGET ut)
  find_package(ut CONFIG QUIET)
  if(TARGET Boost::ut)
    add_library(ut ALIAS Boost::ut)
  elseif(TARGET boost::ut)
    add_library(ut ALIAS boost::ut)
  else()
    find_path(GR4ADSB_BOOST_UT_INCLUDE_DIR boost/ut.hpp)
    if(NOT GR4ADSB_BOOST_UT_INCLUDE_DIR)
      message(FATAL_ERROR "Boost.UT was not found. Install it, or point -DGR4ADSB_BOOST_UT_INCLUDE_DIR at its "
                          "include directory, or configure with -DGR4ADSB_ENABLE_TESTING=OFF.")
    endif()
    add_library(ut INTERFACE)
    target_include_directories(ut INTERFACE "${GR4ADSB_BOOST_UT_INCLUDE_DIR}")
  endif()
endif()

# The blocks are header only and the tests instantiate them directly, so no test links the block library; each needs
# the include path of the family under test and the core its headers are written against. blocklib-core is also
# required: the global block and scheduler registries are defined there, and a test that stands up a graph does not
# link without gr::globalBlockRegistry.
foreach(qa IN ITEMS qa_PpmFramer qa_ModeSDecode qa_AdsbPrinter)
  add_executable(${qa} ${qa}.cpp)
  target_link_libraries(${qa} PRIVATE gr4adsb-digital gr4adsb-adsb gnuradio4::gnuradio-core
                                      gnuradio4::gnuradio-blocklib-core ut gnuradio-options)
  target_include_directories(${qa} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_definitions(${qa} PRIVATE BOOST_UT_DISABLE_MODULE)
  add_test(NAME ${qa} COMMAND ${qa})
endforeach()
