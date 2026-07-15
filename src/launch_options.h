#pragma once

#include <QString>
#include <QStringList>

class QCoreApplication;

struct LaunchOptions {
    QString workingDirectory;
    QString fontFamily;
    double fontSize = 12.0;
    int scrollbackLines = 10'000;
    bool hold = false;
    bool showHelp = false;
    bool showVersion = false;
    QStringList program;
};

// The first argument is expected to be the application name, as it is in
// QCoreApplication::arguments(). This overload keeps option parsing easy to
// exercise without constructing additional application objects in tests.
bool parseLaunchOptions(const QStringList &arguments, LaunchOptions *options,
                        QString *errorMessage = nullptr);

bool parseLaunchOptions(QCoreApplication &application, LaunchOptions *options,
                        QString *errorMessage = nullptr);
