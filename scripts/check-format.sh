#!/usr/bin/env bash

set -euo pipefail

usage()
{
    printf 'Usage: %s [--staged|--diff|--fix]\n' "${0##*/}" >&2
    exit 2
}

mode="--staged"
if [[ "$#" -gt 1 ]]; then
    usage
fi
if [[ "$#" -eq 1 ]]; then
    mode="$1"
fi

case "$mode" in
    --staged | --diff | --fix) ;;
    *) usage ;;
esac

clang_format="${CLANG_FORMAT:-clang-format}"
if ! command -v "$clang_format" >/dev/null 2>&1; then
    printf 'error: clang-format is required for the format check\n' >&2
    exit 1
fi

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

is_cpp_source()
{
    case "$1" in
        *.c | *.cc | *.cpp | *.cxx | *.h | *.hh | *.hpp | *.hxx) return 0 ;;
        *) return 1 ;;
    esac
}

failed=0
check_changed_file()
{
    local path="$1"
    local source="$2"
    local original
    local formatted
    local line
    local start
    local count
    local end
    local -a ranges=()
    local hunks="$temporary_directory/hunks"

    if [[ "$source" == "index" ]]; then
        git diff --cached --unified=0 --no-color \
            --no-ext-diff -- "$path" >"$hunks"
    elif [[ "$source" == "worktree" ]]; then
        git diff --unified=0 --no-color --no-ext-diff -- "$path" >"$hunks"
    else
        git diff HEAD --unified=0 --no-color \
            --no-ext-diff -- "$path" >"$hunks"
    fi

    while IFS= read -r line; do
        if [[ "$line" =~ ^@@\ -[0-9]+(,[0-9]+)?\ \+([0-9]+)(,([0-9]+))?\ @@ ]]; then
            start="${BASH_REMATCH[2]}"
            count="${BASH_REMATCH[4]:-1}"
            if [[ "$count" -gt 0 ]]; then
                end=$((start + count - 1))
                ranges+=("--lines=$start:$end")
            fi
        fi
    done <"$hunks"

    if [[ "${#ranges[@]}" -eq 0 ]]; then
        return
    fi

    if [[ "$mode" == "--fix" ]]; then
        "$clang_format" -i --style=file "${ranges[@]}" "$path"
        return
    fi

    original="$temporary_directory/original"
    formatted="$temporary_directory/formatted"
    if [[ "$source" == "index" ]]; then
        git cat-file blob ":$path" >"$original"
    else
        cp -- "$path" "$original"
    fi

    "$clang_format" --style=file --assume-filename="$root/$path" \
        "${ranges[@]}" <"$original" >"$formatted"

    if ! cmp -s "$original" "$formatted"; then
        printf 'error: changed lines in %s need clang-format\n' "$path" >&2
        if ! diff -u --label "a/$path" --label "b/$path (clang-format)" \
            "$original" "$formatted"; then
            :
        fi
        failed=1
    fi
}

paths="$temporary_directory/paths"
if [[ "$mode" == "--staged" ]]; then
    git diff --cached --name-only --diff-filter=ACMR -z >"$paths"
    while IFS= read -r -d '' path; do
        if is_cpp_source "$path"; then
            check_changed_file "$path" index
        fi
    done <"$paths"
elif [[ "$mode" == "--diff" ]]; then
    git diff --name-only --diff-filter=ACMR -z >"$paths"
    while IFS= read -r -d '' path; do
        if is_cpp_source "$path"; then
            check_changed_file "$path" worktree
        fi
    done <"$paths"
else
    git diff HEAD --name-only --diff-filter=ACMR -z >"$paths"
    while IFS= read -r -d '' path; do
        if ! is_cpp_source "$path"; then
            continue
        fi

        check_changed_file "$path" combined
    done <"$paths"
fi

if [[ "$failed" -ne 0 ]]; then
    printf '\nRun %s --fix, review the result, and stage it again.\n' \
        "$0" >&2
    exit 1
fi
