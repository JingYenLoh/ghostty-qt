#!/bin/sh

set -eu

ZIG_VERSION=0.15.2

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
toolchains_dir="$repo_root/.local/toolchains"
bin_dir="$repo_root/.local/bin"
install_dir="$toolchains_dir/zig-$ZIG_VERSION"
zig_link="$bin_dir/zig"

if [ "$(uname -s)" != Linux ]; then
    echo "error: the project-local Zig bootstrap supports Linux only" >&2
    exit 1
fi

case "$(uname -m)" in
    x86_64|amd64)
        zig_arch=x86_64
        archive_sha256=02aa270f183da276e5b5920b1dac44a63f1a49e55050ebde3aecc9eb82f93239
        ;;
    aarch64|arm64)
        zig_arch=aarch64
        archive_sha256=958ed7d1e00d0ea76590d27666efbf7a932281b3d7ba0c6b01b0ff26498f667f
        ;;
    *)
        echo "error: unsupported Linux architecture: $(uname -m)" >&2
        exit 1
        ;;
esac

mkdir -p "$toolchains_dir" "$bin_dir"

if [ -x "$install_dir/zig" ]; then
    installed_version=$($install_dir/zig version)
    if [ "$installed_version" != "$ZIG_VERSION" ]; then
        echo "error: $install_dir contains Zig $installed_version, expected $ZIG_VERSION" >&2
        exit 1
    fi

    ln -sfn "../toolchains/zig-$ZIG_VERSION/zig" "$zig_link"
    echo "Zig $ZIG_VERSION is already installed at $install_dir"
    exit 0
fi

if [ -e "$install_dir" ]; then
    echo "error: $install_dir exists but does not contain an executable Zig compiler" >&2
    exit 1
fi

archive_name="zig-$zig_arch-linux-$ZIG_VERSION.tar.xz"
cache_dir=${ZIG_DOWNLOAD_CACHE:-"$repo_root/.cache/zig"}
archive_path="$cache_dir/$archive_name"
base_url=${ZIG_DOWNLOAD_BASE_URL:-https://ziglang.org/download}
base_url=${base_url%/}
download_url="$base_url/$ZIG_VERSION/$archive_name"

mkdir -p "$cache_dir"

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
if [ "$actual_sha256" != "$archive_sha256" ]; then
    rm -f -- "$archive_path"
    echo "error: SHA-256 mismatch for $archive_name" >&2
    echo "expected: $archive_sha256" >&2
    echo "actual:   $actual_sha256" >&2
    exit 1
fi

staging_dir=$(mktemp -d "$toolchains_dir/.zig-$ZIG_VERSION.XXXXXX")
trap 'rm -rf -- "$staging_dir"' EXIT HUP INT TERM
tar -xJf "$archive_path" --strip-components=1 -C "$staging_dir"

extracted_version=$($staging_dir/zig version)
if [ "$extracted_version" != "$ZIG_VERSION" ]; then
    echo "error: archive contains Zig $extracted_version, expected $ZIG_VERSION" >&2
    exit 1
fi

mv "$staging_dir" "$install_dir"
trap - EXIT HUP INT TERM
ln -sfn "../toolchains/zig-$ZIG_VERSION/zig" "$zig_link"

echo "Installed Zig $ZIG_VERSION at $install_dir"
