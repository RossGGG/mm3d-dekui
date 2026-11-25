// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI
import UniformTypeIdentifiers

/// Shown until the Dekui folder holds the supported game and a prepared user folder.
struct SetupView: View {
    @Environment(EmulatorModel.self) private var model
    let message: String

    private enum ImportKind { case game, userFolder }
    @State private var importKind = ImportKind.game
    @State private var importing = false

    private static let gameTypes: [UTType] = ["cci", "3ds"].compactMap {
        UTType(filenameExtension: $0)
    } + [.data]

    var body: some View {
        VStack(alignment: .leading, spacing: 24) {
            Label("Set up Dekui", systemImage: "shippingbox")
                .font(.largeTitle)
            Text(message)
                .font(.title3)
                .fixedSize(horizontal: false, vertical: true)
            VStack(alignment: .leading, spacing: 8) {
                Text("1. Import one decrypted USA v1.1 game-card dump (.cci or .3ds).")
                Text("2. Import the user folder that Dekui setup prepared on a desktop.")
                Text("You can also copy both into Files › On My Apple Vision Pro › Dekui.")
                    .foregroundStyle(.secondary)
            }
            HStack(spacing: 16) {
                Button("Import Game…", systemImage: "square.and.arrow.down") {
                    importKind = .game
                    importing = true
                }
                Button("Import User Folder…", systemImage: "folder.badge.plus") {
                    importKind = .userFolder
                    importing = true
                }
                Button("Check Again", systemImage: "arrow.clockwise") {
                    model.checkSetup()
                }
            }
        }
        .padding(48)
        .frame(minWidth: 720, minHeight: 420)
        // One importer: SwiftUI ignores all but one fileImporter per view.
        .fileImporter(
            isPresented: $importing,
            allowedContentTypes: importKind == .userFolder ? [.folder] : Self.gameTypes
        ) { result in
            guard case .success(let url) = result else { return }
            if importKind == .userFolder {
                model.importUserFolder(from: url)
            } else {
                model.importGame(from: url)
            }
        }
    }
}
