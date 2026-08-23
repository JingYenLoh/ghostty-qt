# Integration hooks compiled into the real application for end-to-end tests.

target_sources(
    ghostty-qt
    PRIVATE
        src/testing/application_test_hooks.cpp
        src/testing/application_test_hooks.h
)
