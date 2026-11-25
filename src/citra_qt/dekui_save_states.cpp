// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <memory>
#include <vector>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>
#include "citra_qt/dekui_save_states.h"
#include "common/file_util.h"
#include "core/core.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/loader/loader.h"
#include "core/movie.h"
#include "core/savestate.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#if defined(ENABLE_MM3D_GAME_FOLDER) && defined(__APPLE__) && !defined(CITRA_VISIONOS)
#include "dekui_visionos/Bridge/icloud_states.h"
#define DEKUI_CLOUD_STATES 1
#endif

namespace {

constexpr int ThumbnailWidth = 480;
constexpr QSize ThumbnailSize{192, 108};

u64 TitleId(Core::System& system) {
    u64 title_id{};
    system.GetAppLoader().ReadProgramId(title_id);
    return title_id;
}

} // namespace

// Asks the renderer for the next frame, as the window shows it, and saves a
// downscaled JPEG of it off the render thread.
void DekuiCaptureStateThumbnail(Core::System& system, const std::string& state_path) {
    auto& renderer = system.GPU().Renderer();
    const Layout::FramebufferLayout layout = renderer.GetRenderWindow().GetFramebufferLayout();
    if (layout.width == 0 || layout.height == 0)
        return;
    // Only one kind of thumbnail per state; this one is flat.
    FileUtil::Delete(Core::SaveStateThumbnailPath(state_path, true));
    const std::string path = Core::SaveStateThumbnailPath(state_path, false);
    const int width = static_cast<int>(layout.width), height = static_cast<int>(layout.height);
    auto pixels = std::make_shared<std::vector<u8>>(std::size_t(width) * height * 4);
    renderer.RequestScreenshot(
        pixels->data(),
        [pixels, width, height, path](bool invert_y) {
            QThreadPool::globalInstance()->start([pixels, width, height, invert_y, path] {
                // BGRA bytes, as QImage::Format_RGB32 stores them on little-endian hosts.
                QImage frame(pixels->data(), width, height, width * 4, QImage::Format_RGB32);
                QImage thumb = frame.scaledToWidth(std::min(ThumbnailWidth, width),
                                                   Qt::SmoothTransformation);
                if (invert_y)
                    thumb = thumb.flipped(Qt::Vertical);
                // Written beside and renamed, so a synced folder never sees half a picture.
                const QString temporary = QString::fromStdString(path) + QStringLiteral(".partial");
                if (thumb.save(temporary, "JPG", 80))
                    FileUtil::Rename(temporary.toStdString(), path);
            });
        },
        layout);
}

std::optional<std::string> DekuiSaveNewState(Core::System& system) {
    if (!system.IsPoweredOn())
        return std::nullopt;
    const std::string path =
        Core::NewSaveStatePath(TitleId(system), system.Movie().GetCurrentMovieID());
    if (!system.SendFileSignal(Core::System::Signal::SaveFile, path))
        return std::nullopt;
    // Serviced by the emulation loop, which also runs while frame advancing.
    system.frame_limiter.AdvanceFrame();
    DekuiCaptureStateThumbnail(system, path);
    return path;
}

bool DekuiLoadLatestState(Core::System& system) {
    if (!system.IsPoweredOn())
        return false;
    for (const auto& file :
         Core::ListSaveStateFiles(TitleId(system), system.Movie().GetCurrentMovieID())) {
        if (file.info.status == Core::SaveStateInfo::ValidationStatus::BuildMismatch)
            continue;
        if (!system.SendFileSignal(Core::System::Signal::LoadFile, file.path))
            return false;
        system.frame_limiter.AdvanceFrame();
        return true;
    }
    return false;
}

DekuiSaveStatesDialog::DekuiSaveStatesDialog(Core::System& system_, QWidget* parent)
    : QDialog(parent), system{system_} {
    setWindowTitle(tr("Save States"));
    resize(560, 620);

    auto* layout = new QVBoxLayout(this);
    auto* header = new QHBoxLayout;
    new_state = new QPushButton(tr("New Save State"), this);
    status = new QLabel(this);
    status->setWordWrap(true);
    header->addWidget(new_state);
    header->addWidget(status, 1);
    layout->addLayout(header);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto* list = new QWidget(scroll);
    rows = new QVBoxLayout(list);
    rows->setAlignment(Qt::AlignTop);
    scroll->setWidget(list);
    layout->addWidget(scroll, 1);

    auto* note = new QLabel(tr("Save states capture the whole emulator. They do not replace "
                               "in-game saves, and may not load after an emulator update."),
                            this);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(new_state, &QPushButton::clicked, this, &DekuiSaveStatesDialog::SaveNew);

#ifdef DEKUI_CLOUD_STATES
    Dekui::CloudStates::DownloadMissing();
#endif
    // Pick up states that iCloud delivers while the window is open.
    auto* refresh = new QTimer(this);
    connect(refresh, &QTimer::timeout, this, [this] {
        if (saving_path.empty())
            Reload();
    });
    refresh->start(5000);

    save_timer = new QTimer(this);
    save_timer->setInterval(250);
    connect(save_timer, &QTimer::timeout, this, &DekuiSaveStatesDialog::PollSave);

    Reload();
}

DekuiSaveStatesDialog::~DekuiSaveStatesDialog() = default;

bool DekuiSaveStatesDialog::GameRunning() const {
    return system.IsPoweredOn();
}

QString DekuiSaveStatesDialog::Name(const Entry& entry) const {
    if (entry.slot)
        return *entry.slot == 0 ? tr("Quick Save") : tr("Slot %1").arg(*entry.slot);
    return QLocale().toString(entry.date, QLocale::ShortFormat);
}

