cmake_minimum_required(VERSION 3.22)

project(minimal_invite_rta_client LANGUAGES CXX)

string( REPLACE "-D_FORTIFY_SOURCE=2" "-D_FORTIFY_SOURCE=0" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}" )
string( REPLACE "-D_FORTIFY_SOURCE=2" "-D_FORTIFY_SOURCE=0" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}" )

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(protobuf)
add_subdirectory(google-play-api)

set(XAL_ANDROID_COMPAT_INCLUDE_DIR "" CACHE PATH "Optional fallback include root for Android-specific XAL headers missing from some GDK snapshots")
set(LIBHTTPCLIENT_ANDROID_COMPAT_INCLUDE_DIR "" CACHE PATH "Optional fallback include root for Android-only libHttpClient headers missing from some GDK snapshots")
set(RAPIDJSON_INCLUDE_DIR "" CACHE PATH "Path to the rapidjson include directory")
set(MCPE_LIB_DIR "" CACHE PATH "Directory containing libminecraftpe.so and libHttpClient.Android.so")

find_package(Protobuf)

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS mcs.proto)

add_library(minecraftpe SHARED IMPORTED GLOBAL)
set_target_properties(minecraftpe PROPERTIES
    IMPORTED_LOCATION "${MCPE_LIB_DIR}/libminecraftpe.so"
)

add_library(libhttpclient_android SHARED IMPORTED GLOBAL)
set_target_properties(libhttpclient_android PROPERTIES
    IMPORTED_LOCATION "${MCPE_LIB_DIR}/libHttpClient.Android.so"
)

add_library(minimal_invite_rta_client SHARED
    src/MinimalInviteRtaClient.cpp ${PROTO_SRCS}
)

target_include_directories(minimal_invite_rta_client
    PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/include"
    PRIVATE
        "${XAL_ANDROID_COMPAT_INCLUDE_DIR}"
        "${LIBHTTPCLIENT_ANDROID_COMPAT_INCLUDE_DIR}"
        "${RAPIDJSON_INCLUDE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}"
)

target_link_libraries(minimal_invite_rta_client
    PRIVATE
        minecraftpe
        libhttpclient_android
        log
        libprotobuf

        gplayapi
)

target_compile_options(minimal_invite_rta_client PRIVATE
    -fvisibility=hidden
    -Wall
    -Wextra
    -Wpedantic
)
