// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDateTime>
#include <QDialog>
#include <optional>
#include <string>
#include <vector>
#include "common/common_types.h"

class QLabel;
class QPushButton;
class QTimer;
class QVBoxLayout;

namespace Core {
class System;
}

/// Saves a thumbnail of the next frame beside a state file (Core::SaveStateThumbnailPath).
void DekuiCaptureStateThumbnail(Core::System& system, const std::string& state_path);

/// Saves the game to a new state file, with a thumbnail, and returns its path.
/// Nothing when no game runs or another save state operation is pending.
std::optional<std::string> DekuiSaveNewState(Core::System& system);

/// Loads the newest state this build can load. Returns false when there is
/// none or another save state operation is pending.
bool DekuiLoadLatestState(Core::System& system);

/// Desktop counterpart of the visionOS Save States window
/// (dekui_visionos/App/SaveStatesView.swift): the game's save states, newest
/// first, each with a thumbnail. Every New Save State is its own file, so
/// states never overwrite each other and sync between devices without
/// conflicts. States in Citra's numbered slots (its hotkeys) are listed too.
class DekuiSaveStatesDialog final : public QDialog {
    Q_OBJECT

public:
    DekuiSaveStatesDialog(Core::System& system, QWidget* parent);
    ~DekuiSaveStatesDialog() override;

private:
    struct Entry {
        std::string path;
        std::optional<u32> slot;
        QDateTime date;
        QString incompatibility; ///< Set when another build made it.
        QString thumbnail;       ///< Path, or empty.
        bool stereo = false;     ///< Both eyes side by side (saved with 3D on).
    };

    void Reload();
    void Rebuild();
    QWidget* Row(const Entry& entry);
    void SaveNew();
    void Load(const Entry& entry);
    void Delete(const Entry& entry);
    void PollSave();
    QString Name(const Entry& entry) const;
    bool GameRunning() const;

    Core::System& system;
    std::vector<Entry> entries;
    QVBoxLayout* rows = nullptr;
    QLabel* status = nullptr;
    QPushButton* new_state = nullptr;
    QTimer* save_timer = nullptr;
    std::string saving_path; ///< Empty unless a save is in progress.
    int saving_polls = 0;
};
