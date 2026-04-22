# librdkafka-oauthbearer-aws — AWS OAUTHBEARER token provider plugin
#
# Built as a shared library loaded into librdkafka via plugin.library.paths.
# Statically links aws-sdk-cpp (STS + Core + CRT) so the AWS SDK never
# leaks into librdkafka core's link graph; only consumers of this plugin
# pay the dep.

include(FetchContent)

# -----------------------------------------------------------------------------
# aws-sdk-cpp static build
#
# Earliest aws-sdk-cpp release containing sts:GetWebIdentityToken is 1.11.694
# (commit 8de5906ef3f — IAM outbound federation feature, AWS IAM GA 2025-11-19).
# Pinning here maximises the compat range for users; bump on demand.
# -----------------------------------------------------------------------------

set(AWSSDK_CPP_VERSION "1.11.694"
    CACHE STRING "aws-sdk-cpp git tag to fetch")

set(AWSSDK_LOCAL_SRC ""
    CACHE PATH "Optional path to a local aws-sdk-cpp source tree \
(with crt submodule populated). Overrides the GitHub fetch; useful for \
fast iteration or air-gapped builds.")

# AWS SDK build knobs — static, STS only, no tests.
# Must be set BEFORE FetchContent_MakeAvailable so aws-sdk-cpp's CMake
# sees them as it configures itself.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_ONLY "sts" CACHE STRING "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(AUTORUN_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(CUSTOM_MEMORY_MANAGEMENT OFF CACHE BOOL "" FORCE)
set(ENABLE_UNITY_BUILD OFF CACHE BOOL "" FORCE)

if(AWSSDK_LOCAL_SRC)
    message(STATUS "rdkafka-oauthbearer-aws: using local aws-sdk-cpp at ${AWSSDK_LOCAL_SRC}")
    FetchContent_Declare(awssdk SOURCE_DIR ${AWSSDK_LOCAL_SRC})
else()
    message(STATUS "rdkafka-oauthbearer-aws: fetching aws-sdk-cpp ${AWSSDK_CPP_VERSION} from GitHub")
    FetchContent_Declare(awssdk
        GIT_REPOSITORY https://github.com/aws/aws-sdk-cpp.git
        GIT_TAG ${AWSSDK_CPP_VERSION}
        GIT_SHALLOW TRUE)
endif()

FetchContent_MakeAvailable(awssdk)

