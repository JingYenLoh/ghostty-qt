#pragma once

#include <QMetaType>
#include <QtGlobal>

enum class TerminalFileLocation : quint8 {
    Screen,
    Scrollback,
    Selection,
};

enum class TerminalFileDisposition : quint8 {
    Copy,
    Paste,
    Open,
};

enum class TerminalFileFormat : quint8 {
    Plain,
};

struct TerminalWriteFileAction {
    TerminalFileLocation location = TerminalFileLocation::Screen;
    TerminalFileDisposition disposition = TerminalFileDisposition::Copy;
    TerminalFileFormat format = TerminalFileFormat::Plain;

    friend bool operator==(const TerminalWriteFileAction &,
                           const TerminalWriteFileAction &) = default;
};

Q_DECLARE_METATYPE(TerminalWriteFileAction)
