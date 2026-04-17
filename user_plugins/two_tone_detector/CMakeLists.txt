add_library(two_tone_detector
  MODULE
    two_tone_detector.cc
)

target_link_libraries(two_tone_detector
  trunk_recorder_library
  ${Boost_LIBRARIES}
  ${GNURADIO_PMT_LIBRARIES}
  ${GNURADIO_RUNTIME_LIBRARIES}
  ${GNURADIO_FILTER_LIBRARIES}
  ${GNURADIO_DIGITAL_LIBRARIES}
  ${GNURADIO_ANALOG_LIBRARIES}
  ${GNURADIO_AUDIO_LIBRARIES}
  ${GNURADIO_UHD_LIBRARIES}
  ${UHD_LIBRARIES}
  ${GNURADIO_BLOCKS_LIBRARIES}
  ${GNURADIO_OSMOSDR_LIBRARIES}
  ${LIBOP25_REPEATER_LIBRARIES}
  gnuradio-op25_repeater
)

if(NOT Gnuradio_VERSION VERSION_LESS "3.8")
  target_link_libraries(two_tone_detector
    gnuradio::gnuradio-analog
    gnuradio::gnuradio-blocks
    gnuradio::gnuradio-digital
    gnuradio::gnuradio-filter
    gnuradio::gnuradio-pmt
  )
endif()

install(TARGETS two_tone_detector LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/trunk-recorder)
