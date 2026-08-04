cmake_minimum_required(VERSION 3.16)
project(DAGE VERSION 0.2.0 LANGUAGES C CXX)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(DAGE_BUILD_SHARED "Build shared library" ON)
option(DAGE_BUILD_TESTS "Build tests" ON)
option(DAGE_BUILD_TOOLS "Build CLI tools" ON)
option(DAGE_BUILD_EXAMPLES "Build examples" ON)
option(DAGE_BUILD_BENCHMARKS "Build performance benchmark harness" OFF)
option(DAGE_BUILD_FUZZERS "Build Clang libFuzzer targets" OFF)
option(DAGE_WARNINGS_AS_ERRORS "Treat warnings as errors for DAGE-owned targets" OFF)
option(DAGE_REQUIRE_DEPENDENCY_VERSIONS
  "Fail configuration when an enabled dependency version cannot be inventoried" OFF)
option(DAGE_BUILD_BINDINGS "Build bindings" OFF)
option(DAGE_ENABLE_SANITIZERS "Enable sanitizers" OFF)
set(DAGE_SANITIZERS "address,undefined" CACHE STRING
  "Comma-separated sanitizers used when DAGE_ENABLE_SANITIZERS is ON")

if(DAGE_WARNINGS_AS_ERRORS)
  if(MSVC)
    add_compile_options(/W4 /WX)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
  endif()
