# Repository guidance

## Sanitizer tests in managed environments

The managed development environment runs processes under a supervisor that
prevents LeakSanitizer from using `ptrace`. Tests then finish their assertions
but abort during leak checking with:

```text
LeakSanitizer has encountered a fatal error.
LeakSanitizer does not work under ptrace (strace, gdb, etc).
```

This is an environment limitation, not evidence of a test or memory-safety
failure. The `sanitize` CTest preset sets `detect_leaks=1` itself, so a shell
override placed before `ctest --preset sanitize` will not take effect. In this
environment, run the already-built sanitizer suite directly instead:

```sh
ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build/sanitize --output-on-failure
```

Keep using `ctest --preset sanitize` when LeakSanitizer has the required
process-tracing support; disabling leak detection is only the managed-environment
fallback.
