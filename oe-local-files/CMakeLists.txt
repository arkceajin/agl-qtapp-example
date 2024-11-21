cmake_minimum_required(VERSION 3.16)

project(hello-qml VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick )
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

include(cmake/common.cmake)

## gRPC
    set(proto_name "agl_shell")
    # Proto file
    get_filename_component(hw_proto "proto/${proto_name}.proto" ABSOLUTE)
    get_filename_component(hw_proto_path "${hw_proto}" PATH)

    # Generated sources
    set(hw_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/${proto_name}.pb.cc")
    set(hw_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/${proto_name}.pb.h")
    set(hw_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/${proto_name}.grpc.pb.cc")
    set(hw_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/${proto_name}.grpc.pb.h")
    add_custom_command(
      OUTPUT "${hw_proto_srcs}" "${hw_proto_hdrs}" "${hw_grpc_srcs}" "${hw_grpc_hdrs}"
      COMMAND ${_PROTOBUF_PROTOC}
      ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
        --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
        -I "${hw_proto_path}"
        --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${hw_proto}"
      DEPENDS "${hw_proto}")

    # Include generated *.pb.h files
    include_directories("${CMAKE_CURRENT_BINARY_DIR}")

    # hw_grpc_proto
    add_library(hw_grpc_proto
        ${hw_grpc_srcs}
        ${hw_grpc_hdrs}
        ${hw_proto_srcs}
        ${hw_proto_hdrs})
    target_link_libraries(hw_grpc_proto
        ${_REFLECTION}
        ${_GRPC_GRPCPP}
        ${_PROTOBUF_LIBPROTOBUF})


qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(apphello-qml
    main.cpp
    ${PROTO_SRCS}
    ${GRPC_SRCS}
    src/homescreenhandler.cpp
    src/AglShellGrpcClient.cpp
    src/applicationlauncher.cpp
    ${PROTO_FILES}
)

qt_add_qml_module(apphello-qml
    URI hello-qml
    VERSION 1.0
    QML_FILES
        Main.qml
)

target_include_directories(apphello-qml PRIVATE src )

target_link_libraries(apphello-qml
    PRIVATE Qt6::Quick Qt6::GuiPrivate
    # wayland-client
    PUBLIC
        protobuf::libprotobuf
        gRPC::grpc
        gRPC::grpc++

        hw_grpc_proto
        ${_REFLECTION}
        ${_GRPC_GRPCPP}
        ${_PROTOBUF_LIBPROTOBUF}
)


target_include_directories(apphello-qml PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

# https://github.com/faaxm/exmpl-cmake-grpc/blob/master/proto/CMakeLists.txt#L28
# get_target_property(grpc_cpp_plugin_location gRPC::grpc_cpp_plugin LOCATION)
# protobuf_generate(TARGET apphello-qml LANGUAGE cpp)
# protobuf_generate(TARGET apphello-qml LANGUAGE grpc GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc PLUGIN "protoc-gen-grpc=${_GRPC_CPP_PLUGIN_EXECUTABLE}")

include(GNUInstallDirs)
install(TARGETS apphello-qml
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

set(CMAKE_FIND_DEBUG_MODE ON)
set(CMAKE_VERBOSE_MAKEFILE ON)
