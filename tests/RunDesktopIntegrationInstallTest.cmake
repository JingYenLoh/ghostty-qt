foreach(required_variable
        BUILD_DIR STAGE_DIR REPOSITORY_TMP_DIR INSTALL_BINDIR INSTALL_DATADIR
        APPLICATION_ID EXPECT_CONFIG_HELPER CONFIG)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "Missing required variable ${required_variable}")
    endif()
endforeach()

set(staging_root "${STAGE_DIR}/root")
string(ASCII 34 prefix_double_quote)
string(ASCII 36 prefix_dollar)
string(ASCII 96 prefix_backtick)
# Exercise the characters whose Desktop Entry Exec encoding differs from an
# ordinary string. A literal backslash cannot be covered through `cmake
# --install`: CMake rejects it in CMAKE_INSTALL_PREFIX before our metadata
# patch runs.
set(prefix
    "${STAGE_DIR}/ghostty qt-%-${prefix_dollar}-tick${prefix_backtick}-quote${prefix_double_quote}/${CONFIG}")
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
set(icon
    "${staging_root}${final_data_dir}/icons/hicolor/scalable/apps/${APPLICATION_ID}.svg")
set(metainfo
    "${staging_root}${final_data_dir}/metainfo/${APPLICATION_ID}.metainfo.xml")
foreach(required_path IN ITEMS
        "${executable}" "${desktop}" "${service}" "${icon}" "${metainfo}")
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR "Installed desktop integration is missing: ${required_path}")
    endif()
endforeach()

file(READ "${desktop}" desktop_contents)
file(READ "${service}" service_contents)
file(READ "${icon}" icon_contents)
file(READ "${metainfo}" metainfo_contents)

find_program(desktop_file_validate desktop-file-validate)
if(desktop_file_validate)
    execute_process(
        COMMAND "${desktop_file_validate}" "${desktop}"
        RESULT_VARIABLE desktop_validation_result
        OUTPUT_VARIABLE desktop_validation_output
        ERROR_VARIABLE desktop_validation_error)
    if(NOT desktop_validation_result EQUAL 0)
        message(FATAL_ERROR
            "desktop-file-validate rejected the installed entry (${desktop_validation_result})\n${desktop_validation_output}\n${desktop_validation_error}")
    endif()
endif()

find_program(appstreamcli appstreamcli)
if(appstreamcli)
    execute_process(
        COMMAND "${appstreamcli}" validate --no-net --strict "${metainfo}"
        RESULT_VARIABLE appstream_validation_result
        OUTPUT_VARIABLE appstream_validation_output
        ERROR_VARIABLE appstream_validation_error)
    if(NOT appstream_validation_result EQUAL 0)
        message(FATAL_ERROR
            "appstreamcli rejected the installed metadata (${appstream_validation_result})\n${appstream_validation_output}\n${appstream_validation_error}")
    endif()
endif()

find_program(xmllint xmllint)
if(xmllint)
    foreach(xml_file IN ITEMS "${icon}" "${metainfo}")
        execute_process(
            COMMAND "${xmllint}" --noout "${xml_file}"
            RESULT_VARIABLE xml_validation_result
            OUTPUT_VARIABLE xml_validation_output
            ERROR_VARIABLE xml_validation_error)
        if(NOT xml_validation_result EQUAL 0)
            message(FATAL_ERROR
                "xmllint rejected ${xml_file} (${xml_validation_result})\n${xml_validation_output}\n${xml_validation_error}")
        endif()
    endforeach()
endif()

find_program(rsvg_convert rsvg-convert)
if(rsvg_convert)
    foreach(icon_size IN ITEMS 16 32 64 512)
        set(rendered_icon "${STAGE_DIR}/icon-${icon_size}.png")
        execute_process(
            COMMAND "${rsvg_convert}"
                --width "${icon_size}" --height "${icon_size}"
                --output "${rendered_icon}" "${icon}"
            RESULT_VARIABLE icon_render_result
            OUTPUT_VARIABLE icon_render_output
            ERROR_VARIABLE icon_render_error)
        if(NOT icon_render_result EQUAL 0 OR NOT EXISTS "${rendered_icon}")
            message(FATAL_ERROR
                "Could not render installed icon at ${icon_size}px (${icon_render_result})\n${icon_render_output}\n${icon_render_error}")
        endif()
        file(SIZE "${rendered_icon}" rendered_icon_size)
        if(rendered_icon_size EQUAL 0)
            message(FATAL_ERROR
                "Installed icon rendered an empty ${icon_size}px PNG")
        endif()
    endforeach()
endif()

set(escaped_try_executable "${final_executable}")
string(REPLACE [[\]] [[\\]] escaped_try_executable
    "${escaped_try_executable}")

