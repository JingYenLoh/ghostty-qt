#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(realpath "$script_dir/../..")
build_root="$script_dir/build"
cmake_build_dir="$build_root/cmake"
package_root="$build_root/root"
zig="$repo_root/.local/bin/zig"

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

for tool in cmake dpkg-architecture dpkg-deb dpkg-gencontrol \
    dpkg-shlibdeps git ln ninja nproc realpath sed; do
    command -v "$tool" >/dev/null 2>&1 || fail "required tool not found: $tool"
done

if [ ! -f "$repo_root/ghostty/CMakeLists.txt" ]; then
    fail "initialize the Ghostty submodule first: git submodule update --init --recursive"
fi

if [ ! -x "$zig" ] || [ "$("$zig" version)" != 0.16.0 ]; then
    fail "bootstrap the required Zig 0.16.0 toolchain first: ./scripts/bootstrap-zig.sh"
fi

project_version=$(sed -n \
    's/^project(ghostty-qt VERSION \([^ ]*\).*/\1/p' \
    "$repo_root/CMakeLists.txt")
[ -n "$project_version" ] || fail "could not read the project version"

revision_count=$(git -C "$repo_root" rev-list --count HEAD)
revision=$(git -C "$repo_root" rev-parse --short=7 HEAD)
package_version="${project_version}+git${revision_count}.g${revision}-1"
architecture=$(dpkg-architecture -qDEB_HOST_ARCH)
multiarch=$(dpkg-architecture -qDEB_HOST_MULTIARCH)
package="$script_dir/ghostty-qt_${package_version}_${architecture}.deb"
package_changelog="$build_root/changelog"

cmake -E rm -rf "$build_root"
cmake -E make_directory "$cmake_build_dir" "$package_root/DEBIAN"

cmake -S "$repo_root" -B "$cmake_build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR="lib/$multiarch" \
    -DGHOSTTY_QT_BUILD_TESTS=OFF \
    -DGHOSTTY_QT_CONFIG_BUILD_DIR="$build_root/cache/ghostty-internal" \
    -DGHOSTTY_QT_LINK_MATCHER_BUILD_DIR="$build_root/cache/ghostty-link-matcher" \
    -DGHOSTTY_QT_ZIG_GLOBAL_CACHE_DIR="$build_root/cache/zig-global" \
    -DGHOSTTY_QT_ZIG_EXECUTABLE="$zig" \
    -DZIG_EXECUTABLE="$zig"
cmake --build "$cmake_build_dir" -j"$(nproc)"
DESTDIR="$package_root" cmake --install "$cmake_build_dir"

substvars="$build_root/substvars"
printf 'misc:Depends=\n' >"$substvars"
sed "1s/([^)]*)/($package_version)/" \
    "$script_dir/changelog" >"$package_changelog"
set -- \
    -e "$package_root/usr/bin/ghostty-qt" \
    -e "$package_root/usr/bin/ghostty-qt-config-helper"
for library in "$package_root/usr/lib/$multiarch/ghostty-qt/"*.so*; do
    [ -f "$library" ] || continue
    set -- "$@" -e "$library"
done
(
    cd "$script_dir/.."
    dpkg-shlibdeps --ignore-missing-info \
        -S"$package_root" -T"$substvars" "$@"
)

dpkg-gencontrol \
    -c"$script_dir/control" \
    -f"$build_root/files" \
    -l"$package_changelog" \
    -pghostty-qt \
    -P"$package_root" \
    -T"$substvars" \
    -v"$package_version" \
    -O"$package_root/DEBIAN/control"
dpkg-deb --root-owner-group --build "$package_root" "$package"
ln -sfn "$(basename "$package")" "$script_dir/ghostty-qt-local.deb"

printf '%s\n' "$package"
