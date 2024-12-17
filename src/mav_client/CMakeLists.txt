
project(mav_client)

set(EXECUTE_NAME mav_client)

set(MAV_CLIENT_SOURCES
    mav_client_bin.cpp
    mav_client.cpp
    camera_client.cpp
    camera_local_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../camera_param/camera_param.cc
)

if (BUILD_SERVER)
    set(MAV_CLIENT_SOURCES
        ${MAV_CLIENT_SOURCES}
        camera_rpc_client.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../generated/mavcam_options.grpc.pb.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/../generated/mavcam_options.pb.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/../generated/camera/camera.grpc.pb.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/../generated/camera/camera.pb.cc)
endif()

add_executable(${EXECUTE_NAME}
    ${MAV_CLIENT_SOURCES}
)

target_include_directories(${EXECUTE_NAME}
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../
    ${DEP_INSTALL_DIR}/include
)

set(MAVSDK_DIR ${CMAKE_PREFIX_PATH}/lib/cmake/MAVSDK/)

find_package(MAVSDK REQUIRED)

target_link_libraries(${EXECUTE_NAME}
    PRIVATE
    base
    MAVSDK::mavsdk
    dl
)

if (BUILD_SERVER)
    add_compile_definitions(ENABLE_SERVER)
    target_include_directories(${EXECUTE_NAME}
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../generated
    )

    find_package(OpenSSL REQUIRED)
    if (BUILD_QCOM)
        set(gRPC_DIR ${CMAKE_PREFIX_PATH}/lib/cmake/grpc)
        set(Protobuf_DIR ${CMAKE_PREFIX_PATH}/lib/cmake/protobuf)
        set(absl_DIR ${CMAKE_PREFIX_PATH}/lib/cmake/absl)
        set(re2_DIR ${CMAKE_PREFIX_PATH}/lib/cmake/re2)
        set(c-ares_DIR ${CMAKE_PREFIX_PATH}/lib/cmake/c-ares)
    endif()
    find_package(gRPC REQUIRED)

    target_link_libraries(${EXECUTE_NAME}
        PRIVATE
        gRPC::grpc++
    )
endif()

# get_cmake_property(_variableNames VARIABLES)
# foreach (_variableName ${_variableNames})
#    if (_variableName MATCHES "MAVSDK")
#       message(STATUS "${_variableName}=${${_variableName}}")
#    endif()
# endforeach()

get_target_property(MAVSDK_LIBRARY_PATH MAVSDK::mavsdk IMPORTED_LOCATION)
if(NOT MAVSDK_LIBRARY_PATH)
    # message(WARNING "IMPORTED_LOCATION for MAVSDK::mavsdk is empty, try to use LOCATION property")
    get_target_property(ALTERNATIVE_LIBRARY_PATH MAVSDK::mavsdk LOCATION)
    set(MAVSDK_LIBRARY_PATH ${ALTERNATIVE_LIBRARY_PATH})
endif()

message(STATUS "MAVSDK path: ${MAVSDK_LIBRARY_PATH}")

if (BUILD_MAVSDK)
    # TODO (anxs) temp solution to manually create symlink
    install(CODE "execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink libmavsdk.so.${MAVSDK_VERSION} libmavsdk.so.${MAVSDK_VERSION_MAJOR} WORKING_DIRECTORY ${CMAKE_INSTALL_PREFIX}/lib)")
    install(FILES ${MAVSDK_LIBRARY_PATH} DESTINATION lib)
endif()

install(TARGETS ${EXECUTE_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
