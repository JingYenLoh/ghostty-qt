# Local RPM package for the checkout containing this file. Use build.sh.

%{!?ghostty_qt_repo_root:%global ghostty_qt_repo_root %{_builddir}/ghostty-qt}
%{!?ghostty_qt_zig:%global ghostty_qt_zig %{ghostty_qt_repo_root}/.local/bin/zig}
%{!?ghostty_qt_version:%global ghostty_qt_version 0.1.0}
%{!?ghostty_qt_revision_count:%global ghostty_qt_revision_count 0}
%{!?ghostty_qt_revision:%global ghostty_qt_revision local}

Name:           ghostty-qt
Version:        %{ghostty_qt_version}
Release:        1.%{ghostty_qt_revision_count}.g%{ghostty_qt_revision}%{?dist}
Summary:        Qt Quick frontend for the Ghostty terminal engine

License:        LicenseRef-Unlicensed
URL:            https://github.com/JingYenLoh/ghostty-qt

BuildRequires:  cmake >= 3.24
BuildRequires:  fontconfig-devel
BuildRequires:  gcc-c++
BuildRequires:  git
BuildRequires:  kf6-kwindowsystem-devel
BuildRequires:  layer-shell-qt-devel >= 6.6.4
BuildRequires:  libxkbcommon-devel
BuildRequires:  ninja-build
BuildRequires:  ncurses
BuildRequires:  patch
BuildRequires:  pkgconf-pkg-config
BuildRequires:  python3 >= 3.10
BuildRequires:  qt6-qtbase-devel >= 6.10
BuildRequires:  qt6-qtbase-private-devel >= 6.10
BuildRequires:  qt6-qtdeclarative-devel >= 6.10
BuildRequires:  qt6-qtmultimedia-devel >= 6.10
BuildRequires:  qt6-qtshadertools-devel >= 6.10
BuildRequires:  qt6-qtwayland-devel >= 6.10
BuildRequires:  wayland-devel

Requires:       layer-shell-qt
Requires:       qt6-qtdeclarative
Requires:       qt6-qtwayland
Suggests:       kf6-qqc2-desktop-style

%description
ghostty-qt is a Linux/Wayland terminal emulator using Ghostty's terminal
engine and a Qt Quick frontend.

This binary was built from a local checkout by the project's convenience
packaging recipe. It is not an official distribution package.

%prep
test -f "%{ghostty_qt_repo_root}/ghostty/CMakeLists.txt" || {
    echo 'Initialize the Ghostty submodule first.' >&2
    exit 1
}
test -x "%{ghostty_qt_zig}" || {
    echo 'Bootstrap the project-local Zig toolchain first.' >&2
    exit 1
}
test "$("%{ghostty_qt_zig}" version)" = 0.16.0 || {
    echo 'ghostty-qt requires the project-local Zig 0.16.0 toolchain.' >&2
    exit 1
}

%build
cmake -S "%{ghostty_qt_repo_root}" -B "%{_vpath_builddir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=%{_prefix} \
    -DCMAKE_INSTALL_LIBDIR=%{_lib} \
    -DGHOSTTY_QT_BUILD_TESTS=OFF \
    -DGHOSTTY_QT_CONFIG_BUILD_DIR="%{_topdir}/cache/ghostty-internal" \
    -DGHOSTTY_QT_LINK_MATCHER_BUILD_DIR="%{_topdir}/cache/ghostty-link-matcher" \
    -DGHOSTTY_QT_ZIG_GLOBAL_CACHE_DIR="%{_topdir}/cache/zig-global" \
    -DGHOSTTY_QT_ZIG_EXECUTABLE="%{ghostty_qt_zig}" \
    -DZIG_EXECUTABLE="%{ghostty_qt_zig}"
cmake --build "%{_vpath_builddir}" -j"$(nproc)"

%install
DESTDIR="%{buildroot}" cmake --install "%{_vpath_builddir}"

%files
%{_bindir}/ghostty-qt
%{_bindir}/ghostty-qt-config-helper
%{_libdir}/ghostty-qt/
%{_datadir}/applications/io.github.JingYenLoh.ghostty_qt.desktop
%{_datadir}/dbus-1/services/io.github.JingYenLoh.ghostty_qt.service
%{_datadir}/ghostty-qt/
%{_datadir}/icons/hicolor/scalable/apps/io.github.JingYenLoh.ghostty_qt.svg
%{_datadir}/metainfo/io.github.JingYenLoh.ghostty_qt.metainfo.xml
%{_datadir}/systemd/user/app-io.github.JingYenLoh.ghostty_qt.service

%changelog
* Wed Aug 12 2026 Jing Yen Loh <lohjingyen@gmail.com> - 0.1.0-1
- Add a convenience package for local builds.
