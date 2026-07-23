foreach(required_variable
        BUILD_DIR STAGE_DIR INSTALL_BINDIR INSTALL_DATADIR APPLICATION_ID
        EXPECT_CONFIG_HELPER CONFIG)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "Missing required variable ${required_variable}")
    endif()
endforeach()

set(staging_root "${STAGE_DIR}/root")
set(prefix "/ghostty-qt-test-prefix/${CONFIG}")
file(REMOVE_RECURSE "${STAGE_DIR}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "DESTDIR=${staging_root}"
        "${CMAKE_COMMAND}" --install "${BUILD_DIR}"
        --prefix "${prefix}" --config "${CONFIG}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "Staged install failed (${install_result})\n${install_output}\n${install_error}")
endif()

function(resolve_final_install_path output directory)
    if(IS_ABSOLUTE "${directory}")
        set(path "${directory}")
    else()
        set(path "${prefix}/${directory}")
    endif()
    cmake_path(NORMAL_PATH path)
    set(${output} "${path}" PARENT_SCOPE)
endfunction()

resolve_final_install_path(final_binary_dir "${INSTALL_BINDIR}")
resolve_final_install_path(final_data_dir "${INSTALL_DATADIR}")
set(final_executable "${final_binary_dir}/ghostty-qt")
set(executable "${staging_root}${final_executable}")
set(desktop
    "${staging_root}${final_data_dir}/applications/${APPLICATION_ID}.desktop")
set(service
    "${staging_root}${final_data_dir}/dbus-1/services/${APPLICATION_ID}.service")
foreach(required_path IN ITEMS "${executable}" "${desktop}" "${service}")
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR "Installed desktop integration is missing: ${required_path}")
    endif()
endforeach()

file(READ "${desktop}" desktop_contents)
file(READ "${service}" service_contents)
string(REPLACE "\\" "\\\\" escaped_executable "${final_executable}")
string(REPLACE "\"" "\\\"" escaped_executable "${escaped_executable}")
set(desktop_exec
    "Exec=\"${escaped_executable}\" --single-instance=true")
set(service_exec
    "Exec=\"${escaped_executable}\" --single-instance=true --initial-window=false")
string(FIND "${desktop_contents}" "${desktop_exec}\n"
    desktop_exec_position)
string(FIND "${service_contents}" "Name=${APPLICATION_ID}\n"
    service_name_position)
string(FIND "${service_contents}" "${service_exec}\n"
    service_exec_position)

if(NOT desktop_contents MATCHES "(^|\n)DBusActivatable=true(\n|$)"
   OR desktop_exec_position EQUAL -1
   OR desktop_contents MATCHES "initial-window=false"
   OR desktop_contents MATCHES "(^|\n)(Actions|MimeType|Icon)="
   OR desktop_contents MATCHES "__GHOSTTY_QT_INSTALL_EXECUTABLE__")
    message(FATAL_ERROR "Invalid installed desktop entry:\n${desktop_contents}")
endif()

if(service_name_position EQUAL -1
   OR service_exec_position EQUAL -1
   OR service_contents MATCHES "SystemdService="
   OR service_contents MATCHES "__GHOSTTY_QT_INSTALL_EXECUTABLE__")
    message(FATAL_ERROR "Invalid installed D-Bus service:\n${service_contents}")
endif()

set(config_helper "${staging_root}${final_binary_dir}/ghostty-qt-config-helper")
if(EXPECT_CONFIG_HELPER AND NOT EXISTS "${config_helper}")
    message(FATAL_ERROR "Config-enabled install omitted ${config_helper}")
elseif(NOT EXPECT_CONFIG_HELPER AND EXISTS "${config_helper}")
    message(FATAL_ERROR "Config-disabled install unexpectedly contains ${config_helper}")
endif()
