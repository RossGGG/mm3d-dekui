// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import RealityKit
import SwiftUI
import UIKit

/// The game's save states, newest first, each with a thumbnail. Every New Save
/// State is its own file, so states never overwrite each other and sync
/// between devices without conflicts. States in the desktop app's numbered
/// slots (its hotkeys; slot 0 is Quick Save) are listed too.
struct SaveStatesView: View {
    @Environment(EmulatorModel.self) private var model
    @Environment(\.dismissWindow) private var dismissWindow

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
    private var gameRunning: Bool { model.state == .running || model.state == .paused }

    var body: some View {
        NavigationStack {
            List {
                Section {
                    if savingPath != nil {
                        Label("Saving…", systemImage: "square.and.arrow.down")
                            .foregroundStyle(.tint)
                    }
                    ForEach(states, id: \.path) { state in
                        row(state)
                    }
                } footer: {
                    if !states.isEmpty {
                        Text("Save states capture the whole emulator. They do not replace "
                            + "in-game saves, and may not load after an emulator update.")
                    }
                }
            }
            .overlay {
                if !gameRunning {
                    ContentUnavailableView("No game running", systemImage: "square.stack",
                                           description: Text("Save states are available while the game runs."))
                } else if states.isEmpty && savingPath == nil {
                    ContentUnavailableView("No save states", systemImage: "square.stack",
                                           description: Text("Use New Save State to save the game as it is now."))
                }
            }
            .navigationTitle("Save States")
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    Button("New Save State", systemImage: "plus") { save() }
                        .disabled(!gameRunning || savingPath != nil)
                }
                if let status {
                    ToolbarItem(placement: .status) { Text(status).foregroundStyle(.secondary) }
                }
            }
        }
        .onAppear {
            emulator.downloadCloudSaveStates()
            reload()
        }
        // Pick up states that iCloud delivers while the window is open.
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
        HStack(spacing: 16) {
            Thumbnail(url: state.thumbnail, stereo: state.thumbnailIsStereo, date: state.date)
            VStack(alignment: .leading, spacing: 4) {
                Text(name(state)).font(.headline)
                Text(state.date, format: .dateTime.month(.abbreviated).day().year())
                    .foregroundStyle(.secondary)
                Text(state.date, format: .dateTime.hour().minute().second())
                    .foregroundStyle(.secondary)
                if let incompatibility = state.incompatibility {
                    Label(incompatibility, systemImage: "exclamationmark.triangle")
                        .font(.caption)
                        .foregroundStyle(.orange)
                }
            }
            Spacer(minLength: 0)
            // Stacked at their natural size so the titles never wrap.
            VStack(alignment: .trailing, spacing: 8) {
                Button("Load", systemImage: "square.and.arrow.up") {
                    confirming = .load(state)
                }
                .disabled(!gameRunning || savingPath != nil || state.incompatibility != nil)
                Button("Delete", systemImage: "trash", role: .destructive) {
                    confirming = .delete(state)
                }
                .disabled(savingPath != nil)
            }
            .labelStyle(.titleAndIcon)
            .buttonStyle(.bordered)
            .fixedSize()
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
        // The emulation thread writes the state over the next frames and the
        // thumbnail is encoded separately; wait for both, for up to 15 seconds.
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
            savingPath = nil
            reload()
            status = saved ? "Saved." : "The state was not saved."
        }
    }

    private func load(_ state: DKSaveState) {
        if emulator.loadState(state) {
            // Back to the game, which shows the loaded state.
            dismissWindow()
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

/// A save state's picture. Reloads when the state is saved again.
private struct Thumbnail: View {
    let url: URL?
    let stereo: Bool
    let date: Date?

    var body: some View {
        Group {
            if let url, stereo {
                StereoThumbnail(url: url)
            } else if let url, let image = UIImage(contentsOfFile: url.path) {
                Image(uiImage: image)
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
        .frame(width: 192, height: 108)
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .id("\(url?.path ?? "")-\(date?.timeIntervalSince1970 ?? 0)")
    }
}

/// A side-by-side thumbnail shown in 3D: each eye sees its own half, through
/// the stereo game screen's per-eye material.
private struct StereoThumbnail: View {
    let url: URL

    /// RealityKit state kept outside SwiftUI's, so updates do not modify view state.
    private final class Plane {
        let root = Entity()
        let model = ModelEntity(mesh: .generatePlane(width: 1, height: 1))
        var size: SIMD2<Float> = .zero
        init() { root.addChild(model) }
    }

    @Environment(\.physicalMetrics) private var metrics
    @State private var plane = Plane()

    var body: some View {
        GeometryReader3D { proxy in
            RealityView { content in
                content.add(plane.root)
                guard let texture = try? await TextureResource(contentsOf: url),
                      var material = try? await ShaderGraphMaterial(
                          named: "/Root/StereoScreen",
                          from: Data(StereoScreen.materialSource.utf8))
                else { return }
                try? material.setParameter(name: "Frame", value: .textureResource(texture))
                plane.model.model?.materials = [material]
            } update: { content in
                // Lie on the window's surface, like the stereo game screen.
                let bounds = content.convert(proxy.frame(in: .local), from: .local, to: plane.root)
                let width = bounds.extents.x, height = bounds.extents.y
                guard width > 0, height > 0 else { return }
                plane.model.position = [bounds.center.x, bounds.center.y, bounds.min.z + 0.0002]
                guard SIMD2(width, height) != plane.size else { return }
                plane.size = SIMD2(width, height)
                let radius = Float(metrics.convert(CGFloat(12), to: .meters))
                plane.model.model?.mesh = MeshResource.generatePlane(
                    width: width, height: height, cornerRadius: min(radius, height / 2))
            }
        }
    }
}
