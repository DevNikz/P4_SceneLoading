cmake_minimum_required (VERSION 3.20)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

project ("P4_SceneLoading")

find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(OpenGL REQUIRED) # ensures OpenGL system lib (opengl32 on Windows)
find_package(imgui CONFIG REQUIRED)
find_package(tinyobjloader CONFIG REQUIRED)

# ---- Protobuf / gRPC generation ----
set(PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/proto/sceneloader.proto)
set(GENERATED_PROTO_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto/generated)
file(MAKE_DIRECTORY ${GENERATED_PROTO_DIR})

add_custom_command(
  OUTPUT ${GENERATED_PROTO_DIR}/sceneloader.pb.cc ${GENERATED_PROTO_DIR}/sceneloader.pb.h ${GENERATED_PROTO_DIR}/sceneloader.grpc.pb.cc ${GENERATED_PROTO_DIR}/sceneloader.grpc.pb.h
  COMMAND $<TARGET_FILE:protobuf::protoc>
    --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
    --cpp_out=${GENERATED_PROTO_DIR}
    --grpc_out=${GENERATED_PROTO_DIR}
    --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
    ${PROTO_FILES}
  DEPENDS ${PROTO_FILES}
  COMMENT "Generating protobuf and gRPC sources from ${PROTO_FILES}"
  VERBATIM
)

add_custom_target(proto_generated
  DEPENDS ${GENERATED_PROTO_DIR}/sceneloader.pb.cc ${GENERATED_PROTO_DIR}/sceneloader.grpc.pb.cc
)

# Expose generated include dir
include_directories(${GENERATED_PROTO_DIR})

# ---- Existing project layout (uses generated proto sources) ----
set(PROTO_SRC_PATH "${GENERATED_PROTO_DIR}" CACHE PATH "Proto Files" FORCE)

file(GLOB_RECURSE PROTO_SRC "${PROTO_SRC_PATH}/*.cc")
file(GLOB_RECURSE PROTO_HEADERS "${PROTO_SRC_PATH}/*.h")

# Server sources
set(SERVER_SRC_PATH ${CMAKE_CURRENT_SOURCE_DIR}/src_server CACHE PATH "Project Server SRC" FORCE)
file(GLOB_RECURSE SERVER_SRC CONFIGURE_DEPENDS "${SERVER_SRC_PATH}/*.[ch]pp")

# Client sources (restore this so implementations are compiled & linked)
set(CLIENT_SRC_PATH ${CMAKE_CURRENT_SOURCE_DIR}/src_client CACHE PATH "Project Client SRC" FORCE)
file(GLOB_RECURSE CLIENT_SRC CONFIGURE_DEPENDS "${CLIENT_SRC_PATH}/*.[ch]pp")

# Explicit generated proto filenames (the custom_command generates these)
set(GENERATED_PROTO_SRCS
    ${GENERATED_PROTO_DIR}/sceneloader.pb.cc
    ${GENERATED_PROTO_DIR}/sceneloader.grpc.pb.cc
)
set(GENERATED_PROTO_HDRS
    ${GENERATED_PROTO_DIR}/sceneloader.pb.h
    ${GENERATED_PROTO_DIR}/sceneloader.grpc.pb.h
)

# Server - only server sources, plus generated proto sources/headers
add_executable(P4_Server
    src_server/P4_Server.cpp
    ${SERVER_SRC}
    ${GENERATED_PROTO_SRCS}
    ${GENERATED_PROTO_HDRS}
)
add_dependencies(P4_Server proto_generated)
target_compile_definitions(P4_Server PUBLIC "PROTOBUF_USE_DLLS")
target_include_directories(P4_Server PRIVATE ${GENERATED_PROTO_DIR})
target_link_libraries(P4_Server PUBLIC
    protobuf::libprotobuf
    gRPC::grpc
    gRPC::grpc++
    gRPC::grpc++_reflection
)

# Client - client sources + generated proto sources/headers
add_executable(P4_Client
    src_client/P4_Client.cpp
    ${CLIENT_SRC}
    ${GENERATED_PROTO_SRCS}
    ${GENERATED_PROTO_HDRS}
)
add_dependencies(P4_Client proto_generated)
target_compile_definitions(P4_Client PUBLIC "PROTOBUF_USE_DLLS")
target_include_directories(P4_Client PRIVATE ${GENERATED_PROTO_DIR})
target_include_directories(P4_Client PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb)

# Robustly resolve imported targets provided by packages (vcpkg or system)
# GLFW
set(GLFW_TARGET "")
if(TARGET glfw3::glfw)
    set(GLFW_TARGET glfw3::glfw)
elseif(TARGET glfw::glfw)
    set(GLFW_TARGET glfw::glfw)
elseif(TARGET glfw)
    set(GLFW_TARGET glfw)
endif()

# Glad
set(GLAD_TARGET "")
if(TARGET glad::glad)
    set(GLAD_TARGET glad::glad)
elseif(TARGET glad)
    set(GLAD_TARGET glad)
endif()

# GLM
set(GLM_TARGET "")
if(TARGET glm::glm)
    set(GLM_TARGET glm::glm)
elseif(TARGET glm)
    set(GLM_TARGET glm)
endif()

