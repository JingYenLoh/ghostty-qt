#include "terminal/core/terminal_bell.h"

#include <QApplication>
#include <QAudioOutput>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QUrl>
#include <QtLogging>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace {

class QtTerminalBellDevice final : public TerminalBellDevice {
public:
    void ringSystemBell() override { QApplication::beep(); }

    bool setAudioSource(const GhosttyConfigPath &source) override
    {
        player_.reset();
        audioOutput_.reset();
        if (failedSource_ != source) {
            failedSource_.reset();
        }

        const QFileInfo file(source.path);
        if (!file.isAbsolute() || !file.isFile() || !file.isReadable()) {
            reportUnavailableSource(source);
            return false;
        }
        failedSource_.reset();

        auto audioOutput = std::make_unique<QAudioOutput>();
        auto player = std::make_unique<QMediaPlayer>();
        player->setAudioOutput(audioOutput.get());
        QObject::connect(
            player.get(), &QMediaPlayer::errorOccurred, player.get(),
            [source, reported = false](QMediaPlayer::Error error,
                                       const QString &message) mutable {
                if (source.optional || reported
                    || error == QMediaPlayer::NoError) {
                    return;
                }
                reported = true;
                qWarning().noquote()
                    << QStringLiteral(
                           "ghostty-qt: unable to play required bell audio "
                           "'%1': %2")
                           .arg(source.path, message);
            });
        player->setSource(QUrl::fromLocalFile(source.path));

        audioOutput_ = std::move(audioOutput);
        player_ = std::move(player);
        return true;
    }

    void setAudioVolume(double volume) override
    {
        if (audioOutput_ != nullptr) {
            audioOutput_->setVolume(static_cast<float>(volume));
        }
    }

    bool restartAudio() override
    {
        if (player_ == nullptr
            || player_->mediaStatus() == QMediaPlayer::InvalidMedia) {
            return false;
        }
        // stop() resets the position even for media that does not advertise
        // seeking; play() while LoadingMedia records the pending intent.
        player_->stop();
        player_->play();
        return true;
    }

private:
    void reportUnavailableSource(const GhosttyConfigPath &source)
    {
        if (source.optional || failedSource_ == source) return;
        failedSource_ = source;
        qWarning().noquote()
            << QStringLiteral(
                   "ghostty-qt: required bell audio path is not a readable "
                   "file: '%1'")
                   .arg(source.path);
    }

    std::unique_ptr<QAudioOutput> audioOutput_;
    std::unique_ptr<QMediaPlayer> player_;
    std::optional<GhosttyConfigPath> failedSource_;
};

std::unique_ptr<TerminalBellDevice> makeQtTerminalBellDevice()
{
    return std::make_unique<QtTerminalBellDevice>();
}

} // namespace

TerminalBellPlayer::TerminalBellPlayer()
    : TerminalBellPlayer(makeQtTerminalBellDevice())
{}

TerminalBellPlayer::TerminalBellPlayer(
    std::unique_ptr<TerminalBellDevice> device)
{
    setDevice(std::move(device));
}

TerminalBellPlayer::~TerminalBellPlayer() = default;

void TerminalBellPlayer::setDevice(std::unique_ptr<TerminalBellDevice> device)
{
    device_ =
        device != nullptr ? std::move(device) : makeQtTerminalBellDevice();
    preparedAudioSource_.reset();
    audioSourceReady_ = false;
}

void TerminalBellPlayer::ring(const BellFeatures &features,
                              const std::optional<GhosttyConfigPath> &audioPath,
                              double audioVolume)
{
    if (features.system) {
        device_->ringSystemBell();
    }
    if (!features.audio || !audioPath.has_value()) return;

    if (!audioSourceReady_ || preparedAudioSource_ != audioPath) {
        preparedAudioSource_ = audioPath;
        audioSourceReady_ = device_->setAudioSource(*audioPath);
    }
    if (!audioSourceReady_) return;

    const double volume =
        std::isnan(audioVolume) ? 0.5 : std::clamp(audioVolume, 0.0, 1.0);
    device_->setAudioVolume(volume);
    if (!device_->restartAudio()) {
        audioSourceReady_ = false;
    }
}
