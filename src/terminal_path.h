#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QDebug>
#include <QFile>
#include <QString>
#include <QStringView>

#include <utility>

// An owned POSIX path. Terminal working directories originate in Ghostty
// configuration and OSC 7 as arbitrary bytes, so QString is only a display
// projection and must never become the authoritative value again.
class TerminalPath final {
public:
    TerminalPath() = default;
    TerminalPath(QByteArray bytes)
        : bytes_(std::move(bytes))
    {}
    TerminalPath(QByteArrayView bytes)
        : bytes_(bytes.data(), bytes.size())
    {}
    TerminalPath(const QString &displayPath)
        : bytes_(QFile::encodeName(displayPath))
    {}
    TerminalPath(QStringView displayPath)
        : TerminalPath(displayPath.toString())
    {}

    TerminalPath &operator=(QByteArray bytes)
    {
        bytes_ = std::move(bytes);
        return *this;
    }

    TerminalPath &operator=(const QString &displayPath)
    {
        bytes_ = QFile::encodeName(displayPath);
        return *this;
    }

    [[nodiscard]] const QByteArray &bytes() const noexcept { return bytes_; }
    [[nodiscard]] QByteArrayView view() const noexcept { return bytes_; }
    [[nodiscard]] const char *c_str() const noexcept
    {
        return bytes_.constData();
    }
    [[nodiscard]] bool isEmpty() const noexcept { return bytes_.isEmpty(); }
    void clear() noexcept { bytes_.clear(); }

    [[nodiscard]] QString displayString() const
    {
        return QFile::decodeName(bytes_);
    }

    friend bool operator==(const TerminalPath &,
                           const TerminalPath &) = default;

    friend bool operator==(const TerminalPath &path, QByteArrayView bytes)
    {
        return path.view() == bytes;
    }

    friend bool operator==(QByteArrayView bytes, const TerminalPath &path)
    {
        return path == bytes;
    }

    friend bool operator==(const TerminalPath &path, QStringView displayPath)
    {
        return path
            == QByteArrayView(QFile::encodeName(displayPath.toString()));
    }

    friend bool operator==(const TerminalPath &path, const QString &displayPath)
    {
        return path == QStringView(displayPath);
    }

    friend bool operator==(QStringView displayPath, const TerminalPath &path)
    {
        return path == displayPath;
    }

    friend bool operator==(const QString &displayPath, const TerminalPath &path)
    {
        return path == QStringView(displayPath);
    }

private:
    QByteArray bytes_;
};

inline QDebug operator<<(QDebug debug, const TerminalPath &path)
{
    QDebugStateSaver saver(debug);
    return debug.nospace() << "TerminalPath(" << path.bytes().toHex() << ')';
}
