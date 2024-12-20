project(mav_watch)

set(EXECUTE_NAME mav_watch)

set(MAV_WATCH_SOURCES
    mav_watch.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../led_control/led_control.cc
)

add_executable(${EXECUTE_NAME}
    ${MAV_WATCH_SOURCES}
)

target_include_directories(${EXECUTE_NAME}
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../
)

install(TARGETS ${EXECUTE_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})