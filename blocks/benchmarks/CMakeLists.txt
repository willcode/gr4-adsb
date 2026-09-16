# The scanner's benchmark, which came with the blocks. It measures gr::digital::PpmScanner -- the kernel PpmFramer
# runs on, not the block -- over Rayleigh noise and over a repeated anchor frame, at one and at four samples a slot.
# It is built with the tests and run by hand: a timing figure on a machine that is doing something else is not a
# gate, and this one is read against the copy arm beside it rather than in absolute terms.
add_executable(bm_PpmScanner bm_PpmScanner.cpp)
target_link_libraries(bm_PpmScanner PRIVATE gr4adsb-blocks gr4adsb-options gnuradio4::gr-testing)
target_compile_options(bm_PpmScanner PRIVATE -O3)
