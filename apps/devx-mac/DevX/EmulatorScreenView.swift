import AppKit
import SwiftUI

/// The emulator's screen, drawn from the file the emulator writes frames into.
///
/// The emulator streams RGBA8888 frames into a shared file (the gRPC MMAP
/// transport); the C ABI says when a new one has landed. Every display refresh
/// this view checks the sequence number and, when it moved, copies the frame
/// out and hands it to its layer -- copied, because the emulator overwrites the
/// file in place and a layer pointing into it would tear.
///
/// Input goes the other way: a click or drag becomes touches at device
/// pixels, a scroll becomes a short drag, and keys become key presses.
struct EmulatorScreen: NSViewRepresentable {
    let handle: Int
    let path: String
    let box: Int
    /// The device's own pixel size in its natural orientation.
    let deviceWidth: Int
    let deviceHeight: Int
    /// Called once when the session ends under the view: the emulator
    /// stopped or restarted.
    var onLost: () -> Void = {}

    func makeNSView(context: Context) -> EmulatorScreenNSView {
        let v = EmulatorScreenNSView()
        v.onLost = onLost
        v.handle = handle
        v.attach(path: path, box: box, deviceWidth: deviceWidth, deviceHeight: deviceHeight)
        return v
    }

    func updateNSView(_ v: EmulatorScreenNSView, context: Context) {
        v.onLost = onLost
        v.handle = handle
        v.attach(path: path, box: box, deviceWidth: deviceWidth, deviceHeight: deviceHeight)
    }

    static func dismantleNSView(_ v: EmulatorScreenNSView, coordinator: ()) { v.detach() }
}

final class EmulatorScreenNSView: NSView {
    private var path = ""
    private var box = 0
    private var deviceSize = CGSize.zero
    private var fd: Int32 = -1
    private var mapped: UnsafeMutableRawPointer?
    private var mappedLength = 0
    private var timer: Timer?
    private var lastSeq: UInt32 = .max
    private var frameSize = CGSize.zero
    private var touching = false
    private var scrollPoint: CGPoint?
    private var scrollStartedAt: CGPoint?
    var onLost: () -> Void = {}
    /// The display session this view draws and sends input to.
    var handle = 0

