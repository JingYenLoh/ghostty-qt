#!/bin/sh

set -eu

ECM_VERSION=6.22.0
ECM_ARCHIVE_SHA256=cb83a69571b277c20b3a6567ef0b6f39bf29c43a619282bf4bb076feb4c609a6
LAYERSHELLQT_VERSION=6.6.4
LAYERSHELLQT_ARCHIVE_SHA256=731af7a222bc1a1e87fd993060ed8fa515b4b38cbc294063b700ec87451e013f

if [ "$#" -ne 1 ]; then
    echo "usage: $0 INSTALL_PREFIX" >&2
    exit 2
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

case "$1" in
    /*) install_prefix=$1 ;;
    *) install_prefix="$PWD/$1" ;;
esac

build_root=${LAYERSHELLQT_BUILD_ROOT:-"$repo_root/build/dependencies/layershellqt-$LAYERSHELLQT_VERSION"}
cache_dir=${LAYERSHELLQT_DOWNLOAD_CACHE:-"$repo_root/.cache/layershellqt"}
kde_download_base=${KDE_DOWNLOAD_BASE_URL:-https://download.kde.org}
kde_download_base=${kde_download_base%/}

ecm_archive="$cache_dir/extra-cmake-modules-$ECM_VERSION.tar.xz"
ecm_source="$build_root/extra-cmake-modules-$ECM_VERSION"
ecm_build="$build_root/ecm-build"
layershellqt_archive="$cache_dir/layer-shell-qt-$LAYERSHELLQT_VERSION.tar.xz"
layershellqt_source="$build_root/layer-shell-qt-$LAYERSHELLQT_VERSION"
layershellqt_build="$build_root/layershellqt-build"

mkdir -p "$build_root" "$cache_dir" "$install_prefix"

download_archive()
{
    archive_path=$1
    expected_sha256=$2
    download_url=$3

    if [ ! -f "$archive_path" ]; then
        partial_path="$archive_path.partial.$$"
        trap 'rm -f -- "$partial_path"' EXIT HUP INT TERM
        echo "Downloading $download_url"
        curl --fail --location --retry 3 --proto '=https' --tlsv1.2 \
            --output "$partial_path" "$download_url"
        mv "$partial_path" "$archive_path"
        trap - EXIT HUP INT TERM
    fi

    actual_sha256=$(sha256sum "$archive_path" | awk '{print $1}')
    if [ "$actual_sha256" != "$expected_sha256" ]; then
        rm -f -- "$archive_path"
        echo "error: SHA-256 mismatch for $(basename -- "$archive_path")" >&2
        echo "expected: $expected_sha256" >&2
        echo "actual:   $actual_sha256" >&2
        exit 1
    fi
}

extract_archive()
{
    archive_path=$1
    source_dir=$2

    if [ -f "$source_dir/CMakeLists.txt" ]; then
        return
    fi
    if [ -e "$source_dir" ]; then
        echo "error: incomplete source directory exists: $source_dir" >&2
        exit 1
    fi

    staging_dir=$(mktemp -d "$build_root/.source.XXXXXX")
    trap 'rm -rf -- "$staging_dir"' EXIT HUP INT TERM
    tar -xJf "$archive_path" --strip-components=1 -C "$staging_dir"
    mv "$staging_dir" "$source_dir"
    trap - EXIT HUP INT TERM
}

download_archive \
    "$ecm_archive" \
    "$ECM_ARCHIVE_SHA256" \
    "$kde_download_base/stable/frameworks/6.22/extra-cmake-modules-$ECM_VERSION.tar.xz"
download_archive \
    "$layershellqt_archive" \
    "$LAYERSHELLQT_ARCHIVE_SHA256" \
    "$kde_download_base/stable/plasma/$LAYERSHELLQT_VERSION/layer-shell-qt-$LAYERSHELLQT_VERSION.tar.xz"

extract_archive "$ecm_archive" "$ecm_source"
extract_archive "$layershellqt_archive" "$layershellqt_source"

cmake -S "$ecm_source" -B "$ecm_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$install_prefix" \
    -DBUILD_DOC=OFF \
    -DBUILD_TESTING=OFF
cmake --build "$ecm_build" --target install -j"$(nproc)"

cmake -S "$layershellqt_source" -B "$layershellqt_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$install_prefix" \
    -DKDE_INSTALL_LIBDIR=lib \
    -DBUILD_SHARED_LIBS=ON \
    -DECM_DIR="$install_prefix/share/ECM/cmake"
cmake --build "$layershellqt_build" --target install -j"$(nproc)"

echo "Installed LayerShellQt $LAYERSHELLQT_VERSION at $install_prefix"
