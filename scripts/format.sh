#!/usr/bin/env bash

set -euo pipefail

usage()
{
    printf 'Usage: %s [--all|--check|--staged]\n' "${0##*/}" >&2
    exit 2
}

if [[ "$#" -ne 1 ]]; then
    usage
fi

mode="$1"
case "$mode" in
    --all | --check | --staged) ;;
    *) usage ;;
esac

root="$(git rev-parse --show-toplevel)"
cd "$root"

temporary_directory="$(mktemp -d)"
cleanup()
{
    rm -rf -- "$temporary_directory"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

format_sequence=0
formatted_path=""
next_temporary_path()
{
    local logical_path="$1"

    format_sequence=$((format_sequence + 1))
    formatted_path="$temporary_directory/$format_sequence-${logical_path##*/}"
}

require_command()
{
    local command_name="$1"
    local purpose="$2"

    if ! command -v "$command_name" > /dev/null 2>&1; then
        printf 'error: %s is required to format %s\n' \
            "$command_name" "$purpose" >&2
        return 1
    fi
}

find_qmlformat()
{
    if [[ -n "${QMLFORMAT:-}" ]]; then
        printf '%s\n' "$QMLFORMAT"
    elif command -v qmlformat > /dev/null 2>&1; then
        command -v qmlformat
    elif [[ -x /usr/lib/qt6/bin/qmlformat ]]; then
        printf '%s\n' /usr/lib/qt6/bin/qmlformat
    elif [[ -x /usr/lib64/qt6/bin/qmlformat ]]; then
        printf '%s\n' /usr/lib64/qt6/bin/qmlformat
    else
        printf 'error: Qt 6 qmlformat is required to format QML\n' >&2
        return 1
    fi
}

find_zig()
{
    if [[ -n "${ZIG:-}" ]]; then
        printf '%s\n' "$ZIG"
    elif [[ -x "$root/.local/bin/zig" ]]; then
        printf '%s\n' "$root/.local/bin/zig"
    elif command -v zig > /dev/null 2>&1; then
        command -v zig
    else
        printf 'error: Zig is required to format Zig sources\n' >&2
        return 1
    fi
}

run_gersemi()
{
    if [[ -n "${GERSEMI:-}" ]]; then
        "$GERSEMI" "$@"
    elif command -v gersemi > /dev/null 2>&1; then
        gersemi "$@"
    elif command -v uvx > /dev/null 2>&1; then
        uvx --from gersemi==0.28.0 gersemi "$@"
    else
        printf 'error: gersemi or uvx is required to format CMake files\n' >&2
        return 1
    fi
}

is_supported_path()
{
    case "$1" in
        *.c | *.cc | *.cpp | *.cxx | *.h | *.hh | *.hpp | *.hxx | \
            *.py | *.qml | *.zig | *.zig.zon | *.json | *.svg | *.xml | \
            *.xml.in | *.sh | CMakeLists.txt | */CMakeLists.txt | *.cmake | \
            *.cmake.in | .githooks/* | dist/arch/PKGBUILD)
            return 0
            ;;
        *) return 1 ;;
    esac
}

format_python()
{
    local logical_path="$1"
    local physical_path="$2"
    local linted_path

    require_command ruff Python
    next_temporary_path "$logical_path.linted"
    linted_path="$formatted_path"
    ruff check --fix-only --quiet --stdin-filename "$logical_path" - \
        < "$physical_path" > "$linted_path"
    next_temporary_path "$logical_path"
    ruff format --quiet --stdin-filename "$logical_path" - \
        < "$linted_path" > "$formatted_path"
    ruff check --quiet --stdin-filename "$logical_path" - \
        < "$formatted_path"
    cp -- "$formatted_path" "$physical_path"
}

format_shell()
{
    local logical_path="$1"
    local physical_path="$2"
    local dialect="bash"

    require_command shfmt shell
    if head -n 1 "$physical_path" | grep -Eq '^#!.*/(dash|sh)( |$)'; then
        dialect="posix"
    fi
    next_temporary_path "$logical_path"
    shfmt -ln "$dialect" -i 4 -ci -sr -fn --filename "$logical_path" \
        < "$physical_path" > "$formatted_path"
    cp -- "$formatted_path" "$physical_path"
}

format_file()
{
    local logical_path="$1"
    local physical_path="$2"
    local formatter

    case "$logical_path" in
        *.c | *.cc | *.cpp | *.cxx | *.h | *.hh | *.hpp | *.hxx)
            formatter="${CLANG_FORMAT:-clang-format}"
            require_command "$formatter" C/C++
            next_temporary_path "$logical_path"
            "$formatter" --style=file \
                --assume-filename="$root/$logical_path" < "$physical_path" \
                > "$formatted_path"
            cp -- "$formatted_path" "$physical_path"
            ;;
        *.py)
            format_python "$logical_path" "$physical_path"
            ;;
        *.qml)
            formatter="$(find_qmlformat)"
            next_temporary_path "$logical_path"
            "$formatter" -s "$root/.qmlformat.ini" "$physical_path" \
                > "$formatted_path"
            cp -- "$formatted_path" "$physical_path"
            ;;
        *.zig.zon)
            formatter="$(find_zig)"
            next_temporary_path "$logical_path"
            "$formatter" fmt --zon --stdin < "$physical_path" \
                > "$formatted_path"
            cp -- "$formatted_path" "$physical_path"
            ;;
        *.zig)
            formatter="$(find_zig)"
            next_temporary_path "$logical_path"
            "$formatter" fmt --stdin < "$physical_path" > "$formatted_path"
            cp -- "$formatted_path" "$physical_path"
            ;;
        CMakeLists.txt | */CMakeLists.txt | *.cmake | *.cmake.in)
            next_temporary_path "$logical_path"
            run_gersemi --config "$root/.gersemirc" --no-cache - \
                < "$physical_path" > "$formatted_path"
            cp -- "$formatted_path" "$physical_path"
            ;;
        *.json)
            require_command jq JSON
            next_temporary_path "$logical_path"
            jq --indent 2 . "$physical_path" > "$formatted_path"
            cp -- "$formatted_path" "$physical_path"
            ;;
        *.svg | *.xml | *.xml.in)
            require_command xmllint XML
            next_temporary_path "$logical_path"
            XMLLINT_INDENT='  ' xmllint --format "$physical_path" \
                > "$formatted_path"
            cp -- "$formatted_path" "$physical_path"
            ;;
        *.sh | .githooks/* | dist/arch/PKGBUILD)
            format_shell "$logical_path" "$physical_path"
            ;;
        *) return 3 ;;
    esac
}

format_all()
{
    local path

    while IFS= read -r -d '' path; do
        if [[ -f "$path" ]] && is_supported_path "$path"; then
            format_file "$path" "$path"
        fi
    done < <(git ls-files -z)
}

check_all()
{
    local path
    local candidate
    local failed=0

    while IFS= read -r -d '' path; do
        if [[ ! -f "$path" ]] || ! is_supported_path "$path"; then
            continue
        fi

        next_temporary_path "$path"
        candidate="$formatted_path"
        cp -- "$path" "$candidate"
        format_file "$path" "$candidate"
        if ! cmp -s "$path" "$candidate"; then
            printf 'error: %s needs formatting\n' "$path" >&2
            if ! diff -u --label "a/$path" --label "b/$path (formatted)" \
                "$path" "$candidate"; then
                :
            fi
            failed=1
        fi
    done < <(git ls-files -z)

    return "$failed"
}

format_staged()
{
    local path
    local original
    local candidate
    local file_mode
    local object_id
    local stage_and_path
    local new_object

    while IFS= read -r -d '' path; do
        if ! is_supported_path "$path"; then
            continue
        fi

        read -r file_mode object_id stage_and_path < <(git ls-files -s -- "$path")
        if [[ "$file_mode" != 100* ]]; then
            continue
        fi

        next_temporary_path "$path.original"
        original="$formatted_path"
        git cat-file blob ":$path" > "$original"
        next_temporary_path "$path"
        candidate="$formatted_path"
        cp -- "$original" "$candidate"
        format_file "$path" "$candidate"
        if cmp -s "$original" "$candidate"; then
            continue
        fi

        new_object="$(git hash-object -w -- "$candidate")"
        git update-index --cacheinfo "$file_mode,$new_object,$path"
        if [[ -f "$path" ]] && cmp -s "$path" "$original"; then
            cp -- "$candidate" "$path"
        fi
        printf 'formatted staged file: %s\n' "$path"
    done < <(git diff --cached --name-only --diff-filter=ACMR -z)
}

case "$mode" in
    --all) format_all ;;
    --check) check_all ;;
    --staged) format_staged ;;
esac
