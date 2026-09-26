# Measures gr::digital::PpmScanner, the kernel PpmFramer runs on rather than the block itself, over Rayleigh noise and
# over a repeated anchor frame, at one and at four samples a slot. It builds with the tests, runs by hand, and is read
# against the copy arm beside it rather than in absolute terms.
add_executable(bm_PpmScanner bm_PpmScanner.cpp)
target_link_libraries(bm_PpmScanner PRIVATE gr4adsb-blocks gr4adsb-options gnuradio4::gr-testing)
target_compile_options(bm_PpmScanner PRIVATE -O3)