set(escaped_dbus_executable "${final_executable}")
string(REPLACE [[\]] [[\\\\]] escaped_dbus_executable
    "${escaped_dbus_executable}")
string(ASCII 34 desktop_double_quote)
string(ASCII 36 desktop_dollar)
string(ASCII 96 desktop_backtick)
foreach(quoted_character IN ITEMS
        "${desktop_double_quote}" "${desktop_dollar}" "${desktop_backtick}")
    string(CONCAT escaped_character [[\\]] "${quoted_character}")
    string(REPLACE "${quoted_character}" "${escaped_character}"
        escaped_dbus_executable "${escaped_dbus_executable}")
endforeach()
set(escaped_executable "${escaped_dbus_executable}")
string(REPLACE [[%]] [[%%]] escaped_executable "${escaped_executable}")
set(desktop_exec
    "Exec=\"${escaped_executable}\" --single-instance=true")
set(desktop_try_exec "TryExec=${escaped_try_executable}")
string(CONCAT desktop_new_window_action
    "[Desktop Action new-window]\n"
    "Name=New Window\n"
    "${desktop_exec}\n")
set(service_exec
    "Exec=\"${escaped_dbus_executable}\" --single-instance=true --initial-window=false")
string(FIND "${desktop_contents}" "${desktop_exec}\n"
    desktop_exec_position)
string(FIND "${desktop_contents}" "${desktop_try_exec}\n"
    desktop_try_exec_position)
string(FIND "${desktop_contents}" "Icon=${APPLICATION_ID}\n"
    desktop_icon_position)
string(FIND "${desktop_contents}" "${desktop_new_window_action}"
    desktop_new_window_action_position)
string(FIND "${service_contents}" "Name=${APPLICATION_ID}\n"
    service_name_position)
string(FIND "${service_contents}" "${service_exec}\n"
    service_exec_position)

if(NOT desktop_contents MATCHES "(^|\n)DBusActivatable=true(\n|$)"
   OR desktop_exec_position EQUAL -1
   OR desktop_exec_position GREATER desktop_new_window_action_position
   OR desktop_try_exec_position EQUAL -1
   OR desktop_icon_position EQUAL -1
   OR desktop_new_window_action_position EQUAL -1
   OR NOT desktop_contents MATCHES "(^|\n)Actions=new-window;(\n|$)"
   OR NOT desktop_contents MATCHES "(^|\n)X-TerminalArgExec=-e(\n|$)"
   OR NOT desktop_contents MATCHES "(^|\n)X-TerminalArgTitle=--title=(\n|$)"
   OR NOT desktop_contents MATCHES "(^|\n)X-TerminalArgAppId=--class=(\n|$)"
   OR NOT desktop_contents MATCHES "(^|\n)X-TerminalArgDir=--working-directory=(\n|$)"
   OR NOT desktop_contents MATCHES "(^|\n)X-TerminalArgHold=--wait-after-command(\n|$)"
   OR desktop_contents MATCHES "initial-window=false"
   OR desktop_contents MATCHES "(^|\n)MimeType="
   OR desktop_contents MATCHES "__GHOSTTY_QT_INSTALL_(TRY_|DBUS_)?EXECUTABLE__")
    message(FATAL_ERROR "Invalid installed desktop entry:\n${desktop_contents}")
endif()

string(FIND "${metainfo_contents}" "<id>${APPLICATION_ID}</id>"
    metainfo_id_position)
string(FIND "${metainfo_contents}"
    "<launchable type=\"desktop-id\">${APPLICATION_ID}.desktop</launchable>"
    metainfo_launchable_position)
if(metainfo_id_position EQUAL -1
   OR metainfo_launchable_position EQUAL -1
   OR metainfo_contents MATCHES "@GHOSTTY_QT_[A-Z_]+@"
   OR metainfo_contents MATCHES [[\$<]])
    message(FATAL_ERROR "Invalid installed AppStream metadata:\n${metainfo_contents}")
endif()

if(NOT icon_contents MATCHES "<svg"
   OR NOT icon_contents MATCHES "viewBox=\"0 0 512 512\""
   OR icon_contents MATCHES "<(image|script|foreignObject)([ >])"
   OR icon_contents MATCHES "(href|xlink:href)=")
    message(FATAL_ERROR "Invalid installed scalable icon:\n${icon_contents}")
endif()

if(service_name_position EQUAL -1
   OR service_exec_position EQUAL -1
   OR service_contents MATCHES "%%"
   OR service_contents MATCHES "SystemdService="
   OR service_contents MATCHES "__GHOSTTY_QT_INSTALL_(TRY_|DBUS_)?EXECUTABLE__")
    message(FATAL_ERROR "Invalid installed D-Bus service:\n${service_contents}")
endif()

