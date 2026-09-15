// Draws DevX's app icon and packs it into an .icns.
//
// Generated rather than committed, for the same reason nothing else in this
// repository is a binary asset (ADR-0002): the icon is reproducible from the
// code that draws it, a reviewer can see what it will look like by reading
// this, and there is no opaque blob in the tree that nobody can diff. It uses
// only CoreGraphics and `iconutil`, both of which ship with macOS.
//
// The design is the tool's own thesis rather than decoration. A profile of
// green bars -- measured work -- with one bar drawn as the cyan picket the
// timeline uses for NOT MEASURED. That is the distinction the whole project
// exists to hold: what was measured, and what was not, never the same mark.
//
//   swift tools/gen-icon.swift <output.icns>
import AppKit
import CoreGraphics
import Foundation

// The app's palette, from Theme.swift. Kept in step by hand, which is
// acceptable for two colours; a mismatch would be visible at a glance.
let ground = CGColor(red: 0.035, green: 0.047, blue: 0.041, alpha: 1)
let panel = CGColor(red: 0.067, green: 0.086, blue: 0.075, alpha: 1)
let line = CGColor(red: 0.16, green: 0.27, blue: 0.21, alpha: 1)
let green = CGColor(red: 0.31, green: 0.93, blue: 0.47, alpha: 1)
let cyan = CGColor(red: 0.38, green: 0.84, blue: 0.95, alpha: 1)

/// A bar profile: heights as a fraction of the plot area, and which slot was
/// never measured. Two profiles, because an icon is drawn at sizes two orders
/// of magnitude apart and eight bars at 16 px is a smudge. The small profile
/// keeps the one thing that has to survive -- a run of measured bars and one
/// slot that is visibly not one of them.
struct Profile {
    let bars: [Double]
    let unmeasured: Int
}
let detailed = Profile(bars: [0.34, 0.62, 0.45, 0.88, 0.0, 0.52, 0.71, 0.29],
                       unmeasured: 4)
let compact = Profile(bars: [0.45, 0.82, 0.0, 0.58], unmeasured: 2)