    override var acceptsFirstResponder: Bool { true }
    override var isFlipped: Bool { true }

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
        layer?.contentsGravity = .resizeAspect
        layer?.magnificationFilter = .linear
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    func attach(path: String, box: Int, deviceWidth: Int, deviceHeight: Int) {
        deviceSize = CGSize(width: deviceWidth, height: deviceHeight)
        guard path != self.path || box != self.box else { return }
        detach()
        self.path = path
        self.box = box
        fd = open(path, O_RDONLY)
        guard fd >= 0 else { return }
        mappedLength = box * box * 4
        let p = mmap(nil, mappedLength, PROT_READ, MAP_SHARED, fd, 0)
        guard p != MAP_FAILED else { close(fd); fd = -1; return }
        mapped = p
        lastSeq = .max
        timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in
            self?.tick()
        }
        RunLoop.main.add(timer!, forMode: .common)
    }

    func detach() {
        timer?.invalidate()
        timer = nil
        if let m = mapped { munmap(m, mappedLength) }
        mapped = nil
        if fd >= 0 { close(fd) }
        fd = -1
        path = ""
        box = 0
    }

    deinit { detach() }

    private func tick() {
        guard let base = mapped else { return }
        guard let f = Core.displayFrame(handle) else {
            // The session ended: stop polling and let the owner reconnect.
            timer?.invalidate()
            timer = nil
            onLost()
            return
        }
        guard f.seq != lastSeq, f.width > 0, f.height > 0,
              Int(f.width) * Int(f.height) * 4 <= mappedLength else { return }
        lastSeq = f.seq
        let w = Int(f.width), h = Int(f.height)
        let data = Data(bytes: base, count: w * h * 4)
        guard let provider = CGDataProvider(data: data as CFData),
              let image = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32,
                                  bytesPerRow: w * 4, space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
                                  provider: provider, decode: nil, shouldInterpolate: true,
                                  intent: .defaultIntent) else { return }
        frameSize = CGSize(width: w, height: h)
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        layer?.contents = image
        CATransaction.commit()
    }

    // ---- input ----

    /// Where the frame is drawn inside the view (aspect fit).
    private var imageRect: CGRect {
        guard frameSize.width > 0, frameSize.height > 0 else { return .zero }
        let scale = min(bounds.width / frameSize.width, bounds.height / frameSize.height)
        let w = frameSize.width * scale, h = frameSize.height * scale
        return CGRect(x: (bounds.width - w) / 2, y: (bounds.height - h) / 2, width: w, height: h)
    }

    /// A view point as device pixels in the screen's current orientation, or
    /// nil outside the image.
    private func devicePoint(_ p: CGPoint) -> (Int, Int)? {
        let r = imageRect
        guard r.width > 0, r.contains(p) else { return nil }
        let landscape = frameSize.width > frameSize.height
        let natural = deviceSize.width > 0 ? deviceSize : frameSize
        // The device's pixels in the orientation the frame is in.
        let dw = landscape == (natural.width > natural.height) ? natural.width : natural.height
        let dh = landscape == (natural.width > natural.height) ? natural.height : natural.width
        let x = (p.x - r.minX) / r.width * dw
        let y = (p.y - r.minY) / r.height * dh
        return (Int(x.rounded()), Int(y.rounded()))
    }

    private func point(_ e: NSEvent) -> CGPoint { convert(e.locationInWindow, from: nil) }

    override func mouseDown(with e: NSEvent) {
        window?.makeFirstResponder(self)
        guard let (x, y) = devicePoint(point(e)) else { return }
        touching = true
        Core.touch(handle, x: x, y: y, pressure: 1024)
    }

    override func mouseDragged(with e: NSEvent) {
        guard touching, let (x, y) = devicePoint(point(e)) else { return }
        Core.touch(handle, x: x, y: y, pressure: 1024)
    }

    override func mouseUp(with e: NSEvent) {
        guard touching else { return }
        touching = false
        let (x, y) = devicePoint(point(e)) ?? (0, 0)
        Core.touch(handle, x: x, y: y, pressure: 0)
    }

    /// A scroll is a drag: the content follows the fingers, as on the device.
    override func scrollWheel(with e: NSEvent) {
        let p = point(e)
        let dy = e.hasPreciseScrollingDeltas ? e.scrollingDeltaY : e.scrollingDeltaY * 12
        let dx = e.hasPreciseScrollingDeltas ? e.scrollingDeltaX : e.scrollingDeltaX * 12
        if e.phase == .began || (e.phase == [] && e.momentumPhase == []) {
            scrollStartedAt = p
            scrollPoint = p
            if let (x, y) = devicePoint(p) { Core.touch(handle, x: x, y: y, pressure: 1024) }
        }
        guard var cur = scrollPoint else { return }
        cur.x += dx
        cur.y += dy
        scrollPoint = cur
        if let (x, y) = devicePoint(cur) { Core.touch(handle, x: x, y: y, pressure: 1024) }
        let ended = e.phase == .ended || e.phase == .cancelled || (e.phase == [] && e.momentumPhase == [])
        if ended {
            let (x, y) = devicePoint(cur) ?? devicePoint(scrollStartedAt ?? p) ?? (0, 0)
            Core.touch(handle, x: x, y: y, pressure: 0)
            scrollPoint = nil
        }
    }

    override func keyDown(with e: NSEvent) {
        switch e.keyCode {
        case 36, 76: Core.key(handle, "Enter")
        case 51: Core.key(handle, "Backspace")
        case 117: Core.key(handle, "Delete")
        case 48: Core.key(handle, "Tab")
        case 53: Core.key(handle, "GoBack")       // Esc acts as Back, as in Android Studio
        case 123: Core.key(handle, "ArrowLeft")
        case 124: Core.key(handle, "ArrowRight")
        case 125: Core.key(handle, "ArrowDown")
        case 126: Core.key(handle, "ArrowUp")
        default:
            if let s = e.characters, !s.isEmpty,
               s.unicodeScalars.allSatisfy({ $0.value >= 32 && $0.value < 127 }) {
                Core.text(handle, s)
            }
        }
    }
}
