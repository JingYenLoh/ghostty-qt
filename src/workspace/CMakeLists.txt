# Window/workspace models and UI-facing controllers.

ghostty_qt_add_internal_library(ghostty-qt-window-ui
    src/workspace/window_ui_controller.cpp
    src/workspace/window_ui_controller.h
)
target_link_libraries(ghostty-qt-window-ui PUBLIC Qt6::Core Qt6::Qml)

ghostty_qt_add_internal_library(ghostty-qt-tab-list-model
    src/workspace/tab_list_model.cpp
    src/workspace/tab_list_model.h
    src/workspace/workspace_ids.h
)
target_link_libraries(ghostty-qt-tab-list-model PUBLIC Qt6::Core)
