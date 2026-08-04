if(NOT DEFINED OLD_BINARY OR NOT DEFINED BASELINE_LIBRARY OR NOT DEFINED NEW_LIBRARY)
  message(FATAL_ERROR "OLD_BINARY, BASELINE_LIBRARY and NEW_LIBRARY are required")
endif()

get_filename_component(old_binary_name "${OLD_BINARY}" NAME)
get_filename_component(baseline_library_name "${BASELINE_LIBRARY}" NAME)
get_filename_component(new_library_name "${NEW_LIBRARY}" NAME)
if(NOT baseline_library_name STREQUAL new_library_name)
  message(FATAL_ERROR
    "Baseline and current ABI libraries must have the same filename: "
    "${baseline_library_name} != ${new_library_name}")
endif()

set(stage_directory "${OLD_BINARY}.current-library")
file(MAKE_DIRECTORY "${stage_directory}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${OLD_BINARY}" "${stage_directory}/${old_binary_name}"
  RESULT_VARIABLE binary_copy_result)
if(NOT binary_copy_result EQUAL 0)
  message(FATAL_ERROR "Failed to stage the frozen ABI binary")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${NEW_LIBRARY}" "${stage_directory}/${new_library_name}"
  RESULT_VARIABLE library_copy_result)
if(NOT library_copy_result EQUAL 0)
  message(FATAL_ERROR "Failed to stage the current ABI library")
endif()

if(APPLE)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            "DYLD_LIBRARY_PATH=${stage_directory}"
            "${stage_directory}/${old_binary_name}"
    RESULT_VARIABLE run_result)
elseif(UNIX)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            "LD_LIBRARY_PATH=${stage_directory}"
            "${stage_directory}/${old_binary_name}"
    RESULT_VARIABLE run_result)
else()
  execute_process(
    COMMAND "${stage_directory}/${old_binary_name}"
    RESULT_VARIABLE run_result)
endif()
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "Frozen ABI binary failed against the current library: ${run_result}")
endif()
