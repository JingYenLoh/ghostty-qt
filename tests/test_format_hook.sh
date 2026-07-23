#!/usr/bin/env bash

set -euo pipefail

if [[ "$#" -ne 1 ]]; then
    printf 'Usage: %s <source-directory>\n' "${0##*/}" >&2
    exit 2
fi

source_directory="$1"
temporary_directory="$(mktemp -d)"
cleanup()
{
    rm -rf -- "$temporary_directory"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

repo="$temporary_directory/repository"
mkdir -p "$repo/.githooks" "$repo/scripts"
cp "$source_directory/.clang-format" "$repo/.clang-format"
cp "$source_directory/.githooks/pre-commit" "$repo/.githooks/pre-commit"
cp "$source_directory/scripts/check-format.sh" \
    "$repo/scripts/check-format.sh"
chmod +x "$repo/.githooks/pre-commit" "$repo/scripts/check-format.sh"

cd "$repo"
git init --quiet
git config user.email "format-hook@example.invalid"
git config user.name "Format Hook Test"

write_formatted()
{
    printf '%s\n' \
        'int answer()' \
        '{' \
        '    return 42;' \
        '}' \
        '' \
        'int secondAnswer()' \
        '{' \
        '    return 7;' \
        '}' >sample.cpp
}

write_unformatted()
{
    printf '%s\n' \
        'int answer()' \
        '{' \
        '    return 42;' \
        '}' \
        '' \
        'int secondAnswer(){return 7;}' >sample.cpp
}

restore_baseline()
{
    git restore --staged --worktree sample.cpp
}

printf '%s\n' \
    'int answer()' \
    '{' \
    '    return 42;' \
    '}' >sample.cpp
git add .clang-format .githooks/pre-commit scripts/check-format.sh sample.cpp
git commit --quiet --no-verify -m baseline
git config core.hooksPath .githooks

# Formatted staged content passes.
write_formatted
git add sample.cpp
.githooks/pre-commit
restore_baseline

# A staged-only violation fails. --fix updates the worktree relative to HEAD
# but deliberately leaves the index untouched until the developer restages it.
write_unformatted
git add sample.cpp
if .githooks/pre-commit >/dev/null 2>&1; then
    printf 'error: unformatted staged content passed\n' >&2
    exit 1
fi
scripts/check-format.sh --fix
if .githooks/pre-commit >/dev/null 2>&1; then
    printf 'error: --fix unexpectedly modified the index\n' >&2
    exit 1
fi
git add sample.cpp
.githooks/pre-commit
restore_baseline

# The hook reads the index rather than the worktree in both partial-stage
# directions.
write_formatted
git add sample.cpp
write_unformatted
.githooks/pre-commit
restore_baseline

write_unformatted
git add sample.cpp
write_formatted
if .githooks/pre-commit >/dev/null 2>&1; then
    printf 'error: formatted worktree masked an unformatted index\n' >&2
    exit 1
fi
restore_baseline

# Non-C++ staged content is outside the hook's scope.
printf '%s\n' 'not C++ and intentionally unformatted' >notes.txt
git add notes.txt
.githooks/pre-commit