void DekuiSaveStatesDialog::Reload() {
    entries.clear();
    if (GameRunning()) {
        for (const auto& file :
             Core::ListSaveStateFiles(TitleId(system), system.Movie().GetCurrentMovieID())) {
            Entry entry;
            entry.path = file.path;
            entry.slot = file.slot;
            entry.date = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(file.info.time));
            if (file.info.status == Core::SaveStateInfo::ValidationStatus::BuildMismatch)
                entry.incompatibility =
                    tr("Made by %1").arg(QString::fromStdString(file.info.build_name));
            const auto stereo = Core::SaveStateThumbnailPath(file.path, true);
            const auto flat = Core::SaveStateThumbnailPath(file.path, false);
            entry.stereo = FileUtil::Exists(stereo);
            if (entry.stereo)
                entry.thumbnail = QString::fromStdString(stereo);
            else if (FileUtil::Exists(flat))
                entry.thumbnail = QString::fromStdString(flat);
            entries.push_back(std::move(entry));
        }
    }
    Rebuild();
}

void DekuiSaveStatesDialog::Rebuild() {
    while (auto* item = rows->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    new_state->setEnabled(GameRunning() && saving_path.empty());

    if (!GameRunning()) {
        rows->addWidget(new QLabel(tr("Save states are available while the game runs.")));
        return;
    }
    if (!saving_path.empty())
        rows->addWidget(new QLabel(tr("Saving…")));
    if (entries.empty() && saving_path.empty()) {
        rows->addWidget(
            new QLabel(tr("No save states. Use New Save State to save the game as it is now.")));
        return;
    }
    for (const auto& entry : entries)
        rows->addWidget(Row(entry));
}

QWidget* DekuiSaveStatesDialog::Row(const Entry& entry) {
    auto* row = new QWidget;
    auto* layout = new QHBoxLayout(row);

    auto* picture = new QLabel(row);
    picture->setFixedSize(ThumbnailSize);
    picture->setAlignment(Qt::AlignCenter);
    QPixmap pixmap;
    if (!entry.thumbnail.isEmpty() && pixmap.load(entry.thumbnail)) {
        // A 3D thumbnail holds both eyes side by side; show the left one.
        if (entry.stereo)
            pixmap = pixmap.copy(0, 0, pixmap.width() / 2, pixmap.height());
        picture->setPixmap(pixmap.scaled(ThumbnailSize, Qt::KeepAspectRatioByExpanding,
                                         Qt::SmoothTransformation));
    } else {
        picture->setText(tr("No picture"));
    }
    picture->setStyleSheet(QStringLiteral("background: palette(midlight); border-radius: 8px;"));
    layout->addWidget(picture);

    auto* text = new QVBoxLayout;
    text->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(Name(entry)), row));
    if (entry.slot)
        text->addWidget(new QLabel(QLocale().toString(entry.date, QLocale::LongFormat), row));
    if (!entry.incompatibility.isEmpty()) {
        auto* warning = new QLabel(QStringLiteral("⚠ %1").arg(entry.incompatibility), row);
        warning->setStyleSheet(QStringLiteral("color: #c87800;"));
        text->addWidget(warning);
    }
    text->addStretch();
    layout->addLayout(text, 1);

    auto* actions = new QVBoxLayout;
    auto* load = new QPushButton(tr("Load"), row);
    auto* remove = new QPushButton(tr("Delete"), row);
    const bool idle = GameRunning() && saving_path.empty();
    load->setEnabled(idle && entry.incompatibility.isEmpty());
    remove->setEnabled(idle);
    connect(load, &QPushButton::clicked, this, [this, entry] {
        if (QMessageBox::question(this, tr("Load %1?").arg(Name(entry)),
                                  tr("Progress since that state was saved will be lost.")) ==
            QMessageBox::Yes)
            Load(entry);
    });
    connect(remove, &QPushButton::clicked, this, [this, entry] {
        if (QMessageBox::question(this, tr("Delete %1?").arg(Name(entry)),
                                  tr("The save state is removed from all your devices.")) ==
            QMessageBox::Yes)
            Delete(entry);
    });
    actions->addWidget(load);
    actions->addWidget(remove);
    actions->addStretch();
    layout->addLayout(actions);
    return row;
}

void DekuiSaveStatesDialog::SaveNew() {
    if (!GameRunning() || !saving_path.empty())
        return;
    const auto path = DekuiSaveNewState(system);
    if (!path) {
        status->setText(tr("Another save state operation is still in progress."));
        return;
    }
    saving_path = *path;
    saving_polls = 0;
    status->clear();
    Rebuild();
    save_timer->start();
}

// The emulation thread writes the state over the next frames and the
// thumbnail is encoded separately; wait for both, for up to 15 seconds.
void DekuiSaveStatesDialog::PollSave() {
    const bool state_written = FileUtil::Exists(saving_path);
    const bool thumbnail_written =
        FileUtil::Exists(Core::SaveStateThumbnailPath(saving_path, false));
    if (!(state_written && thumbnail_written) && ++saving_polls < 60)
        return;
    save_timer->stop();
    saving_path.clear();
    Reload();
    status->setText(state_written ? tr("Saved.") : tr("The state was not saved."));
}

void DekuiSaveStatesDialog::Load(const Entry& entry) {
    if (!GameRunning())
        return;
    if (!system.SendFileSignal(Core::System::Signal::LoadFile, entry.path)) {
        status->setText(tr("Another save state operation is still in progress."));
        return;
    }
    system.frame_limiter.AdvanceFrame();
    // Return to the game, which shows the loaded state.
    accept();
}

void DekuiSaveStatesDialog::Delete(const Entry& entry) {
    if (!Core::DeleteSaveStateFile(entry.path))
        status->setText(tr("%1 could not be deleted.").arg(Name(entry)));
    Reload();
}
