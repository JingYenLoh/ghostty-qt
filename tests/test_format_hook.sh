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
cp "$source_directory/scripts/format.sh" "$repo/scripts/format.sh"
chmod +x "$repo/.githooks/pre-commit" "$repo/scripts/format.sh"

cd "$repo"
git init --quiet
git config user.email "format-hook@example.invalid"
git config user.name "Format Hook Test"

write_formatted()
{
    local path="${1:-sample.cpp}"

    printf '%s\n' \
        'int answer()' \
        '{' \
        '    return 42;' \
        '}' \
        '' \
        'int secondAnswer()' \
        '{' \
        '    return 7;' \
        '}' > "$path"
}

write_unformatted()
{
    local path="${1:-sample.cpp}"

    printf '%s\n' \
        'int answer()' \
        '{' \
        '    return 42;' \
        '}' \
        '' \
        'int secondAnswer(){return 7;}' > "$path"
}

restore_baseline()
{
    git restore --staged --worktree sample.cpp
}

printf '%s\n' \
    'int answer()' \
    '{' \
    '    return 42;' \
    '}' > sample.cpp
git add .clang-format .githooks/pre-commit scripts/format.sh sample.cpp
git commit --quiet --no-verify -m baseline
git config core.hooksPath .githooks

expected="$temporary_directory/expected.cpp"
actual="$temporary_directory/actual.cpp"
unformatted="$temporary_directory/unformatted.cpp"
write_formatted "$expected"
write_unformatted "$unformatted"

assert_staged_formatted()
{
    git cat-file blob :sample.cpp > "$actual"
    if ! cmp -s "$expected" "$actual"; then
        printf 'error: staged C++ content was not formatted\n' >&2
        exit 1
    fi
}

# Formatted staged content passes.
write_formatted
git add sample.cpp
.githooks/pre-commit
assert_staged_formatted
restore_baseline

# An unformatted staged file is formatted in both the index and worktree when
# there are no unrelated unstaged changes.
write_unformatted
git add sample.cpp
.githooks/pre-commit
assert_staged_formatted
if ! cmp -s "$expected" sample.cpp; then
    printf 'error: synchronized worktree content was not formatted\n' >&2
    exit 1
fi
restore_baseline

# Partial staging is preserved in both directions. A formatted index does not
# overwrite an unformatted worktree, and formatting the index does not disturb
# a different worktree version.
write_formatted
git add sample.cpp
write_unformatted
.githooks/pre-commit
assert_staged_formatted
if ! cmp -s "$unformatted" sample.cpp; then
    printf 'error: hook overwrote an unrelated unstaged edit\n' >&2
    exit 1
fi
restore_baseline

write_unformatted
git add sample.cpp
write_formatted
.githooks/pre-commit
assert_staged_formatted
if ! cmp -s "$expected" sample.cpp; then
    printf 'error: hook changed a different worktree version\n' >&2
    exit 1
fi
restore_baseline

# Unsupported staged content is outside the hook's scope.
printf '%s\n' 'not C++ and intentionally unformatted' > notes.txt
git add notes.txt
.githooks/pre-commit
