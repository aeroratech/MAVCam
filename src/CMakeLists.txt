cmake_minimum_required(VERSION 3.14)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(cmake/compiler_flags.cmake)

option(BUILD_SERVER "Build server and client with grpc support" ON)

add_subdirectory(base)
add_subdirectory(mav_client)
if (BUILD_SERVER)
    add_subdirectory(mav_server)
endif()
add_subdirectory(mav_watch)

#install definition file
set(INSTALL_DESTINATION ${CMAKE_INSTALL_PREFIX}/share/mav-cam/definition/)
set(DEFINITION_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/definition/D64TR.xml
    ${CMAKE_CURRENT_SOURCE_DIR}/definition/Q50MZ.xml
)

install(FILES ${DEFINITION_FILES} DESTINATION ${INSTALL_DESTINATION})
