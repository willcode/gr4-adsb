# One executable of Boost.UT suites per block. They build their frames field by field from the standard and run
# short graphs over synthetic streams; none reads a file or opens a radio.
#
# The blocks are compiled into these tests rather than loaded from the library, so a failure is about a header and
# not about what a plugin directory held.
foreach(qa_test IN ITEMS qa_PpmFramer qa_ModeSDecode qa_AdsbPrinter)
  add_executable(${qa_test} ${qa_test}.cpp)
  target_link_libraries(
    ${qa_test}
    PRIVATE gr4adsb-blocks
            gr4adsb-options
            gr4adsb-ut
            gnuradio4::gr-testing
            gnuradio4::gnuradio-blocklib-core)
  # gnuradio-core's Graph and BlockModel reference gr_plugin_base's out-of-line destructor, which is defined in
  # gnuradio-plugin alone; a test that builds a graph needs it on the link line.
  if(TARGET gnuradio4::gnuradio-plugin)
    target_link_libraries(${qa_test} PRIVATE gnuradio4::gnuradio-plugin)
  endif()
  add_test(NAME ${qa_test} COMMAND ${qa_test})
  set_tests_properties(${qa_test} PROPERTIES TIMEOUT 60)
endforeach()

# The decoder's test carries its records across gr::blocks::basic's two packet adapters.
target_link_libraries(qa_ModeSDecode PRIVATE gnuradio4::gr-basic)
