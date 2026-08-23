include_guard(GLOBAL)

# Generate desktop integration metadata and define installation rules.

if(GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG)
    set(_ghostty_qt_config_install_dir "${CMAKE_INSTALL_LIBDIR}/ghostty-qt")
    cmake_path(
        ABSOLUTE_PATH _ghostty_qt_config_install_dir
        BASE_DIRECTORY "${CMAKE_INSTALL_PREFIX}"
        NORMALIZE
        OUTPUT_VARIABLE _ghostty_qt_config_install_dir_absolute
    )
    cmake_path(
        RELATIVE_PATH _ghostty_qt_config_install_dir_absolute
        BASE_DIRECTORY "${_ghostty_qt_install_bindir_absolute}"
        OUTPUT_VARIABLE _ghostty_qt_config_install_relative_dir
    )
    if(IS_ABSOLUTE "${CMAKE_INSTALL_BINDIR}")
        set(_ghostty_qt_installed_helper
            "\$ENV{DESTDIR}${CMAKE_INSTALL_BINDIR}/ghostty-qt-config-helper"
        )
    else()
        set(_ghostty_qt_installed_helper
            "\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/ghostty-qt-config-helper"
        )
    endif()
    # Force a fresh source binary on repeated installs before applying the
    # explicit RPATH rewrite below.
    install(CODE "file(REMOVE \"${_ghostty_qt_installed_helper}\")")
    install(
        TARGETS ghostty-qt-config-helper
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
    install(
        FILES "${GHOSTTY_QT_CONFIG_RUNTIME_LIBRARY}"
        DESTINATION "${_ghostty_qt_config_install_dir}"
    )
    install(
        DIRECTORY "${GHOSTTY_QT_THEMES_STAGE_DIR}/"
        DESTINATION "${GHOSTTY_QT_INSTALL_THEMES_DIR}"
    )
    string(
        CONCAT _ghostty_qt_install_rpath_code
        "file(RPATH_CHANGE\n"
        "  FILE \"${_ghostty_qt_installed_helper}\"\n"
        "  OLD_RPATH \"${GHOSTTY_QT_CONFIG_PREFIX}/lib\"\n"
        "  NEW_RPATH \"$ORIGIN/${_ghostty_qt_config_install_relative_dir}\")"
    )
    install(CODE "${_ghostty_qt_install_rpath_code}")
endif()

set(_ghostty_qt_desktop_build_dir
    "${CMAKE_CURRENT_BINARY_DIR}/desktop-integration"
)
file(MAKE_DIRECTORY "${_ghostty_qt_desktop_build_dir}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/linux/ghostty-qt.desktop.in"
    "${_ghostty_qt_desktop_build_dir}/ghostty-qt.desktop.in"
    @ONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/linux/ghostty-qt.service.in"
    "${_ghostty_qt_desktop_build_dir}/ghostty-qt.service.in"
    @ONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/linux/ghostty-qt-systemd.service.in"
    "${_ghostty_qt_desktop_build_dir}/ghostty-qt-systemd.service.in"
    @ONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/linux/ghostty-qt.metainfo.xml.in"
    "${_ghostty_qt_desktop_build_dir}/ghostty-qt.metainfo.xml.in"
    @ONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchInstalledDesktopMetadata.cmake.in"
    "${_ghostty_qt_desktop_build_dir}/PatchInstalledDesktopMetadata.cmake.in"
    @ONLY
)
file(
    GENERATE OUTPUT
        "${_ghostty_qt_desktop_build_dir}/$<CONFIG>/${GHOSTTY_QT_EFFECTIVE_APPLICATION_ID}.desktop"
    INPUT "${_ghostty_qt_desktop_build_dir}/ghostty-qt.desktop.in"
)
file(
    GENERATE OUTPUT
        "${_ghostty_qt_desktop_build_dir}/$<CONFIG>/${GHOSTTY_QT_EFFECTIVE_APPLICATION_ID}.service"
    INPUT "${_ghostty_qt_desktop_build_dir}/ghostty-qt.service.in"
)
file(
    GENERATE OUTPUT
        "${_ghostty_qt_desktop_build_dir}/$<CONFIG>/app-${GHOSTTY_QT_EFFECTIVE_APPLICATION_ID}.service"
    INPUT "${_ghostty_qt_desktop_build_dir}/ghostty-qt-systemd.service.in"
)
file(
    GENERATE OUTPUT
        "${_ghostty_qt_desktop_build_dir}/$<CONFIG>/${GHOSTTY_QT_EFFECTIVE_APPLICATION_ID}.metainfo.xml"
    INPUT "${_ghostty_qt_desktop_build_dir}/ghostty-qt.metainfo.xml.in"
)
file(
    GENERATE OUTPUT
        "${_ghostty_qt_desktop_build_dir}/$<CONFIG>/${GHOSTTY_QT_EFFECTIVE_APPLICATION_ID}.svg"
    INPUT
        "${CMAKE_CURRENT_SOURCE_DIR}/dist/linux/icons/hicolor/scalable/apps/ghostty-qt.svg"
)
file(
    GENERATE OUTPUT
        "${_ghostty_qt_desktop_build_dir}/$<CONFIG>/PatchInstalledDesktopMetadata.cmake"
    INPUT
        "${_ghostty_qt_desktop_build_dir}/PatchInstalledDesktopMetadata.cmake.in"
)

install(TARGETS ghostty-qt RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(
    FILES
        "${_ghostty_qt_desktop_build_dir}/$<CONFIG>/${GHOSTTY_QT_EFFECTIVE_APPLICATION_ID}.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
)
install(
    FILES
        "${_ghostty_qt_desktop_build_dir}/$<CONFIG>/${GHOSTTY_QT_EFFECTIVE_APPLICATION_ID}.service"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/dbus-1/services"
)
install(
    FILES
        "${_ghostty_qt_desktop_build_dir}/$<CONFIG>/app-${GHOSTTY_QT_EFFECTIVE_APPLICATION_ID}.service"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/systemd/user"
)
install(
    FILES
        "${_ghostty_qt_desktop_build_dir}/$<CONFIG>/${GHOSTTY_QT_EFFECTIVE_APPLICATION_ID}.metainfo.xml"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo"
)
install(
    FILES
        "${_ghostty_qt_desktop_build_dir}/$<CONFIG>/${GHOSTTY_QT_EFFECTIVE_APPLICATION_ID}.svg"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
)
install(
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/examples/shaders/bouncing-dvd.glsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/examples/shaders/crt-pane-transition.glsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/examples/shaders/flame-pane-transition.glsl"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/ghostty-qt/shaders"
)
install(
    FILES "${GHOSTTY_QT_SHELL_COMPLETION_SOURCE_DIR}/ghostty-qt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/bash-completion/completions"
)
install(
    FILES "${GHOSTTY_QT_SHELL_COMPLETION_SOURCE_DIR}/ghostty-qt.fish"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/fish/vendor_completions.d"
)
install(
    FILES "${GHOSTTY_QT_SHELL_COMPLETION_SOURCE_DIR}/_ghostty-qt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/zsh/site-functions"
)
install(
    SCRIPT
        "${_ghostty_qt_desktop_build_dir}/$<CONFIG>/PatchInstalledDesktopMetadata.cmake"
)
install(
    DIRECTORY "${GHOSTTY_QT_TERMINFO_DIR}/"
    DESTINATION "${GHOSTTY_QT_INSTALL_TERMINFO_DIR}"
)
install(
    DIRECTORY "${GHOSTTY_QT_SHELL_INTEGRATION_STAGE_DIR}/"
    DESTINATION "${GHOSTTY_QT_INSTALL_SHELL_INTEGRATION_DIR}"
)