# ImGui
set(IMGUI_TARGET "")
if(TARGET imgui::imgui)
    set(IMGUI_TARGET imgui::imgui)
elseif(TARGET imgui)
    set(IMGUI_TARGET imgui)
endif()

# Tinyobjloader
set(TINYOBJ_TARGET "")
if(TARGET tinyobjloader::tinyobjloader)
    set(TINYOBJ_TARGET tinyobjloader::tinyobjloader)
elseif(TARGET tinyobjloader)
    set(TINYOBJ_TARGET tinyobjloader)
endif()

# Verify essential targets and provide helpful errors
if(GLFW_TARGET STREQUAL "")
    message(FATAL_ERROR "Could not find an imported GLFW target (glfw3). Make sure glfw3 is installed and you passed -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake to CMake.")
endif()
if(GLAD_TARGET STREQUAL "")
    message(FATAL_ERROR "Could not find an imported GLAD target (glad). Make sure glad is installed and you passed -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake to CMake.")
endif()
if(GLM_TARGET STREQUAL "")
    message(FATAL_ERROR "Could not find an imported GLM target (glm). Make sure glm is installed and you passed -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake to CMake.")
endif()
if(IMGUI_TARGET STREQUAL "")
    message(FATAL_ERROR "Could not find an imported ImGui target (imgui). Install via vcpkg and pass the vcpkg toolchain to CMake.")
endif()
if(TINYOBJ_TARGET STREQUAL "")
    message(FATAL_ERROR "Could not find an imported tinyobjloader target. Install via vcpkg and pass the vcpkg toolchain to CMake.")
endif()

# --- detect stb (vcpkg imported target) ---
set(STB_TARGET "")
if(TARGET stb::stb)
    set(STB_TARGET stb::stb)
elseif(TARGET stb)
    set(STB_TARGET stb)
endif()

if(STB_TARGET STREQUAL "")
    message(WARNING "Could not find an imported stb target. If you want to build skybox support you should install 'stb' via vcpkg and pass the vcpkg toolchain to CMake.")
endif()

# Link client against both networking and rendering deps
target_link_libraries(P4_Client PRIVATE
    protobuf::libprotobuf
    gRPC::grpc
    gRPC::grpc++
    gRPC::grpc++_reflection

    ${GLFW_TARGET}
    ${GLAD_TARGET}
    ${GLM_TARGET}
    ${IMGUI_TARGET}
    ${TINYOBJ_TARGET}
    ${STB_TARGET}
    OpenGL::GL

    imgui
    imgui_backends
)

include(FetchContent)

FetchContent_Declare(
  imgui_repo
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.9  # match the version you have in vcpkg; change if needed
)
FetchContent_MakeAvailable(imgui_repo)

# Create a core ImGui static library if not provided by the system/vcpkg
if (NOT TARGET imgui)
    add_library(imgui STATIC
        ${imgui_repo_SOURCE_DIR}/imgui.cpp
        ${imgui_repo_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_repo_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_repo_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_repo_SOURCE_DIR}/imgui_widgets.cpp
    )
    target_include_directories(imgui PUBLIC
        ${imgui_repo_SOURCE_DIR}
    )
    target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_LOADER_GLAD)
endif()

# Backends (GLFW + OpenGL3)
if (NOT TARGET imgui_backends)
    add_library(imgui_backends STATIC
        ${imgui_repo_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_repo_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(imgui_backends PUBLIC
        ${imgui_repo_SOURCE_DIR}
        ${imgui_repo_SOURCE_DIR}/backends
    )
    target_link_libraries(imgui_backends PUBLIC
        ${GLFW_TARGET}
        ${GLAD_TARGET}
        OpenGL::GL
    )
    target_compile_definitions(imgui_backends PUBLIC IMGUI_IMPL_OPENGL_LOADER_GLAD)
endif()

# Link ImGui and backends to P4_Client
target_link_libraries(P4_Client PRIVATE imgui imgui_backends)

# Ensure both server and client binaries are placed in the desired folder (out/build/x64-debug)
set(OUTPUT_BIN_DIR "${CMAKE_SOURCE_DIR}/out/build/x64-debug")
file(MAKE_DIRECTORY "${OUTPUT_BIN_DIR}")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_BIN_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${OUTPUT_BIN_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${OUTPUT_BIN_DIR}")

foreach(cfg IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${cfg}" CFG_UPPER)
    set(var_runtime "CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG_UPPER}")
    set(var_lib "CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CFG_UPPER}")
    set(var_archive "CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CFG_UPPER}")
    set(${var_runtime} "${OUTPUT_BIN_DIR}" CACHE PATH "Runtime output dir for ${cfg}" FORCE)
    set(${var_lib} "${OUTPUT_BIN_DIR}" CACHE PATH "Library output dir for ${cfg}" FORCE)
    set(${var_archive} "${OUTPUT_BIN_DIR}" CACHE PATH "Archive output dir for ${cfg}" FORCE)
endforeach()

if(TARGET P4_Server OR TARGET P4_Client)
    set_target_properties(P4_Server P4_Client PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_BIN_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${OUTPUT_BIN_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${OUTPUT_BIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${OUTPUT_BIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OUTPUT_BIN_DIR}"
    )
endif()

