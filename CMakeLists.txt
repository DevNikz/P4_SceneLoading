cmake_minimum_required (VERSION 3.20)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

project ("P4_SceneLoading")

find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

set(PROTO_SRC_PATH "${CMAKE_CURRENT_SOURCE_DIR}/proto/generated" CACHE PATH "Proto Files" FORCE)

file(GLOB_RECURSE PROTO_SRC "${PROTO_SRC_PATH}/*.cc")
file(GLOB_RECURSE PROTO_HEADERS "${PROTO_SRC_PATH}/*.h")

#Server
set(SERVER_SRC_PATH ${CMAKE_CURRENT_SOURCE_DIR}/src_server CACHE PATH "Project Server SRC" FORCE)
file(GLOB_RECURSE SERVER_SRC CONFIGURE_DEPENDS "${SERVER_SRC_PATH}/*.[ch]pp")
add_executable(P4_Server "P4_Server.cpp" "P4_Server.h" ${PROTO_SRC} ${PROTO_HEADERS} ${SERVER_SRC})
target_compile_definitions(P4_Server PUBLIC "PROTOBUF_USE_DLLS")
target_link_libraries(P4_Server PUBLIC
	protobuf::libprotobuf
	gRPC::grpc
	gRPC::grpc++
	gRPC::grpc++_reflection
)

#Client
set(CLIENT_SRC_PATH ${CMAKE_CURRENT_SOURCE_DIR}/src_client CACHE PATH "Project Client SRC" FORCE)
file(GLOB_RECURSE CLIENT_SRC CONFIGURE_DEPENDS "${CLIENT_SRC_PATH}/*.[ch]pp")
add_executable(P4_Client "P4_Client.cpp" "P4_Client.h" ${PROTO_SRC} ${PROTO_HEADERS} ${CLIENT_SRC})
target_compile_definitions(P4_Client PUBLIC "PROTOBUF_USE_DLLS")
target_link_libraries(P4_Client PUBLIC
	protobuf::libprotobuf
	gRPC::grpc
	gRPC::grpc++
	gRPC::grpc++_reflection
)