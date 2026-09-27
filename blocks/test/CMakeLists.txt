# One executable of Boost.UT suites per block. They build their frames field by field from the standard and run short
# graphs over synthetic streams; none reads a file or opens a radio. Two more executables replay a recorded capture.
#
# The blocks are compiled into these tests rather than loaded from the library: a failure then points at a header rather
# than at what a plugin directory held.
foreach(
  qa_test IN
  ITEMS qa_PpmFramer
        qa_ModeSDecode
        qa_AdsbPrinter
        qa_PpmRecording
        qa_AdsbRecording)
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

# The two recording tests read a 1090 MHz capture from GR4_RECORDINGS_DIR: qa_PpmRecording runs the pulse-position
# scanner over it, and qa_AdsbRecording runs the whole chain from the file source to the printer. Each has an executable
# of its own, since exit 77 is a whole binary's status: an absent capture skips that test alone. The label `recording`
# selects or excludes both.
target_link_libraries(qa_AdsbRecording PRIVATE gnuradio4::gr-basic gnuradio4::gr-fileio)
foreach(qa_test IN ITEMS qa_PpmRecording qa_AdsbRecording)
  set_tests_properties(
    ${qa_test}
    PROPERTIES SKIP_RETURN_CODE
               77
               LABELS
               recording
               ENVIRONMENT
               "GR4_RECORDINGS_DIR=${GR4_RECORDINGS_DIR}")
endforeach()
set_tests_properties(qa_AdsbRecording PROPERTIES TIMEOUT 300)

# qa_AdsbLabels checks the key a graph file names each block by, and the family and the role each block declares to a
# registry. It is built where the core has block labels.
if(GR4ADSB_BLOCK_LABELS)
  add_executable(qa_AdsbLabels qa_AdsbLabels.cpp)
  target_link_libraries(
    qa_AdsbLabels
    PRIVATE gr4adsb-blocks
            gr4adsb-options
            gr4adsb-ut
            gnuradio4::gnuradio-blocklib-core)
  if(TARGET gnuradio4::gnuradio-plugin)
    target_link_libraries(qa_AdsbLabels PRIVATE gnuradio4::gnuradio-plugin)
  endif()
  add_test(NAME qa_AdsbLabels COMMAND qa_AdsbLabels)
  set_tests_properties(qa_AdsbLabels PROPERTIES TIMEOUT 60)
endif()
