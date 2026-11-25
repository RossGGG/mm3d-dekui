// Renders an SVG to a PNG with a transparent background, using AppKit's SVG support.
// usage: swift render_svg.swift <in.svg> <out.png> <width> <height>
import AppKit

let args = CommandLine.arguments
guard args.count == 5, let image = NSImage(contentsOf: URL(fileURLWithPath: args[1])),
      let width = Int(args[3]), let height = Int(args[4]) else {
    FileHandle.standardError.write("usage: render_svg <in.svg> <out.png> <width> <height>\n".data(using: .utf8)!)
    exit(1)
}
let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: width, pixelsHigh: height,
                              bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                              colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
NSGraphicsContext.current?.imageInterpolation = .high
image.draw(in: NSRect(x: 0, y: 0, width: width, height: height))
NSGraphicsContext.restoreGraphicsState()
try! bitmap.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: args[2]))
