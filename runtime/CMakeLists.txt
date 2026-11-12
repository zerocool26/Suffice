cmake_minimum_required(VERSION 3.16)
project(suffice VERSION 0.13.0 LANGUAGES CXX)

option(SUFFICE_BUILD_TESTS "Build the exactness and format tests" ON)
option(SUFFICE_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(SUFFICE_NATIVE "Tune generated CPU instructions for the build machine" OFF)

set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)

add_library(suffice_core STATIC
    src/ir.cpp
    src/storage.cpp
    src/preimage.cpp
    src/encoded_ranges.cpp
    src/runtime.cpp
    src/vector.cpp
    src/predicate.cpp
    src/adaptive.cpp
    src/wordcode.cpp
    src/frontier.cpp
    src/residual.cpp
    src/boolean_proof.cpp
    src/frontier_runtime.cpp
    src/codegen.cpp
    src/portfolio_codegen.cpp
)
target_include_directories(suffice_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(suffice_core PUBLIC cxx_std_20)
target_link_libraries(suffice_core PUBLIC Threads::Threads)

add_executable(suffice src/main.cpp)
target_link_libraries(suffice PRIVATE suffice_core)

set(SUFFICE_TARGETS suffice_core suffice)
if(SUFFICE_BUILD_TESTS)
    enable_testing()
    add_executable(suffice_tests
        tests/main.cpp
        tests/test_ir.cpp
        tests/test_storage.cpp
        tests/test_preimage.cpp
        tests/test_encoded_ranges.cpp
        tests/test_runtime.cpp
        tests/test_vector.cpp
        tests/test_predicate.cpp
        tests/test_adaptive.cpp
        tests/test_decode.cpp
        tests/test_ranges.cpp
        tests/test_wordcode.cpp
        tests/test_frontier.cpp
        tests/test_residual.cpp
        tests/test_boolean_proof.cpp
        tests/test_frontier_runtime.cpp
        tests/test_codegen.cpp
        tests/test_portfolio.cpp
    )
    target_link_libraries(suffice_tests PRIVATE suffice_core)
    list(APPEND SUFFICE_TARGETS suffice_tests)
    # Assertions in a test translation unit must remain active in Release builds.
    if(MSVC)
        target_compile_options(suffice_tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(suffice_tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME suffice_correctness COMMAND suffice_tests)
endif()

option(SUFFICE_BUILD_PAYLOAD "Build the optional Zstandard payload application" OFF)
if(SUFFICE_BUILD_PAYLOAD)
    find_path(SUFFICE_ZSTD_INCLUDE_DIR zstd.h)
    find_library(SUFFICE_ZSTD_LIBRARY NAMES zstd libzstd)
    if(NOT SUFFICE_ZSTD_INCLUDE_DIR OR NOT SUFFICE_ZSTD_LIBRARY)
        message(FATAL_ERROR "Payload application needs Zstandard headers and library; set SUFFICE_ZSTD_INCLUDE_DIR and SUFFICE_ZSTD_LIBRARY.")
    endif()
    add_library(suffice_payload STATIC apps/payload/payload.cpp)
    target_include_directories(suffice_payload PRIVATE ${SUFFICE_ZSTD_INCLUDE_DIR})
    target_link_libraries(suffice_payload PUBLIC suffice_core ${SUFFICE_ZSTD_LIBRARY})
    add_executable(suffice_payload_cli apps/payload/main.cpp)
    set_target_properties(suffice_payload_cli PROPERTIES OUTPUT_NAME suffice_payload)
    target_link_libraries(suffice_payload_cli PRIVATE suffice_payload)
    add_executable(suffice_payload_bench apps/payload/benchmark.cpp apps/payload/sqlite_gate.cpp)
    target_link_libraries(suffice_payload_bench PRIVATE suffice_payload ${CMAKE_DL_LIBS})
    add_library(suffice_catalog STATIC apps/catalog/catalog.cpp apps/catalog/generation.cpp)
    target_link_libraries(suffice_catalog PUBLIC suffice_payload)
    add_executable(suffice_catalog_cli apps/catalog/main.cpp)
    set_target_properties(suffice_catalog_cli PROPERTIES OUTPUT_NAME suffice_catalog)
    target_link_libraries(suffice_catalog_cli PRIVATE suffice_catalog)
    list(APPEND SUFFICE_TARGETS suffice_payload suffice_payload_cli suffice_payload_bench suffice_catalog suffice_catalog_cli)
    if(SUFFICE_BUILD_TESTS)
        add_executable(suffice_payload_tests tests/payload_test_main.cpp tests/test_payload.cpp)
        target_include_directories(suffice_payload_tests PRIVATE ${SUFFICE_ZSTD_INCLUDE_DIR})
        target_link_libraries(suffice_payload_tests PRIVATE suffice_payload)
        if(MSVC)
            target_compile_options(suffice_payload_tests PRIVATE /UNDEBUG)
        else()
            target_compile_options(suffice_payload_tests PRIVATE -UNDEBUG)
        endif()
        list(APPEND SUFFICE_TARGETS suffice_payload_tests)
        add_test(NAME suffice_payload_correctness COMMAND suffice_payload_tests)
        add_executable(suffice_catalog_tests tests/catalog_test_main.cpp tests/test_catalog.cpp)
        target_include_directories(suffice_catalog_tests PRIVATE ${SUFFICE_ZSTD_INCLUDE_DIR})
        target_link_libraries(suffice_catalog_tests PRIVATE suffice_catalog)
        add_executable(suffice_digest_tests tests/catalog_digest_main.cpp)
        target_link_libraries(suffice_digest_tests PRIVATE suffice_core)
        add_executable(suffice_wire_tests tests/catalog_wire_main.cpp)
        target_link_libraries(suffice_wire_tests PRIVATE suffice_core)
        if(MSVC)
            target_compile_options(suffice_catalog_tests PRIVATE /UNDEBUG)
            target_compile_options(suffice_digest_tests PRIVATE /UNDEBUG)
            target_compile_options(suffice_wire_tests PRIVATE /UNDEBUG)
        else()
            target_compile_options(suffice_catalog_tests PRIVATE -UNDEBUG)
            target_compile_options(suffice_digest_tests PRIVATE -UNDEBUG)
            target_compile_options(suffice_wire_tests PRIVATE -UNDEBUG)
        endif()
        list(APPEND SUFFICE_TARGETS suffice_catalog_tests suffice_digest_tests suffice_wire_tests)
        add_test(NAME suffice_catalog_correctness COMMAND suffice_catalog_tests)
        add_test(NAME suffice_digest_correctness COMMAND suffice_digest_tests)
        add_test(NAME suffice_wire_correctness COMMAND suffice_wire_tests)
    endif()
    install(TARGETS suffice_payload_cli suffice_catalog_cli RUNTIME DESTINATION bin)
endif()

foreach(target IN LISTS SUFFICE_TARGETS)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(SUFFICE_NATIVE)
        if(MSVC)
            message(FATAL_ERROR "SUFFICE_NATIVE is supported only by GCC or Clang; choose explicit MSVC architecture flags separately.")
        else()
            target_compile_options(${target} PRIVATE -march=native)
        endif()
    endif()
    if(SUFFICE_SANITIZE)
        if(MSVC)
            message(FATAL_ERROR "The combined sanitizer build requires GCC or Clang; it has not been validated with MSVC.")
        else()
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
endforeach()

install(TARGETS suffice RUNTIME DESTINATION bin)
