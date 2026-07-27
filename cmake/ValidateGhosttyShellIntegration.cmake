if(NOT DEFINED SHELL_INTEGRATION_DIR)
    message(FATAL_ERROR "SHELL_INTEGRATION_DIR is required")
endif()

set(_ghostty_qt_expected_resources
    README.md
    bash/bash-preexec.sh
    bash/ghostty.bash
    elvish/lib/ghostty-integration.elv
    fish/vendor_conf.d/ghostty-shell-integration.fish
    nushell/vendor/autoload/ghostty.nu
    zsh/.zshenv
    zsh/ghostty-integration
)
foreach(_ghostty_qt_resource IN LISTS _ghostty_qt_expected_resources)
    if(NOT EXISTS "${SHELL_INTEGRATION_DIR}/${_ghostty_qt_resource}")
        message(FATAL_ERROR
            "Staged shell integration is missing ${_ghostty_qt_resource}")
    endif()
endforeach()

file(GLOB_RECURSE _ghostty_qt_all_resources
    RELATIVE "${SHELL_INTEGRATION_DIR}"
    LIST_DIRECTORIES FALSE
    "${SHELL_INTEGRATION_DIR}/*")
list(SORT _ghostty_qt_all_resources)
list(SORT _ghostty_qt_expected_resources)
if(NOT _ghostty_qt_all_resources STREQUAL _ghostty_qt_expected_resources)
    message(FATAL_ERROR
        "Staged shell-integration inventory differs from the reviewed pinned "
        "set.\nExpected: ${_ghostty_qt_expected_resources}\n"
        "Actual: ${_ghostty_qt_all_resources}")
endif()

set(_ghostty_qt_patched_resources
    bash/ghostty.bash
    elvish/lib/ghostty-integration.elv
    fish/vendor_conf.d/ghostty-shell-integration.fish
    nushell/vendor/autoload/ghostty.nu
    zsh/ghostty-integration
)
foreach(_ghostty_qt_resource IN LISTS _ghostty_qt_patched_resources)
    file(READ
        "${SHELL_INTEGRATION_DIR}/${_ghostty_qt_resource}"
        _ghostty_qt_resource_contents)
    string(FIND "${_ghostty_qt_resource_contents}"
        "ghostty-qt +ssh" _ghostty_qt_comment_position)
    if(_ghostty_qt_comment_position EQUAL -1)
        message(FATAL_ERROR
            "${_ghostty_qt_resource} does not describe its ghostty-qt +ssh wrapper")
    endif()
endforeach()

set(_ghostty_qt_expected_executable_references
    "bash/ghostty.bash|GHOSTTY_BIN_DIR/ghostty-qt"
    "elvish/lib/ghostty-integration.elv|GHOSTTY_BIN_DIR/\"ghostty-qt\""
    "fish/vendor_conf.d/ghostty-shell-integration.fish|GHOSTTY_BIN_DIR/ghostty-qt"
    "nushell/vendor/autoload/ghostty.nu|path join \"ghostty-qt\""
    "zsh/ghostty-integration|GHOSTTY_BIN_DIR/ghostty-qt"
)
foreach(_ghostty_qt_reference IN LISTS _ghostty_qt_expected_executable_references)
    string(REPLACE "|" ";" _ghostty_qt_reference_parts
        "${_ghostty_qt_reference}")
    list(GET _ghostty_qt_reference_parts 0 _ghostty_qt_resource)
    list(GET _ghostty_qt_reference_parts 1 _ghostty_qt_expected_reference)
    file(READ
        "${SHELL_INTEGRATION_DIR}/${_ghostty_qt_resource}"
        _ghostty_qt_resource_contents)
    string(FIND "${_ghostty_qt_resource_contents}"
        "${_ghostty_qt_expected_reference}" _ghostty_qt_reference_position)
    if(_ghostty_qt_reference_position EQUAL -1)
        message(FATAL_ERROR
            "${_ghostty_qt_resource} does not call the ghostty-qt executable")
    endif()
endforeach()

foreach(_ghostty_qt_resource IN LISTS _ghostty_qt_all_resources)
    file(READ "${SHELL_INTEGRATION_DIR}/${_ghostty_qt_resource}"
        _ghostty_qt_resource_contents)
    if(_ghostty_qt_resource_contents MATCHES
       "GHOSTTY_BIN_DIR[/\"]+ghostty([\" \r\n]|$)")
        message(FATAL_ERROR
            "${_ghostty_qt_resource} still calls the upstream ghostty executable")
    endif()
    if(_ghostty_qt_resource_contents MATCHES
       "path join \"ghostty\"")
        message(FATAL_ERROR
            "${_ghostty_qt_resource} still calls the upstream ghostty executable")
    endif()
endforeach()