endif()
option(DAGE_BUILD_OPENTELEMETRY_BRIDGE "Build SDK-neutral OpenTelemetry bridge" ON)
option(DAGE_BUILD_BUNDLE_TOOLS "Build official Directory/ZIP resource providers" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(jsoncpp CONFIG QUIET)
set(DAGE_CONFIG_JSONCPP_PKG OFF)
if(TARGET JsonCpp::JsonCpp)
  set(DAGE_JSONCPP_TARGET JsonCpp::JsonCpp)
elseif(TARGET jsoncpp_lib)
  set(DAGE_JSONCPP_TARGET jsoncpp_lib)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
  set(DAGE_JSONCPP_TARGET PkgConfig::JSONCPP)
  set(DAGE_CONFIG_JSONCPP_PKG ON)
endif()
find_package(OpenSSL QUIET)
set(DAGE_CONFIG_OPENSSL_PKG OFF)
if(OpenSSL_FOUND)
  set(DAGE_OPENSSL_TARGET OpenSSL::Crypto)
else()
  if(NOT PkgConfig_FOUND)
    find_package(PkgConfig REQUIRED)
  endif()
  pkg_check_modules(OPENSSL REQUIRED IMPORTED_TARGET openssl)
  set(DAGE_OPENSSL_TARGET PkgConfig::OPENSSL)
  set(DAGE_CONFIG_OPENSSL_PKG ON)
endif()
if(DAGE_BUILD_BUNDLE_TOOLS)
  set(DAGE_CONFIG_ZLIB_PKG OFF)
  find_package(ZLIB QUIET)
  if(ZLIB_FOUND)
    set(DAGE_ZLIB_TARGET ZLIB::ZLIB)
  else()
    if(NOT PkgConfig_FOUND)
      find_package(PkgConfig REQUIRED)
    endif()
    pkg_check_modules(ZLIB_PC REQUIRED IMPORTED_TARGET zlib)
    set(DAGE_ZLIB_TARGET PkgConfig::ZLIB_PC)
    set(DAGE_CONFIG_ZLIB_PKG ON)
  endif()
endif()

set(DAGE_JSONCPP_VERSION "")
foreach(DAGE_VERSION_CANDIDATE IN ITEMS jsoncpp_VERSION JsonCpp_VERSION JSONCPP_VERSION)
  if(DEFINED ${DAGE_VERSION_CANDIDATE} AND NOT "${${DAGE_VERSION_CANDIDATE}}" STREQUAL "")
    set(DAGE_JSONCPP_VERSION "${${DAGE_VERSION_CANDIDATE}}")
    break()
  endif()
endforeach()
set(DAGE_OPENSSL_VERSION "")
foreach(DAGE_VERSION_CANDIDATE IN ITEMS OpenSSL_VERSION OPENSSL_VERSION)
  if(DEFINED ${DAGE_VERSION_CANDIDATE} AND NOT "${${DAGE_VERSION_CANDIDATE}}" STREQUAL "")
    set(DAGE_OPENSSL_VERSION "${${DAGE_VERSION_CANDIDATE}}")
    break()
  endif()
endforeach()
set(DAGE_ZLIB_VERSION "not-enabled")
if(DAGE_BUILD_BUNDLE_TOOLS)
  set(DAGE_ZLIB_VERSION "")
  foreach(DAGE_VERSION_CANDIDATE IN ITEMS ZLIB_VERSION ZLIB_VERSION_STRING ZLIB_PC_VERSION)
    if(DEFINED ${DAGE_VERSION_CANDIDATE} AND NOT "${${DAGE_VERSION_CANDIDATE}}" STREQUAL "")
      set(DAGE_ZLIB_VERSION "${${DAGE_VERSION_CANDIDATE}}")
      break()
    endif()
  endforeach()
endif()
if(DAGE_REQUIRE_DEPENDENCY_VERSIONS)
  foreach(DAGE_REQUIRED_VERSION IN ITEMS DAGE_JSONCPP_VERSION DAGE_OPENSSL_VERSION DAGE_ZLIB_VERSION)
    if("${${DAGE_REQUIRED_VERSION}}" STREQUAL "")
      message(FATAL_ERROR
        "Cannot create release dependency inventory: ${DAGE_REQUIRED_VERSION} is unknown")
    endif()
  endforeach()
endif()
foreach(DAGE_OPTION IN ITEMS DAGE_BUILD_SHARED DAGE_BUILD_BUNDLE_TOOLS)
  if(${DAGE_OPTION})
    set(DAGE_INVENTORY_${DAGE_OPTION} true)
  else()
    set(DAGE_INVENTORY_${DAGE_OPTION} false)
  endif()
endforeach()
set(DAGE_INVENTORY_SHARED ${DAGE_INVENTORY_DAGE_BUILD_SHARED})
set(DAGE_INVENTORY_BUNDLE_TOOLS ${DAGE_INVENTORY_DAGE_BUILD_BUNDLE_TOOLS})
configure_file(cmake/dage-dependencies.json.in
  ${CMAKE_CURRENT_BINARY_DIR}/dage-dependencies.json @ONLY)

set(DAGE_LIBRARY_TYPE STATIC)
if(DAGE_BUILD_SHARED)
  set(DAGE_LIBRARY_TYPE SHARED)
endif()

if(DAGE_BUILD_BINDINGS)
  find_package(JNI REQUIRED)
  add_library(dage_jni SHARED bindings/java/dage_jni.cpp)
  target_include_directories(dage_jni PRIVATE ${JNI_INCLUDE_DIRS})
  target_link_libraries(dage_jni PRIVATE dage)
  if(WIN32)
    set_target_properties(dage_jni PROPERTIES PREFIX "")
  endif()
endif()

add_library(dage ${DAGE_LIBRARY_TYPE}
  src/dage.cpp
  src/c_api.cpp
  src/scheduler.cpp
  src/resource_provider.cpp
  src/bundle.cpp
  src/ir.cpp
  src/sha256.cpp
  src/bundle_resolver.cpp)
target_sources(dage PRIVATE src/reliability.cpp)
target_sources(dage PRIVATE src/trace.cpp)
target_sources(dage PRIVATE src/resource_lease.cpp)
target_include_directories(dage PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(dage PRIVATE ${DAGE_JSONCPP_TARGET} ${DAGE_OPENSSL_TARGET})
target_compile_definitions(dage PRIVATE DAGE_BUILDING_LIBRARY)
set_target_properties(dage PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 0)
if(NOT DAGE_BUILD_SHARED)
  target_compile_definitions(dage PUBLIC DAGE_STATIC)
endif()
if(DAGE_BUILD_BUNDLE_TOOLS)
  add_library(dage_bundle_tools STATIC src/tool_resource_providers.cpp src/bundle_store.cpp)
  target_include_directories(dage_bundle_tools PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(dage_bundle_tools PUBLIC dage PRIVATE ${DAGE_ZLIB_TARGET} ${DAGE_JSONCPP_TARGET})
endif()
if(DAGE_BUILD_OPENTELEMETRY_BRIDGE)
  add_library(dage_opentelemetry STATIC src/opentelemetry.cpp)
  target_include_directories(dage_opentelemetry PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(dage_opentelemetry PUBLIC dage)
endif()
if(WIN32)
  target_compile_definitions(dage PRIVATE DAGE_EXPORTS)
  if(DAGE_BUILD_SHARED)
    set_target_properties(dage PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    if(MINGW)
      target_link_options(dage PRIVATE -Wl,--export-all-symbols)
    endif()
  endif()
endif()
if(DAGE_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(dage PUBLIC "-fsanitize=${DAGE_SANITIZERS}" -fno-omit-frame-pointer)
  target_link_options(dage PUBLIC "-fsanitize=${DAGE_SANITIZERS}")
endif()

if(DAGE_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "DAGE_BUILD_FUZZERS requires Clang libFuzzer support")
  endif()
  add_executable(dage_workflow_fuzzer fuzz/workflow_fuzzer.cpp)
  target_link_libraries(dage_workflow_fuzzer PRIVATE dage)
  target_compile_options(dage_workflow_fuzzer PRIVATE
    -fsanitize=fuzzer -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Werror)
  target_link_options(dage_workflow_fuzzer PRIVATE -fsanitize=fuzzer)
endif()

if(DAGE_BUILD_TOOLS)
  add_executable(dage_cli tools/dage_cli.cpp)
  target_link_libraries(dage_cli PRIVATE dage)
  if(DAGE_BUILD_BUNDLE_TOOLS)
    target_link_libraries(dage_cli PRIVATE dage_bundle_tools)
    target_compile_definitions(dage_cli PRIVATE DAGE_HAS_BUNDLE_TOOLS=1)
  endif()
  set_target_properties(dage_cli PROPERTIES OUTPUT_NAME dage)
endif()

if(DAGE_BUILD_EXAMPLES)
  add_executable(dage_cpp_example examples/cpp/basic.cpp)
  target_link_libraries(dage_cpp_example PRIVATE dage)
  add_executable(dage_hsm_kms_example examples/cpp/hsm_kms_key_provider.cpp)
  target_link_libraries(dage_hsm_kms_example PRIVATE dage)
endif()

set(DAGE_INSTALL_TARGETS dage)
if(DAGE_BUILD_BUNDLE_TOOLS)
  list(APPEND DAGE_INSTALL_TARGETS dage_bundle_tools)
endif()
if(DAGE_BUILD_OPENTELEMETRY_BRIDGE)
  list(APPEND DAGE_INSTALL_TARGETS dage_opentelemetry)
endif()
install(TARGETS ${DAGE_INSTALL_TARGETS}
  EXPORT DAGETargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(DAGE_BUILD_TOOLS)
  install(TARGETS dage_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE README.md
  DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/DAGE)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/dage-dependencies.json
  DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/DAGE)
install(EXPORT DAGETargets
  FILE DAGETargets.cmake
  NAMESPACE DAGE::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DAGE)
configure_package_config_file(
  cmake/DAGEConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/DAGEConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DAGE)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/DAGEConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/DAGEConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/DAGEConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DAGE)
configure_file(cmake/dage.pc.in ${CMAKE_CURRENT_BINARY_DIR}/dage.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/dage.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

set(CPACK_PACKAGE_NAME "DAGE")
set(CPACK_PACKAGE_VENDOR "DAGE Project")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
  "Small, embeddable, provider-agnostic DAG runtime for AI applications")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME
  "DAGE-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_CHECKSUM "SHA256")
if(WIN32)
  set(CPACK_GENERATOR "ZIP")
else()
  set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)

if(DAGE_BUILD_BENCHMARKS)
  if(DAGE_BUILD_SHARED)
    set(DAGE_BENCHMARK_LINKAGE "shared")
  else()
    set(DAGE_BENCHMARK_LINKAGE "static")
  endif()
  add_executable(dage_benchmarks benchmarks/dage_benchmarks.cpp)
  target_link_libraries(dage_benchmarks PRIVATE dage)
  target_compile_definitions(dage_benchmarks PRIVATE
    DAGE_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    DAGE_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    DAGE_BENCHMARK_LINKAGE="${DAGE_BENCHMARK_LINKAGE}")
  if(MSVC)
    target_compile_options(dage_benchmarks PRIVATE /W4 /WX)
    target_link_libraries(dage_benchmarks PRIVATE psapi)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(dage_benchmarks PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()

if(DAGE_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  find_program(DAGE_OPENSSL_CLI NAMES openssl)
  if(Python3_Interpreter_FOUND AND DAGE_OPENSSL_CLI)
    add_test(NAME dage_release_manifest_test
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/release_manifest_test.py
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/release_manifest.py
        ${DAGE_OPENSSL_CLI})
    set_tests_properties(dage_release_manifest_test PROPERTIES
      LABELS "release"
      TIMEOUT 30)
  endif()
  enable_testing()
  if(DAGE_BUILD_BENCHMARKS)
    add_test(NAME dage_benchmarks_smoke COMMAND dage_benchmarks --smoke --json)
    set_tests_properties(dage_benchmarks_smoke PROPERTIES
      LABELS "benchmark"
      TIMEOUT 60)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
      add_test(NAME dage_benchmark_budget_tool_test
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/benchmark_budget.py"
                --self-test)
      set_tests_properties(dage_benchmark_budget_tool_test PROPERTIES
        LABELS "benchmark"
        TIMEOUT 30)
    endif()
  endif()
  add_executable(dage_tests tests/test_dage.cpp)
  target_compile_definitions(dage_tests PRIVATE DAGE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(dage_tests PRIVATE dage ${DAGE_OPENSSL_TARGET})
  if(DAGE_BUILD_BUNDLE_TOOLS)
    target_link_libraries(dage_tests PRIVATE dage_bundle_tools ${DAGE_ZLIB_TARGET})
    target_compile_definitions(dage_tests PRIVATE DAGE_HAS_BUNDLE_TOOLS=1)
  endif()
  if(DAGE_BUILD_OPENTELEMETRY_BRIDGE)
    target_link_libraries(dage_tests PRIVATE dage_opentelemetry)
    target_compile_definitions(dage_tests PRIVATE DAGE_TEST_OPENTELEMETRY_BRIDGE=1)
  endif()
  set(DAGE_CORE_TEST_SUITES
    infrastructure
    bundle_ir
    resolver
    value
    validation
    all_node_format
    runtime
    error_retry_policy
    effect_replay
    reliability_recovery
    state_store_and_async
    observable_kernel
    trace_pipeline_and_lease
    timeout_cancel
    human_checkpoint
    patch
    loop_and_export
    parallel)
  if(DAGE_BUILD_BUNDLE_TOOLS)
    list(APPEND DAGE_CORE_TEST_SUITES
      bundle_tool_provider
      bundle_golden_vector
      bundle_store)
  endif()
  foreach(DAGE_CORE_TEST_SUITE IN LISTS DAGE_CORE_TEST_SUITES)
    set(DAGE_CORE_TEST_NAME "dage_core_${DAGE_CORE_TEST_SUITE}")
    add_test(NAME "${DAGE_CORE_TEST_NAME}" COMMAND dage_tests "${DAGE_CORE_TEST_SUITE}")
    set_tests_properties("${DAGE_CORE_TEST_NAME}" PROPERTIES
      LABELS "core"
      TIMEOUT 60)
  endforeach()
  add_executable(dage_c_header_test tests/c_header_test.c)
  target_link_libraries(dage_c_header_test PRIVATE dage)
  add_test(NAME dage_c_header_test COMMAND dage_c_header_test)
  add_executable(dage_c_async_test tests/c_async_test.cpp)
  target_link_libraries(dage_c_async_test PRIVATE dage)
  add_test(NAME dage_c_async_test COMMAND dage_c_async_test)
  add_executable(dage_c_spi_test tests/c_spi_test.cpp)
  target_link_libraries(dage_c_spi_test PRIVATE dage)
  add_test(NAME dage_c_spi_test COMMAND dage_c_spi_test)
  add_executable(dage_c_resolver_test tests/c_resolver_test.cpp)
  target_compile_definitions(dage_c_resolver_test PRIVATE DAGE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(dage_c_resolver_test PRIVATE dage)
  add_test(NAME dage_c_resolver_test COMMAND dage_c_resolver_test)
  add_executable(dage_c_analysis_test tests/c_analysis_test.cpp)
  target_link_libraries(dage_c_analysis_test PRIVATE dage)
  add_test(NAME dage_c_analysis_test COMMAND dage_c_analysis_test)
  add_executable(dage_recovery_stress tests/recovery_stress.cpp)
  target_link_libraries(dage_recovery_stress PRIVATE dage)
  add_test(NAME dage_recovery_stress COMMAND dage_recovery_stress)
  add_executable(dage_property_dag_test tests/property_dag_test.cpp)
  target_link_libraries(dage_property_dag_test PRIVATE dage)
  add_test(NAME dage_property_dag_test COMMAND dage_property_dag_test)
  set_tests_properties(dage_property_dag_test PROPERTIES LABELS "property" TIMEOUT 60)
  add_executable(dage_concurrency_soak tests/concurrency_soak.cpp)
  target_link_libraries(dage_concurrency_soak PRIVATE dage)
  add_test(NAME dage_concurrency_soak
    COMMAND dage_concurrency_soak --threads 4 --iterations 50)
  set_tests_properties(dage_concurrency_soak PROPERTIES LABELS "concurrency" TIMEOUT 60)
  add_executable(dage_failure_matrix tests/failure_matrix.cpp)
  target_link_libraries(dage_failure_matrix PRIVATE dage)
  add_test(NAME dage_failure_matrix COMMAND dage_failure_matrix)
  set_tests_properties(dage_failure_matrix PROPERTIES LABELS "failure" TIMEOUT 90)
  foreach(DAGE_STRICT_TEST_TARGET IN ITEMS
      dage_property_dag_test dage_concurrency_soak dage_failure_matrix)
    if(MSVC)
      target_compile_options(${DAGE_STRICT_TEST_TARGET} PRIVATE /W4 /WX)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${DAGE_STRICT_TEST_TARGET} PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    endif()
  endforeach()
  if(DAGE_BUILD_SHARED)
    add_executable(dage_abi_v1_layout_64 tests/abi/v1/layout_64.c)
    target_include_directories(dage_abi_v1_layout_64 PRIVATE include)
    set_target_properties(dage_abi_v1_layout_64 PROPERTIES
      C_STANDARD 99
      C_STANDARD_REQUIRED YES
      C_EXTENSIONS NO)
    add_test(NAME dage_abi_v1_layout_64 COMMAND dage_abi_v1_layout_64)
    set_tests_properties(dage_abi_v1_layout_64 PROPERTIES
      LABELS "abi"
      TIMEOUT 30)
    add_executable(dage_abi_export_audit tests/abi/export_audit.c)
    set_target_properties(dage_abi_export_audit PROPERTIES
      C_STANDARD 99
      C_STANDARD_REQUIRED YES
      C_EXTENSIONS NO)
    if(NOT WIN32)
      target_link_libraries(dage_abi_export_audit PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME dage_abi_v1_export_manifest
      COMMAND dage_abi_export_audit
        $<TARGET_FILE:dage>
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/abi/v1/exports.txt)
    set_tests_properties(dage_abi_v1_export_manifest PROPERTIES
      LABELS "abi"
      TIMEOUT 30)
    add_library(dage_abi_v1_stub SHARED tests/abi/v1/stub.c)
    target_include_directories(dage_abi_v1_stub PRIVATE tests/abi/v1)
    set_target_properties(dage_abi_v1_stub PROPERTIES
      C_STANDARD 99
      C_STANDARD_REQUIRED YES
      C_EXTENSIONS NO
      OUTPUT_NAME dage
      VERSION ${PROJECT_VERSION}
      SOVERSION 0
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/abi/v1/stub"
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/abi/v1/stub"
      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/abi/v1/stub")
    if(WIN32)
      set_target_properties(dage_abi_v1_stub PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    endif()
    add_executable(dage_abi_v1_old_binary tests/abi/v1/consumer.c)
    target_include_directories(dage_abi_v1_old_binary PRIVATE tests/abi/v1)
    target_link_libraries(dage_abi_v1_old_binary PRIVATE dage_abi_v1_stub)
    set_target_properties(dage_abi_v1_old_binary PROPERTIES
      C_STANDARD 99
      C_STANDARD_REQUIRED YES
      C_EXTENSIONS NO
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/abi/v1/consumer")
    add_executable(dage_abi_v1_old_header tests/abi/v1/consumer.c)
    target_include_directories(dage_abi_v1_old_header PRIVATE tests/abi/v1)
    target_link_libraries(dage_abi_v1_old_header PRIVATE dage)
    set_target_properties(dage_abi_v1_old_header PROPERTIES
      C_STANDARD 99
      C_STANDARD_REQUIRED YES
      C_EXTENSIONS NO)
    add_test(NAME dage_abi_v1_old_header_new_library COMMAND dage_abi_v1_old_header)
    add_test(NAME dage_abi_v1_old_binary_new_library
      COMMAND ${CMAKE_COMMAND}
        -DOLD_BINARY=$<TARGET_FILE:dage_abi_v1_old_binary>
        -DBASELINE_LIBRARY=$<TARGET_FILE:dage_abi_v1_stub>
        -DNEW_LIBRARY=$<TARGET_FILE:dage>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/abi/run_old_binary.cmake)
    set_tests_properties(
      dage_abi_v1_old_header_new_library
      dage_abi_v1_old_binary_new_library
      PROPERTIES LABELS "abi" TIMEOUT 30)
  endif()
endif()
