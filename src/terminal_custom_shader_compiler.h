#pragma once

#include "terminal_custom_shader_options.h"

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include <chrono>
#include <functional>
#include <memory>

inline constexpr auto terminalCustomShaderCompilerCacheVersion =
    "ghostty-qt-shadertoy-v5";

struct TerminalCustomShaderStage {
    QString sourcePath;
    QString qsbPath;
    QByteArray cacheKey;
    QByteArray serializedShader;

    bool operator==(const TerminalCustomShaderStage &) const = default;
};

struct TerminalCustomShaderCompileMetrics {
    int sourceCount = 0;
    int compiledCount = 0;
    int cacheHitCount = 0;
    std::chrono::nanoseconds sourceReadTime{};
    std::chrono::nanoseconds bakeTime{};
    std::chrono::nanoseconds totalTime{};

    bool operator==(const TerminalCustomShaderCompileMetrics &) const = default;
};

struct TerminalCustomShaderCompileResult {
    QVector<TerminalCustomShaderStage> stages;
    TerminalCustomShaderCompileMetrics metrics;
    QString diagnostic;

    [[nodiscard]] bool succeeded() const { return diagnostic.isEmpty(); }
};

// The synchronous entry point is intentionally public for focused tests and
// microbenchmarks. Production requests go through the broker below so shader
// file I/O and QShaderBaker never run on either the GUI or render thread.
[[nodiscard]] TerminalCustomShaderCompileResult
compileTerminalCustomShaders(const TerminalCustomShaderOptions &options,
                             const QString &cacheDirectory = {});

class TerminalCustomShaderCompileBroker final : public QObject {
    Q_OBJECT

public:
    using Completion =
        std::function<void(TerminalCustomShaderCompileResult result)>;

    explicit TerminalCustomShaderCompileBroker(QObject *parent = nullptr);

    // Equivalent requests made in the same event-loop turn are coalesced.
    // Requests arriving after compilation starts are serviced by one queued
    // rerun, so a same-path edit cannot inherit an older in-flight result.
    // The callback always runs on this object's thread and is dropped if
    // `context` dies.
    void request(const TerminalCustomShaderOptions &options, QObject *context,
                 Completion completion, const QString &cacheDirectory = {});

    [[nodiscard]] int inFlightCount() const;

private:
    struct PendingCompletion {
        QPointer<QObject> context;
        Completion completion;
    };

    struct State;
    void launch(const QByteArray &key);

    std::shared_ptr<State> state_;
};
