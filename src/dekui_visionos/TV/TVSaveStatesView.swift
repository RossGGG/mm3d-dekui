// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI
import UIKit

/// The game's save states, newest first. Selecting one loads it; New Save
/// State saves the game as it is behind the menu.
struct TVSaveStatesView: View {
    @Environment(TVModel.self) private var model

    private enum Action: Identifiable {
        case load(DKSaveState), delete(DKSaveState)
        var id: String {
            switch self {
            case .load(let state): return "load-\(state.path)"
            case .delete(let state): return "delete-\(state.path)"
            }
        }
    }

    @State private var states: [DKSaveState] = []
    @State private var confirming: Action?
    /// The state being written, until it and its thumbnail exist.
    @State private var savingPath: String?
    @State private var status: String?

    private var emulator: DKEmulator { model.emulator }

    var body: some View {
        List {
            Section {
                Button {
                    save()
                } label: {
                    Label(savingPath == nil ? "New Save State" : "Saving…",
                          systemImage: "square.and.arrow.down")
                }
                .disabled(!model.gameRunning || savingPath != nil)
                if let status {
                    Text(status).foregroundStyle(.secondary)
                }
            }
            Section {
                ForEach(states, id: \.path) { state in
                    row(state)
                }
            } footer: {
                Text(states.isEmpty
                     ? "No save states yet."
                     : "Select a state to load it; press and hold to delete it. Save states "
                        + "capture the whole emulator. They do not replace in-game saves, and may "
                        + "not load after an emulator update.")
            }
        }
        .navigationTitle("Save States")
        .onAppear {
            // Fetches states saved on other devices through iCloud (CloudKit).
            emulator.downloadCloudSaveStates()
            reload()
        }
        // Picks up states that arrive while the list is open.
        .task {
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(5))
                if savingPath == nil { reload() }
            }
        }
        .onChange(of: model.state) { reload() }
        .alert(item: $confirming) { action in
            switch action {
            case .load(let state):
                return Alert(
                    title: Text("Load \(name(state))?"),
                    message: Text("Progress since that state was saved will be lost."),
                    primaryButton: .destructive(Text("Load")) { load(state) },
                    secondaryButton: .cancel())
            case .delete(let state):
                return Alert(
                    title: Text("Delete \(name(state))?"),
                    message: Text("The save state is removed from all your devices."),
                    primaryButton: .destructive(Text("Delete")) { delete(state) },
                    secondaryButton: .cancel())
            }
        }
    }

    private func row(_ state: DKSaveState) -> some View {
        Button {
            confirming = .load(state)
        } label: {
            HStack(spacing: 32) {
                Thumbnail(url: state.thumbnail, stereo: state.thumbnailIsStereo, date: state.date)
                VStack(alignment: .leading, spacing: 6) {
                    Text(name(state)).font(.headline)
                    Text(state.date, format: .dateTime.month(.abbreviated).day().year().hour().minute())
                        .foregroundStyle(.secondary)
                    if let incompatibility = state.incompatibility {
                        Label(incompatibility, systemImage: "exclamationmark.triangle")
                            .font(.caption)
                            .foregroundStyle(.orange)
                    }
                }
            }
        }
        .disabled(!model.gameRunning || savingPath != nil || state.incompatibility != nil)
        .contextMenu {
            Button("Delete", systemImage: "trash", role: .destructive) {
                confirming = .delete(state)
            }
        }
    }

    private func name(_ state: DKSaveState) -> String {
        switch state.slot {
        case 0: return "Quick Save"
        case 1...: return "Slot \(state.slot)"
        default: return state.date.formatted(date: .abbreviated, time: .shortened)
        }
    }

    private func reload() {
        states = emulator.saveStates()
    }

    private func save() {
        guard let path = emulator.saveNewState() else {
            status = "Another save state operation is still in progress."
            return
        }
        savingPath = path
        status = nil
        // The emulation thread writes the state over the next frames (the
        // bridge resumes a paused game for that) and the thumbnail is encoded
        // separately; wait for both, for up to 15 seconds.
        Task { @MainActor in
            var saved = false
            for _ in 0..<60 {
                try? await Task.sleep(for: .milliseconds(250))
                guard let state = emulator.saveStates().first(where: { $0.path == path }) else {
                    continue
                }
                saved = true
                if state.thumbnail != nil { break }
            }
            if model.menuShown && model.state == .running {
                emulator.pause()
            }
            savingPath = nil
            reload()
            status = saved ? "Saved." : "The state was not saved."
        }
    }

    private func load(_ state: DKSaveState) {
        if emulator.loadState(state) {
            // Back to the game, which shows the loaded state.
            model.menuShown = false
        } else {
            status = "Another save state operation is still in progress."
        }
    }

    private func delete(_ state: DKSaveState) {
        if !emulator.deleteState(state) {
            status = "\(name(state)) could not be deleted."
        }
        reload()
    }
}

/// A save state's picture; a 3D one shows its left eye.
private struct Thumbnail: View {
    let url: URL?
    let stereo: Bool
    let date: Date?

    var body: some View {
        Group {
            if let url, let image = UIImage(contentsOfFile: url.path) {
                Image(uiImage: stereo ? leftHalf(image) : image)
                    .resizable()
                    .aspectRatio(contentMode: .fill)
            } else {
                Image(systemName: "photo")
                    .font(.title)
                    .foregroundStyle(.tertiary)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .background(.quaternary)
            }
        }
        .frame(width: 256, height: 144)
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .id("\(url?.path ?? "")-\(date?.timeIntervalSince1970 ?? 0)")
    }

    private func leftHalf(_ image: UIImage) -> UIImage {
        guard let cg = image.cgImage,
              let left = cg.cropping(to: CGRect(x: 0, y: 0, width: cg.width / 2, height: cg.height))
        else { return image }
        return UIImage(cgImage: left)
    }
}