func drawIcon(size: Int) -> CGImage? {
    let s = CGFloat(size)
    guard let ctx = CGContext(
        data: nil, width: size, height: size, bitsPerComponent: 8,
        bytesPerRow: 0, space: CGColorSpace(name: CGColorSpace.sRGB)!,
        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else {
        return nil
    }

    // macOS icons sit inside their canvas rather than filling it, and the
    // corner radius scales with the size.
    let inset = s * 0.055
    let rect = CGRect(x: inset, y: inset, width: s - inset * 2, height: s - inset * 2)
    let radius = rect.width * 0.22

    let body = CGPath(roundedRect: rect, cornerWidth: radius,
                      cornerHeight: radius, transform: nil)
    ctx.addPath(body)
    ctx.setFillColor(ground)
    ctx.fillPath()

    // A hairline border, the same device the window chrome uses instead of
    // shadows.
    ctx.addPath(body)
    ctx.setStrokeColor(line)
    ctx.setLineWidth(max(1, s * 0.008))
    ctx.strokePath()

    // The plot area, inset inside the body, with its own edge so it reads as a
    // panel rather than as a lighter patch of the ground.
    let pad = rect.width * 0.14
    let plot = rect.insetBy(dx: pad, dy: pad)
    ctx.addPath(CGPath(rect: plot, transform: nil))
    ctx.setFillColor(panel)
    ctx.fillPath()
    if s >= 64 {
        ctx.addPath(CGPath(rect: plot, transform: nil))
        ctx.setStrokeColor(line)
        ctx.setLineWidth(max(1, s * 0.005))
        ctx.strokePath()
    }

    // A baseline: the bars stand on something, which is what makes a measured
    // zero readable as a mark rather than as absence.
    ctx.setStrokeColor(line)
    ctx.setLineWidth(max(1, s * 0.006))
    ctx.move(to: CGPoint(x: plot.minX, y: plot.minY))
    ctx.addLine(to: CGPoint(x: plot.maxX, y: plot.minY))
    ctx.strokePath()

    let profile = s >= 128 ? detailed : compact
    let slot = plot.width / CGFloat(profile.bars.count)
    let barWidth = slot * 0.60
    let gap = (slot - barWidth) / 2

    for (i, fraction) in profile.bars.enumerated() {
        let x = plot.minX + slot * CGFloat(i) + gap
        if i == profile.unmeasured {
            drawUnmeasured(ctx, x: x, width: barWidth, plot: plot, s: s)
            continue
        }
        let height = max(plot.height * CGFloat(fraction), max(1, s * 0.012))
        ctx.setFillColor(green)
        ctx.fill(CGRect(x: x, y: plot.minY, width: barWidth, height: height))
    }

    return ctx.makeImage()
}

/// The NOT MEASURED slot.
///
/// This is the part of the icon that has to be got right, and the first attempt
/// got it wrong: a full-height cyan column simply read as the biggest bar,
/// which is the exact claim the tool refuses to make. So it is drawn as the
/// absence of a bar -- a dashed outline around an empty column, with sparse
/// pickets inside. Nothing about it has a top edge at a height, because a top
/// edge at a height is a value.
func drawUnmeasured(_ ctx: CGContext, x: CGFloat, width: CGFloat,
                    plot: CGRect, s: CGFloat) {
    let col = CGRect(x: x, y: plot.minY, width: width, height: plot.height)
    ctx.setFillColor(cyan.copy(alpha: 0.07)!)
    ctx.fill(col)

    ctx.setStrokeColor(cyan.copy(alpha: 0.9)!)
    let stroke = max(1, s * 0.009)
    ctx.setLineWidth(stroke)

    // One picket, dashed, at the centre -- enough to say the column is drawn
    // rather than empty, without turning into texture. Below 128 px even that
    // becomes speckle, so the outline carries it alone.
    if s >= 128 {
        ctx.setLineDash(phase: 0, lengths: [s * 0.075, s * 0.05])
        ctx.move(to: CGPoint(x: x + width / 2, y: plot.minY + stroke))
        ctx.addLine(to: CGPoint(x: x + width / 2, y: plot.maxY - stroke))
        ctx.strokePath()
        ctx.setLineDash(phase: 0, lengths: [])
    }

    // The outline: dashed, so the column is bounded but never closed off at a
    // level. Long dashes -- a handful of segments per side, not a stipple.
    let dash = s >= 128 ? s * 0.10 : s * 0.16
    ctx.setLineDash(phase: 0, lengths: [dash, dash * 0.62])
    ctx.stroke(col.insetBy(dx: stroke / 2, dy: stroke / 2))
    ctx.setLineDash(phase: 0, lengths: [])
}

func writePNG(_ image: CGImage, to url: URL) throws {
    let rep = NSBitmapImageRep(cgImage: image)
    guard let data = rep.representation(using: .png, properties: [:]) else {
        throw NSError(domain: "gen-icon", code: 1,
                      userInfo: [NSLocalizedDescriptionKey: "PNG encode failed"])
    }
    try data.write(to: url)
}

// --- main -------------------------------------------------------------------

let args = CommandLine.arguments
guard args.count > 1 else {
    FileHandle.standardError.write(
        "usage: swift tools/gen-icon.swift <output.icns>\n".data(using: .utf8)!)
    exit(2)
}
let output = URL(fileURLWithPath: args[1])

let work = URL(fileURLWithPath: NSTemporaryDirectory())
    .appendingPathComponent("devx-icon-\(getpid()).iconset")
try? FileManager.default.removeItem(at: work)
try FileManager.default.createDirectory(at: work, withIntermediateDirectories: true)

// The set `iconutil` expects. Each logical size has a 1x and a 2x file.
let plan: [(name: String, pixels: Int)] = [
    ("icon_16x16", 16), ("icon_16x16@2x", 32),
    ("icon_32x32", 32), ("icon_32x32@2x", 64),
    ("icon_128x128", 128), ("icon_128x128@2x", 256),
    ("icon_256x256", 256), ("icon_256x256@2x", 512),
    ("icon_512x512", 512), ("icon_512x512@2x", 1024),
]
for entry in plan {
    guard let image = drawIcon(size: entry.pixels) else {
        FileHandle.standardError.write(
            "could not draw \(entry.pixels)px\n".data(using: .utf8)!)
        exit(1)
    }
    try writePNG(image, to: work.appendingPathComponent("\(entry.name).png"))
}

let task = Process()
task.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
task.arguments = ["-c", "icns", work.path, "-o", output.path]
try task.run()
task.waitUntilExit()
try? FileManager.default.removeItem(at: work)
if task.terminationStatus != 0 {
    FileHandle.standardError.write("iconutil failed\n".data(using: .utf8)!)
    exit(Int32(task.terminationStatus))
}
print("wrote \(output.path)")
