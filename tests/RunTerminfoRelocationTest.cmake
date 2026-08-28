foreach(
    required_variable
    BUILD_DIR
    STAGE_DIR
    INSTALL_BINDIR
    INSTALL_TERMINFO_DIR
    INSTALL_SHELL_INTEGRATION_DIR
    INSTALL_THEMES_DIR
    SHELL_INTEGRATION_VALIDATE_SCRIPT
    PROBE
    CONFIG_HELPER_NAME
    CONFIG
)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "Missing required variable ${required_variable}")
    endif()
endforeach()

set(original_prefix "${STAGE_DIR}/original")
set(relocated_prefix "${STAGE_DIR}/relocated")
file(REMOVE_RECURSE "${STAGE_DIR}")

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix
        "${original_prefix}" --config "${CONFIG}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0)
    message(
        FATAL_ERROR
        "Staged install failed (${install_result})\n${install_output}\n${install_error}"
    )
endif()

# The probe is test-only: copy it beside the installed application, but do not
# include it in the project's production install rules.
set(original_bindir "${original_prefix}/${INSTALL_BINDIR}")
file(MAKE_DIRECTORY "${original_bindir}")
file(COPY "${PROBE}" DESTINATION "${original_bindir}")
get_filename_component(probe_name "${PROBE}" NAME)

file(RENAME "${original_prefix}" "${relocated_prefix}" RESULT rename_result)
if(rename_result)
    message(FATAL_ERROR "Unable to relocate staged prefix: ${rename_result}")
endif()

set(relocated_shell_integration
    "${relocated_prefix}/${INSTALL_SHELL_INTEGRATION_DIR}"
)
execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSHELL_INTEGRATION_DIR=${relocated_shell_integration}" -P
        "${SHELL_INTEGRATION_VALIDATE_SCRIPT}"
    RESULT_VARIABLE shell_integration_result
    OUTPUT_VARIABLE shell_integration_output
    ERROR_VARIABLE shell_integration_error
)
if(NOT shell_integration_result EQUAL 0)
    message(
        FATAL_ERROR
        "Relocated shell integration is invalid "
        "(${shell_integration_result})\n"
        "${shell_integration_output}\n${shell_integration_error}"
    )
endif()