# Disable -Werror on the vendored aws-sdk-cpp tree. aws-sdk-cpp compiles
# with -Werror by default; newer compilers (AppleClang 21, recent GCCs)
# occasionally enable additional warnings that hit upstream code aws-sdk-cpp
# has not yet cleaned up. That's upstream's hygiene concern, not ours — we
# need correctness, not zero-warnings. Keep -Werror on our own plugin code.
function(_rdoa_disable_werror_recursive dir)
    get_property(_tgts DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t ${_tgts})
        get_target_property(_type ${_t} TYPE)
        if(NOT _type STREQUAL "INTERFACE_LIBRARY" AND NOT _type STREQUAL "UTILITY")
            target_compile_options(${_t} PRIVATE -Wno-error)
        endif()
    endforeach()
    get_property(_subs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
    foreach(_sd ${_subs})
        _rdoa_disable_werror_recursive(${_sd})
    endforeach()
endfunction()

_rdoa_disable_werror_recursive(${awssdk_SOURCE_DIR})

# -----------------------------------------------------------------------------
# Plugin shared library
# -----------------------------------------------------------------------------

set(sources
    plugin.c
    config.c
    aws_sts_provider.cpp)

set(headers
    rdkafka_oauthbearer_aws.h)

add_library(rdkafka-oauthbearer-aws SHARED ${sources})

# Drop the "lib" prefix so plugin.library.paths=rdkafka-oauthbearer-aws
# resolves to rdkafka-oauthbearer-aws.{so,dylib,dll}. Matches
# tests/interceptor_test convention.
#
# Symbol hygiene (M6): default-hide every symbol in our C/C++ code.
# The two public entry points (conf_init, rd_kafka_oauthbearer_aws_register)
# re-promote themselves to default visibility via RD_OAUTHBEARER_AWS_EXPORT
# so dlsym() can resolve them.
set_target_properties(rdkafka-oauthbearer-aws PROPERTIES
    PREFIX ""
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

target_include_directories(rdkafka-oauthbearer-aws
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(rdkafka-oauthbearer-aws
    PUBLIC rdkafka
    PRIVATE aws-cpp-sdk-sts aws-cpp-sdk-core)

# Linker-side symbol hiding + C++ runtime isolation.
#
# CXX_VISIBILITY_PRESET=hidden above only affects our own compilation
# units. The aws-sdk-cpp / aws-crt-cpp / aws-c-* static archives were
# built with default visibility — without explicit linker help, their
# symbols still end up exported from the final plugin .so. Linux
# supports --exclude-libs,ALL which marks every static-archive symbol
# as hidden in the output. macOS's linker has no exact equivalent for
# AR-level visibility (hidden-l<name> is per-library and fragile);
# in practice the compiler-side preset + dead-stripping of unused
# archive members is sufficient to keep the final exports small.
#
# On Linux we also bundle libstdc++ / libgcc statically so downstream
# apps don't pick up a surprise NEEDED entry on the C++ runtime —
# this is the whole point of the plugin pattern (core librdkafka stays
# pure C even when this plugin is installed). macOS ships libc++ as
# part of the OS, so we leave that as a dynamic dep.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(rdkafka-oauthbearer-aws PRIVATE
        -static-libstdc++
        -static-libgcc
        "LINKER:--exclude-libs,ALL")
elseif(APPLE)
    # macOS has no --exclude-libs,ALL; an explicit exports list is the
    # cleanest way to strip static-archive symbols from the final dylib.
    set(_oba_exports_list
        "${CMAKE_CURRENT_SOURCE_DIR}/exports.list")
    target_link_options(rdkafka-oauthbearer-aws PRIVATE
        "LINKER:-exported_symbols_list,${_oba_exports_list}")
    # Re-link whenever the exports list changes.
    set_target_properties(rdkafka-oauthbearer-aws PROPERTIES
        LINK_DEPENDS "${_oba_exports_list}")
endif()

#------------------------------------------------------------------------------
# Static archive variant (M7)
#
# For consumers of librdkafka's static variant (`librdkafka-static.a`) —
# confluent-kafka-go via cgo, distroless / Alpine-static containers,
# embedded deployments — who cannot or do not want to dlopen the plugin.
# They call rd_kafka_oauthbearer_aws_register() explicitly at startup
# instead of setting plugin.library.paths.
#
# Shipped as a thin archive (just our plugin's object files). Users link
# it alongside their own librdkafka.a AND the aws-sdk-cpp / CRT static
# archives — pkg-config (`pkg-config --libs --static
# rdkafka-oauthbearer-aws`) surfaces the exact list.
#
# Separate target rather than an OBJECT library + two variants, because
# the small double-compile cost (3 files) is cheaper than the added
# CMake machinery; if this grows, switch to OBJECT.
#------------------------------------------------------------------------------
add_library(rdkafka-oauthbearer-aws-static STATIC ${sources})

set_target_properties(rdkafka-oauthbearer-aws-static PROPERTIES
    OUTPUT_NAME "rdkafka-oauthbearer-aws"    # → librdkafka-oauthbearer-aws.a
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

target_include_directories(rdkafka-oauthbearer-aws-static
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

# PUBLIC so aws-sdk-cpp headers are available during OUR compilation
# AND the link requirement propagates to downstream targets via
# find_package(rdkafka-oauthbearer-aws) in CMake consumers. Does NOT
# bundle the AWS archives into our .a — users still link them explicitly
# (via pkg-config or a future CMake config file).
target_link_libraries(rdkafka-oauthbearer-aws-static
    PUBLIC rdkafka aws-cpp-sdk-sts aws-cpp-sdk-core)

#------------------------------------------------------------------------------
# pkg-config generation
#------------------------------------------------------------------------------
# Assemble Libs.private. AWS SDK archive names are stable; the order
# matters for traditional Unix linkers (leaf deps after consumers).
# On macOS we add the frameworks the CRT depends on; on Linux we add
# -lpthread and -ldl which librdkafka already needs.
set(OBA_PKGCONFIG_STATIC_LIBS
    "-laws-cpp-sdk-sts -laws-cpp-sdk-core -laws-crt-cpp"
    "-laws-c-auth -laws-c-http -laws-c-io -laws-c-common"
    "-laws-c-cal -laws-c-sdkutils -laws-c-compression -laws-c-event-stream"
    # aws-crt-cpp's ApiHandle initialises *every* CRT subsystem at
    # startup, including s3 and mqtt — so their init/cleanup symbols
    # must resolve at link time even if our code never touches S3/MQTT.
    "-laws-c-s3 -laws-c-mqtt"
    "-laws-checksums"
    "-lcurl -lssl -lcrypto -lz")

# TLS stack differs by platform. On macOS, aws-sdk-cpp uses Apple's
# Security/Network frameworks — s2n and aws-lc are not built. On Linux,
# aws-sdk-cpp builds and uses s2n + aws-lc (bundled in aws-crt-cpp).
if(APPLE)
    list(APPEND OBA_PKGCONFIG_STATIC_LIBS
        "-framework Security -framework CoreFoundation -framework Network")
else()
    list(APPEND OBA_PKGCONFIG_STATIC_LIBS
        "-ls2n -laws-lc"
        "-lpthread -ldl -lrt -lm")
endif()

string(REPLACE ";" " " OBA_PKGCONFIG_STATIC_LIBS "${OBA_PKGCONFIG_STATIC_LIBS}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/rdkafka-oauthbearer-aws.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/rdkafka-oauthbearer-aws.pc"
    @ONLY)

#------------------------------------------------------------------------------
# Install rules
#------------------------------------------------------------------------------
install(
    TARGETS rdkafka-oauthbearer-aws rdkafka-oauthbearer-aws-static
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(
    FILES ${headers}
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/librdkafka"
)

install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/rdkafka-oauthbearer-aws.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
)

#------------------------------------------------------------------------------
# Plugin-local unit tests (M8)
#
# Gated by WITH_PLUGIN_OAUTHBEARER_AWS_TESTS so CI + distro builds can
# omit them. enable_testing() is called at the top-level CMakeLists.txt
# only when RDKAFKA_BUILD_TESTS=ON; we call it again here so the plugin
# tests remain runnable via `ctest` even when the librdkafka test suite
# is disabled (common for distro packaging builds).
#------------------------------------------------------------------------------
option(WITH_PLUGIN_OAUTHBEARER_AWS_TESTS
    "Build the AWS OAUTHBEARER plugin's unit tests" ON)

if(WITH_PLUGIN_OAUTHBEARER_AWS_TESTS)
    if(NOT CMAKE_TESTING_ENABLED)
        enable_testing()
    endif()
    add_subdirectory(tests)
endif()