# Exercise the installed service through the reference D-Bus daemon when its
# command-line client is available. The logical install prefix is kept inside
# STAGE_DIR so a symlink can make the DESTDIR-staged executable reachable at
# the exact path encoded in Exec.
find_program(dbus_daemon dbus-daemon)
find_program(gdbus gdbus)
find_program(kill_process kill)
if(dbus_daemon AND gdbus AND kill_process)
    cmake_path(GET prefix PARENT_PATH logical_prefix_parent)
    file(MAKE_DIRECTORY "${logical_prefix_parent}")
    file(CREATE_LINK "${staging_root}${prefix}" "${prefix}" SYMBOLIC
        RESULT prefix_link_result)
    if(prefix_link_result)
        message(FATAL_ERROR
            "Could not expose the staged prefix for D-Bus activation: ${prefix_link_result}")
    endif()

    set(activation_config "${STAGE_DIR}/activation-config")
    # Keep the AF_UNIX address well below its platform limit even though this
    # test intentionally exercises a long install prefix.
    string(SHA256 activation_runtime_id
        "${BUILD_DIR};${CONFIG};${EXPECT_CONFIG_HELPER}")
    string(SUBSTRING "${activation_runtime_id}" 0 12 activation_runtime_id)
    set(activation_runtime
        "${REPOSITORY_TMP_DIR}/dbus-${activation_runtime_id}")
    file(REMOVE_RECURSE "${activation_runtime}")
    file(MAKE_DIRECTORY
        "${activation_config}/ghostty"
        "${activation_config}/ghostty-qt"
        "${activation_runtime}")
    file(CHMOD "${activation_runtime}"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    file(WRITE "${activation_config}/ghostty/config.ghostty"
        "initial-window = true\nconfirm-close-surface = false\n")
    file(WRITE "${activation_config}/ghostty-qt/config"
        "single-instance = false\n")
    set(application_object_path "/${APPLICATION_ID}")
    string(REPLACE "." "/" application_object_path
        "${application_object_path}")
    string(REPLACE "-" "_" application_object_path
        "${application_object_path}")

    set(activation_address "unix:path=${activation_runtime}/bus")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            --unset=TERM_PROGRAM
            "XDG_DATA_HOME=${staging_root}${final_data_dir}"
            "XDG_CONFIG_HOME=${activation_config}"
            "XDG_RUNTIME_DIR=${activation_runtime}"
            "TMPDIR=${activation_runtime}"
            "GHOSTTY_QT_ALLOW_NON_WAYLAND=1"
            "GHOSTTY_QT_TEST_DESKTOP_ACTIVATION=1"
            "QT_QPA_PLATFORM=offscreen"
            "QT_QUICK_BACKEND=software"
            "${dbus_daemon}" --session
            "--address=${activation_address}"
            --fork --print-address=1 --print-pid=1
        RESULT_VARIABLE daemon_result
        OUTPUT_VARIABLE daemon_output
        ERROR_VARIABLE daemon_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        TIMEOUT 5)
    if(NOT daemon_result EQUAL 0)
        message(FATAL_ERROR
            "Could not start the private D-Bus daemon (${daemon_result})\n${daemon_output}\n${daemon_error}")
    endif()
    string(REPLACE "\n" ";" daemon_lines "${daemon_output}")
    set(daemon_pid "")
    foreach(daemon_line IN LISTS daemon_lines)
        if(daemon_line MATCHES "^[0-9]+$")
            set(daemon_pid "${daemon_line}")
        endif()
    endforeach()
    if(NOT daemon_pid)
        message(FATAL_ERROR
            "Private D-Bus daemon did not report its PID: ${daemon_output}")
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "DBUS_SESSION_BUS_ADDRESS=${activation_address}"
            "${gdbus}" call --session
            --dest "${APPLICATION_ID}"
            --object-path "${application_object_path}"
            --method org.freedesktop.Application.Activate "{}"
        RESULT_VARIABLE activation_result
        OUTPUT_VARIABLE activation_output
        ERROR_VARIABLE activation_error
        TIMEOUT 15)
    execute_process(COMMAND "${kill_process}" "${daemon_pid}")
    file(REMOVE_RECURSE "${activation_runtime}")
    if(NOT activation_result EQUAL 0)
        message(FATAL_ERROR
            "Installed D-Bus service activation failed (${activation_result})\n${activation_output}\n${activation_error}\n${service_contents}")
    endif()
endif()

set(config_helper "${staging_root}${final_binary_dir}/ghostty-qt-config-helper")
if(EXPECT_CONFIG_HELPER AND NOT EXISTS "${config_helper}")
    message(FATAL_ERROR "Config-enabled install omitted ${config_helper}")
elseif(NOT EXPECT_CONFIG_HELPER AND EXISTS "${config_helper}")
    message(FATAL_ERROR "Config-disabled install unexpectedly contains ${config_helper}")
endif()