if(CONFIG_HELPER_NAME)
    set(relocated_themes "${relocated_prefix}/${INSTALL_THEMES_DIR}")
    file(
        GLOB relocated_theme_files
        LIST_DIRECTORIES FALSE
        "${relocated_themes}/*"
    )
    list(LENGTH relocated_theme_files relocated_theme_count)
    if(
        NOT relocated_theme_count EQUAL 602
        OR NOT EXISTS "${relocated_themes}/3024 Day"
        OR NOT EXISTS "${relocated_themes}/3024 Night"
        OR NOT EXISTS "${relocated_themes}/Dracula"
    )
        message(
            FATAL_ERROR
            "Relocated pinned theme inventory is incomplete: "
            "expected 602 files, found ${relocated_theme_count}"
        )
    endif()

    set(relocated_application
        "${relocated_prefix}/${INSTALL_BINDIR}/ghostty-qt"
    )
    set(relocated_config_helper
        "${relocated_prefix}/${INSTALL_BINDIR}/${CONFIG_HELPER_NAME}"
    )
    if(NOT EXISTS "${relocated_application}")
        message(
            FATAL_ERROR
            "Installed application is missing: ${relocated_application}"
        )
    elseif(NOT EXISTS "${relocated_config_helper}")
        message(
            FATAL_ERROR
            "Installed config helper is missing: ${relocated_config_helper}"
        )
    endif()

    set(relocated_config_home "${STAGE_DIR}/config-helper-home")
    file(MAKE_DIRECTORY "${relocated_config_home}")
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH
            "XDG_CONFIG_HOME=${relocated_config_home}"
            "${relocated_config_helper}" +validate-config
        RESULT_VARIABLE config_helper_result
        OUTPUT_VARIABLE config_helper_output
        ERROR_VARIABLE config_helper_error
    )
    if(NOT config_helper_result EQUAL 0)
        message(
            FATAL_ERROR
            "Relocated config helper failed (${config_helper_result})\n"
            "${config_helper_output}\n${config_helper_error}"
        )
    endif()

    # The application is not a self-contained Qt bundle. Preserve the SDK's
    # runtime search path while the invalid QPA name verifies that CLI
    # delegation still happens before QApplication construction.
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH --unset=DISPLAY
            --unset=WAYLAND_DISPLAY
            "QT_QPA_PLATFORM=ghostty-cli-relocation-must-not-load-qt"
            "XDG_CONFIG_HOME=${relocated_config_home}"
            "${relocated_config_helper}" +help
        RESULT_VARIABLE direct_help_result
        OUTPUT_VARIABLE direct_help_output
        ERROR_VARIABLE direct_help_error
    )
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env --unset=DISPLAY --unset=WAYLAND_DISPLAY
            "QT_QPA_PLATFORM=ghostty-cli-relocation-must-not-load-qt"
            "XDG_CONFIG_HOME=${relocated_config_home}"
            "${relocated_application}" +help
        RESULT_VARIABLE delegated_help_result
        OUTPUT_VARIABLE delegated_help_output
        ERROR_VARIABLE delegated_help_error
    )
    if(
        NOT direct_help_result EQUAL 0
        OR NOT delegated_help_result EQUAL direct_help_result
        OR NOT delegated_help_output STREQUAL direct_help_output
        OR NOT delegated_help_error STREQUAL direct_help_error
        OR NOT delegated_help_output MATCHES "Available actions:"
    )
        message(
            FATAL_ERROR
            "Relocated CLI delegation differs from its sibling helper\n"
            "helper (${direct_help_result}):\n${direct_help_output}\n"
            "${direct_help_error}\n"
            "application (${delegated_help_result}):\n"
            "${delegated_help_output}\n${delegated_help_error}"
        )
    endif()

    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH --unset=DISPLAY
            --unset=GHOSTTY_RESOURCES_DIR --unset=WAYLAND_DISPLAY
            "QT_QPA_PLATFORM=ghostty-cli-relocation-must-not-load-qt"
            "XDG_CONFIG_HOME=${relocated_config_home}"
            "${relocated_config_helper}" +list-themes --plain
        RESULT_VARIABLE direct_themes_result
        OUTPUT_VARIABLE direct_themes_output
        ERROR_VARIABLE direct_themes_error
    )
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env --unset=DISPLAY
            --unset=GHOSTTY_RESOURCES_DIR --unset=WAYLAND_DISPLAY
            "QT_QPA_PLATFORM=ghostty-cli-relocation-must-not-load-qt"
            "XDG_CONFIG_HOME=${relocated_config_home}"
            "${relocated_application}" +list-themes --plain
        RESULT_VARIABLE delegated_themes_result
        OUTPUT_VARIABLE delegated_themes_output
        ERROR_VARIABLE delegated_themes_error
    )
    if(
        NOT direct_themes_result EQUAL 0
        OR NOT delegated_themes_result EQUAL direct_themes_result
        OR NOT delegated_themes_output STREQUAL direct_themes_output
        OR NOT delegated_themes_error STREQUAL direct_themes_error
        OR NOT delegated_themes_output MATCHES "Dracula \\(resources\\)"
    )
        message(
            FATAL_ERROR
            "Relocated theme CLI delegation differs from its sibling helper\n"
            "helper (${direct_themes_result}):\n${direct_themes_output}\n"
            "${direct_themes_error}\n"
            "application (${delegated_themes_result}):\n"
            "${delegated_themes_output}\n${delegated_themes_error}"
        )
    endif()

    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH
            "XDG_CONFIG_HOME=${relocated_config_home}"
            "${relocated_config_helper}" +show-config-json
            --ghostty-qt-color-scheme=light
        RESULT_VARIABLE structured_helper_result
        OUTPUT_VARIABLE structured_helper_output
        ERROR_VARIABLE structured_helper_error
    )
    if(NOT structured_helper_result EQUAL 0)
        message(
            FATAL_ERROR
            "Relocated structured config export failed "
            "(${structured_helper_result})\n${structured_helper_output}\n"
            "${structured_helper_error}"
        )
    endif()
    string(
        JSON structured_schema
        ERROR_VARIABLE structured_json_error
        GET "${structured_helper_output}"
        version
    )
    string(
        JSON values_type
        ERROR_VARIABLE values_json_error
        TYPE "${structured_helper_output}"
        values
    )
    string(
        JSON lifetime_type
        ERROR_VARIABLE lifetime_json_error
        TYPE "${structured_helper_output}"
        values
        quit-after-last-window-closed
    )
    string(
        JSON keybind_root_type
        ERROR_VARIABLE keybind_json_error
        TYPE "${structured_helper_output}"
        keybindings
        root
    )
    string(
        JSON default_keybind_root_type
        ERROR_VARIABLE default_keybind_json_error
        TYPE "${structured_helper_output}"
        default-keybindings
        root
    )
    string(
        JSON single_instance
        ERROR_VARIABLE single_instance_json_error
        GET "${structured_helper_output}"
        values
        gtk-single-instance
    )
    string(
        JSON initial_window_type
        ERROR_VARIABLE initial_window_json_error
        TYPE "${structured_helper_output}"
        values
        initial-window
    )
    string(
        JSON initial_window
        ERROR_VARIABLE initial_window_value_json_error
        GET "${structured_helper_output}"
        values
        initial-window
    )
    string(
        JSON desktop_notifications_type
        ERROR_VARIABLE desktop_notifications_json_error
        TYPE "${structured_helper_output}"
        values
        desktop-notifications
    )
    string(
        JSON desktop_notifications
        ERROR_VARIABLE desktop_notifications_value_json_error
        GET "${structured_helper_output}"
        values
        desktop-notifications
    )
    string(
        JSON progress_style_type
        ERROR_VARIABLE progress_style_json_error
        TYPE "${structured_helper_output}"
        values
        progress-style
    )
    string(
        JSON progress_style
        ERROR_VARIABLE progress_style_value_json_error
        GET "${structured_helper_output}"
        values
        progress-style
    )
    string(
        JSON palette_length
        ERROR_VARIABLE palette_json_error
        LENGTH "${structured_helper_output}"
        values
        palette
    )
    if(
        structured_json_error
        OR values_json_error
        OR lifetime_json_error
        OR keybind_json_error
        OR default_keybind_json_error
        OR single_instance_json_error
        OR initial_window_json_error
        OR initial_window_value_json_error
        OR desktop_notifications_json_error
        OR desktop_notifications_value_json_error
        OR progress_style_json_error
        OR progress_style_value_json_error
        OR palette_json_error
        OR NOT structured_schema EQUAL 7
        OR NOT values_type STREQUAL "OBJECT"
        OR NOT lifetime_type STREQUAL "BOOLEAN"
        OR NOT initial_window_type STREQUAL "BOOLEAN"
        OR NOT initial_window
        OR NOT desktop_notifications_type STREQUAL "BOOLEAN"
        OR NOT desktop_notifications
        OR NOT progress_style_type STREQUAL "BOOLEAN"
        OR NOT progress_style
        OR NOT single_instance STREQUAL "detect"
        OR NOT keybind_root_type STREQUAL "ARRAY"
        OR NOT default_keybind_root_type STREQUAL "ARRAY"
        OR NOT palette_length EQUAL 256
    )
        message(
            FATAL_ERROR
            "Relocated helper returned invalid structured config JSON: "
            "${structured_json_error};${values_json_error};"
            "${lifetime_json_error};${keybind_json_error};"
            "${default_keybind_json_error};${single_instance_json_error};"
            "${initial_window_json_error};${initial_window_value_json_error};"
            "${desktop_notifications_json_error};"
            "${desktop_notifications_value_json_error};"
            "${progress_style_json_error};${progress_style_value_json_error};"
            "${palette_json_error}\n"
            "${structured_helper_output}"
        )
    endif()
