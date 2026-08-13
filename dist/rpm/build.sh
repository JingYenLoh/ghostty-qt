#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(realpath "$script_dir/../..")
build_root="$script_dir/build"
zig="$repo_root/.local/bin/zig"

fail()
{
    printf 'error: %s\n' "$*" >&2
    exit 1
}

for tool in cmake find git nproc realpath rpmbuild sed; do
    command -v "$tool" > /dev/null 2>&1 || fail "required tool not found: $tool"
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

cmake -E rm -rf "$build_root"
cmake -E make_directory \
    "$build_root/BUILD" \
    "$build_root/BUILDROOT" \
    "$build_root/RPMS" \
    "$build_root/SOURCES" \
    "$build_root/SPECS" \
    "$build_root/SRPMS"

rpmbuild -bb "$script_dir/ghostty-qt.spec" \
    --define "_topdir $build_root" \
    --define "ghostty_qt_repo_root $repo_root" \
    --define "ghostty_qt_zig $zig" \
    --define "ghostty_qt_version $project_version" \
    --define "ghostty_qt_revision_count $revision_count" \
    --define "ghostty_qt_revision $revision"

find "$build_root/RPMS" -type f -name '*.rpm' -print