endif()

set(relocated_probe "${relocated_prefix}/${INSTALL_BINDIR}/${probe_name}")
set(expected_database "${relocated_prefix}/${INSTALL_TERMINFO_DIR}")
file(REAL_PATH "${expected_database}" expected_database_real)

find_program(terminfo_infocmp infocmp REQUIRED)
execute_process(
    COMMAND
        "${terminfo_infocmp}" -x -1 -A "${expected_database_real}" xterm-ghostty
    RESULT_VARIABLE terminfo_result
    OUTPUT_VARIABLE terminfo_output
    ERROR_VARIABLE terminfo_error
)
string(FIND "${terminfo_output}" "Smol=\\E[53m" smol_offset)
string(FIND "${terminfo_output}" "Rmol=\\E[55m" rmol_offset)
if(NOT terminfo_result EQUAL 0 OR smol_offset EQUAL -1 OR rmol_offset EQUAL -1)
    message(
        FATAL_ERROR
        "Relocated xterm-ghostty terminfo is missing overline capabilities "
        "(${terminfo_result}):\n${terminfo_output}\n${terminfo_error}"
    )
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env --unset=GHOSTTY_QT_TERMINFO
        "${relocated_probe}"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT probe_result EQUAL 0)
    message(
        FATAL_ERROR
        "Relocated probe failed (${probe_result})\n${probe_output}\n${probe_error}"
    )
endif()
if(NOT probe_output STREQUAL expected_database_real)
    message(
        FATAL_ERROR
        "Relocated probe selected '${probe_output}', expected '${expected_database_real}'"
    )
endif()

# An explicit valid override must win even though the private installed
# database is present.
set(override_database "${STAGE_DIR}/override-database")
file(MAKE_DIRECTORY "${override_database}")
file(COPY "${expected_database}/" DESTINATION "${override_database}")
file(REAL_PATH "${override_database}" override_database_real)
execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env "GHOSTTY_QT_TERMINFO=${override_database}"
        "${relocated_probe}"
    RESULT_VARIABLE override_result
    OUTPUT_VARIABLE override_output
    ERROR_VARIABLE override_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(
    NOT override_result EQUAL 0
    OR NOT override_output STREQUAL override_database_real
)
    message(
        FATAL_ERROR
        "Explicit override was not selected (${override_result}): "
        "'${override_output}'\n${override_error}"
    )
endif()

# An explicit invalid override is authoritative and must not silently fall back
# to the installed database.
set(invalid_database "${STAGE_DIR}/invalid-database")
file(MAKE_DIRECTORY "${invalid_database}")
execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env "GHOSTTY_QT_TERMINFO=${invalid_database}"
        "${relocated_probe}"
    RESULT_VARIABLE invalid_result
    OUTPUT_VARIABLE invalid_output
    ERROR_VARIABLE invalid_error
)
if(invalid_result EQUAL 0)
    message(
        FATAL_ERROR
        "Invalid explicit override unexpectedly resolved to '${invalid_output}'"
    )
endif()
if(NOT invalid_error MATCHES "GHOSTTY_QT_TERMINFO=.*does not contain")
    message(
        FATAL_ERROR
        "Invalid override did not produce the expected diagnostic: ${invalid_error}"
    )
endif()
