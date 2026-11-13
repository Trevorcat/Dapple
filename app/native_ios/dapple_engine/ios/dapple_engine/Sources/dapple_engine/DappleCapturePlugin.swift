import AVFoundation
import CoreLocation
import Flutter
import ImageIO
import UIKit
import UniformTypeIdentifiers

// Phone camera capture on the channel the Android side uses (docs/ios/swift-designs.md section 1):
//   dapple/capture  available -> bool
//                   permission {request} -> "granted" | "denied" | "blocked"
//                   openSettings -> bool
//                   capabilities -> {device, os, build, lenses: [...]}   (what each lens really offers, probed, never by model name)
//                   capture -> {jpg?, dng?, thumb?, source: "avfoundation", mode, lens} | nil (closed without a shot)
//                   autoShutter {on, delayMs}   test runs: the shutter presses itself after the delay (nobody can tap from the PC)
// The capture screen is a native full-screen view controller (AVCaptureVideoPreviewLayer: upright, zero copies), like CaptureActivity.

// MARK: capability probe

enum CaptureMode: String {
    case rawJpeg = "raw+jpeg", raw = "raw", jpeg = "jpeg"
    var lower: CaptureMode? { self == .rawJpeg ? .raw : (self == .raw ? .jpeg : nil) }
}

struct LensCaps {
    var id: String, label: String, position: String, type: String
    var bayer: [String] = [], proRaw = false, rawThumbJpeg = false
    var maxW = 0, maxH = 0
    var fov: Float = 0
    var rawFormat: OSType? { proRawFormat ?? bayerFormat }
    var bayerFormat: OSType?, proRawFormat: OSType?

    /// The ladder for this lens, best first (RAW+JPEG -> RAW -> JPEG; a lens without RAW has JPEG only).
    var modes: [CaptureMode] { rawFormat == nil ? [.jpeg] : [.rawJpeg, .raw, .jpeg] }
    var rawKind: String { proRawFormat != nil ? "proraw" : (bayerFormat != nil ? "bayer" : "none") }

    var json: [String: Any] {
        ["id": id, "label": label, "position": position, "type": type, "rawKind": rawKind, "proRaw": proRaw, "bayerRaw": bayerFormat != nil,
         "rawThumbJpeg": rawThumbJpeg, "maxPixels": "\(maxW)x\(maxH)", "fov": Double(fov), "modes": modes.map { $0.rawValue }]
    }
}

enum CaptureCaps {
    static func back() -> [AVCaptureDevice] {
        let types: [AVCaptureDevice.DeviceType] = [.builtInUltraWideCamera, .builtInWideAngleCamera, .builtInTelephotoCamera]
        let found = AVCaptureDevice.DiscoverySession(deviceTypes: types, mediaType: .video, position: .back).devices
        return types.compactMap { t in found.first { $0.deviceType == t } }  // ultra-wide, wide, tele in this order
    }

    static func front() -> AVCaptureDevice? {
        AVCaptureDevice.DiscoverySession(deviceTypes: [.builtInWideAngleCamera], mediaType: .video, position: .front).devices.first
    }

    static var available: Bool {
        #if targetEnvironment(simulator)
        return false
        #else
        return !back().isEmpty || front() != nil
        #endif
    }

    static func label(_ d: AVCaptureDevice, wide: AVCaptureDevice?) -> String {
        if d.position == .front { return "前" }
        guard let w = wide, d !== w else { return "1×" }
        if d.deviceType == .builtInUltraWideCamera { return "0.5×" }
        // the field of view of the active format moves a little with the session: round to whole steps (2×, 3×, 5×), never below 2× for a tele
        let r = tan(Double(w.activeFormat.videoFieldOfView) * .pi / 360) / tan(Double(d.activeFormat.videoFieldOfView) * .pi / 360)
        return String(format: "%g×", max(2, r.rounded()))
    }

    static func typeName(_ d: AVCaptureDevice) -> String {
        switch d.deviceType {
        case .builtInUltraWideCamera: return "ultra-wide"
        case .builtInTelephotoCamera: return "tele"
        default: return "wide"
        }
    }

    /// Needs camera permission (an input cannot be created without it). Builds a throw-away session so the output reports its real formats.
    static func probe(_ d: AVCaptureDevice, wide: AVCaptureDevice?) -> LensCaps {
        var c = LensCaps(id: d.uniqueID, label: label(d, wide: wide), position: d.position == .front ? "front" : "back", type: typeName(d))
        c.fov = d.activeFormat.videoFieldOfView
        let s = AVCaptureSession()
        let out = AVCapturePhotoOutput()
        s.beginConfiguration()
        s.sessionPreset = .photo
        guard let input = try? AVCaptureDeviceInput(device: d), s.canAddInput(input), s.canAddOutput(out) else { s.commitConfiguration(); return c }
        s.addInput(input)
        s.addOutput(out)
        s.commitConfiguration()
        c.bayerFormat = out.availableRawPhotoPixelFormatTypes.first { AVCapturePhotoOutput.isBayerRAWPixelFormat($0) }
        c.bayer = out.availableRawPhotoPixelFormatTypes.filter { AVCapturePhotoOutput.isBayerRAWPixelFormat($0) }.map { fourCC($0) }
        if out.isAppleProRAWSupported {
            out.isAppleProRAWEnabled = true
            c.proRaw = true
            c.proRawFormat = out.availableRawPhotoPixelFormatTypes.first { AVCapturePhotoOutput.isAppleProRAWPixelFormat($0) }
        }
        if let f = c.rawFormat { c.rawThumbJpeg = AVCapturePhotoSettings(rawPixelFormatType: f).availableRawEmbeddedThumbnailPhotoCodecTypes.contains(.jpeg) }
        if let m = d.activeFormat.supportedMaxPhotoDimensions.max(by: { Int($0.width) * Int($0.height) < Int($1.width) * Int($1.height) }) { c.maxW = Int(m.width); c.maxH = Int(m.height) }
        return c
    }

    static func fourCC(_ t: OSType) -> String {
        String(bytes: [UInt8((t >> 24) & 255), UInt8((t >> 16) & 255), UInt8((t >> 8) & 255), UInt8(t & 255)], encoding: .ascii) ?? "\(t)"
    }

    static var osBuild: String {
        var n = 0
        sysctlbyname("kern.osversion", nil, &n, nil, 0)
        var b = [CChar](repeating: 0, count: max(n, 1))
        sysctlbyname("kern.osversion", &b, &n, nil, 0)
        return String(cString: b)
    }

    static var machine: String {
        var u = utsname()
        uname(&u)
        return withUnsafePointer(to: &u.machine) { $0.withMemoryRebound(to: CChar.self, capacity: 1) { String(cString: $0) } }
    }

    static func report() -> [String: Any] {
        var m: [String: Any] = ["device": machine, "os": UIDevice.current.systemVersion, "build": osBuild, "available": available]
        guard AVCaptureDevice.authorizationStatus(for: .video) == .authorized else { m["lenses"] = []; m["note"] = "camera permission not granted"; return m }
        let b = back()
        let wide = b.first { $0.deviceType == .builtInWideAngleCamera }
        var lenses = b.map { probe($0, wide: wide).json }
        if let f = front() { lenses.append(probe(f, wide: nil).json) }
        m["lenses"] = lenses
        return m
    }
}

// MARK: plugin

public final class DappleCapturePlugin: NSObject {
    private var pending: FlutterResult?
    static var autoShutterMs = 0  // > 0: the shutter presses itself after this many ms (test runs)
    static var forcedMode: CaptureMode?  // test runs: start the ladder at this mode

    init(registrar: FlutterPluginRegistrar) {
        super.init()
        FlutterMethodChannel(name: "dapple/capture", binaryMessenger: registrar.messenger()).setMethodCallHandler(handle)
    }

    private func handle(_ call: FlutterMethodCall, _ result: @escaping FlutterResult) {
        let a = call.arguments as? [String: Any] ?? [:]
        switch call.method {
        case "available":
            result(CaptureCaps.available)
        case "permission":
            switch AVCaptureDevice.authorizationStatus(for: .video) {
            case .authorized: result("granted")
            case .notDetermined:
                if (a["request"] as? Bool) == true {
                    AVCaptureDevice.requestAccess(for: .video) { ok in DispatchQueue.main.async { result(ok ? "granted" : "blocked") } }
                } else { result("denied") }
            default: result("blocked")  // denied / restricted: only Settings (or the parent) can change it
            }
        case "openSettings":
            guard let u = URL(string: UIApplication.openSettingsURLString) else { return result(false) }
            UIApplication.shared.open(u, options: [:]) { ok in result(ok) }
        case "capabilities":
            DispatchQueue.global(qos: .userInitiated).async { let r = CaptureCaps.report(); DispatchQueue.main.async { result(r) } }
        case "autoShutter":
            Self.autoShutterMs = (a["on"] as? Bool) == true ? ((a["delayMs"] as? Int) ?? 5000) : 0
            Self.forcedMode = (a["mode"] as? String).flatMap { CaptureMode(rawValue: $0) }
            result(nil)
        case "capture":
            guard pending == nil else { return result(FlutterError(code: "busy", message: nil, details: nil)) }
            guard AVCaptureDevice.authorizationStatus(for: .video) == .authorized else { return result(FlutterError(code: "permission", message: nil, details: nil)) }
            guard CaptureCaps.available else { return result(FlutterError(code: "capture", message: "no camera", details: nil)) }
            guard let top = Self.topViewController() else { return result(FlutterError(code: "capture", message: "no view controller", details: nil)) }
            pending = result
            let vc = CaptureViewController()
            vc.modalPresentationStyle = .fullScreen
            vc.autoShutterMs = Self.autoShutterMs
            vc.onFinish = { [weak self] r in
                guard let self = self, let p = self.pending else { return }
                self.pending = nil
                p(r)
            }
            top.present(vc, animated: true)
        default:
            result(FlutterMethodNotImplemented)
        }
    }

    static func topViewController() -> UIViewController? {
        var vc = UIApplication.shared.connectedScenes.compactMap { ($0 as? UIWindowScene)?.windows.first(where: { $0.isKeyWindow })?.rootViewController }.first
        while let p = vc?.presentedViewController { vc = p }
        return vc
    }
}

// MARK: capture screen

private func clog(_ s: String) {
    NSLog("dapple capture: %@", s)
    // test runs read this file back (apps pull Documents/capture_log.txt); it is small and only written when a test run is active
    guard DappleCapturePlugin.autoShutterMs > 0, let dir = NSSearchPathForDirectoriesInDomains(.documentDirectory, .userDomainMask, true).first else { return }
    let line = "\(ISO8601DateFormatter().string(from: Date())) \(s)\n"
    let url = URL(fileURLWithPath: dir + "/capture_log.txt")
    if let h = try? FileHandle(forWritingTo: url) { h.seekToEndOfFile(); h.write(Data(line.utf8)); try? h.close() } else { try? line.write(to: url, atomically: true, encoding: .utf8) }
}

final class PreviewView: UIView {
    override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }
    var previewLayer: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }
}

final class ShutterButton: UIControl {
    override init(frame: CGRect) { super.init(frame: frame); backgroundColor = .clear }
    required init?(coder: NSCoder) { fatalError() }
    override var isHighlighted: Bool { didSet { setNeedsDisplay() } }
    override var isEnabled: Bool { didSet { alpha = isEnabled ? 1 : 0.4 } }
    override func draw(_ rect: CGRect) {
        let w = bounds.width
        UIColor.white.setStroke()
        let ring = UIBezierPath(ovalIn: bounds.insetBy(dx: w * 0.025, dy: w * 0.025))
        ring.lineWidth = w * 0.05
        ring.stroke()
        UIColor.white.withAlphaComponent(isHighlighted ? 0.7 : 1).setFill()
        UIBezierPath(ovalIn: bounds.insetBy(dx: w * 0.13, dy: w * 0.13)).fill()
    }
}

final class CaptureViewController: UIViewController, AVCapturePhotoCaptureDelegate {
    var onFinish: ((Any?) -> Void)?
    var autoShutterMs = 0

    private let session = AVCaptureSession()
    private let photoOutput = AVCapturePhotoOutput()
    private let sessionQ = DispatchQueue(label: "dapple.capture.session")
    private var input: AVCaptureDeviceInput?
    private var device: AVCaptureDevice?
    private var backLenses: [AVCaptureDevice] = []
    private var frontLens: AVCaptureDevice?
    private var lens: LensCaps?
    private var proRawOn = false
    private var strategy: Strategy?
    private var wantFront = false

    private let preview = PreviewView()
    private let shutter = ShutterButton()
    private let badge = UILabel(), message = UILabel(), evLabel = UILabel()
    private let focusBox = UIView()
    private let lensRow = UIStackView()
    private let flip = UIButton(type: .system), close = UIButton(type: .system)

    private var busy = false, finished = false, running = false
    private var shotId = 0
    private var startedAt = Date()
    private var orientation: AVCaptureVideoOrientation = .portrait
    private var biasAtPanStart: Float = 0
    private var shot: Shot?
    private var timeoutWork: DispatchWorkItem?

    /// Files and state of one shutter press while its callbacks arrive.
    private final class Shot {
        let id: Int, mode: CaptureMode, stamp: String, attempt: Int, t0 = Date()
        var dng: Data?, jpg: Data?, thumb: Data?, error: Error?
        init(id: Int, mode: CaptureMode, stamp: String, attempt: Int) { self.id = id; self.mode = mode; self.stamp = stamp; self.attempt = attempt }
    }

    override var supportedInterfaceOrientations: UIInterfaceOrientationMask { .portrait }
    override var prefersStatusBarHidden: Bool { false }
    override var preferredStatusBarStyle: UIStatusBarStyle { .lightContent }
    override var prefersHomeIndicatorAutoHidden: Bool { true }

    // MARK: lifecycle

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black
        buildUI()
        backLenses = CaptureCaps.back()
        frontLens = CaptureCaps.front()
        UIDevice.current.beginGeneratingDeviceOrientationNotifications()
        let nc = NotificationCenter.default
        nc.addObserver(self, selector: #selector(deviceRotated), name: UIDevice.orientationDidChangeNotification, object: nil)
        nc.addObserver(self, selector: #selector(interrupted(_:)), name: .AVCaptureSessionWasInterrupted, object: session)
        nc.addObserver(self, selector: #selector(interruptionEnded), name: .AVCaptureSessionInterruptionEnded, object: session)
        nc.addObserver(self, selector: #selector(runtimeError(_:)), name: .AVCaptureSessionRuntimeError, object: session)
        nc.addObserver(self, selector: #selector(subjectAreaChanged), name: .AVCaptureDeviceSubjectAreaDidChange, object: nil)
        nc.addObserver(self, selector: #selector(appActive), name: UIApplication.didBecomeActiveNotification, object: nil)
        let wide = backLenses.first { $0.deviceType == .builtInWideAngleCamera } ?? backLenses.first ?? frontLens
        guard let first = wide else { return finish(error: "no camera") }
        preview.previewLayer.session = session
        preview.previewLayer.videoGravity = .resizeAspect
        select(first, initial: true)
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        let w = view.bounds.width, top = view.safeAreaInsets.top + 52
        preview.frame = CGRect(x: 0, y: top, width: w, height: w * 4 / 3)
        let bottom = view.bounds.height - view.safeAreaInsets.bottom
        shutter.frame = CGRect(x: (w - 76) / 2, y: bottom - 76 - 22, width: 76, height: 76)
        flip.frame = CGRect(x: w - 32 - 48, y: shutter.frame.midY - 24, width: 48, height: 48)
        let n = CGFloat(lensRow.arrangedSubviews.count)
        let lw = n * 44 + max(0, n - 1) * 14
        lensRow.frame = CGRect(x: (w - lw) / 2, y: shutter.frame.minY - 62, width: lw, height: 40)
        close.frame = CGRect(x: 8, y: view.safeAreaInsets.top + 2, width: 48, height: 48)
        badge.frame = CGRect(x: (w - 140) / 2, y: view.safeAreaInsets.top + 12, width: 140, height: 28)
        message.frame = CGRect(x: 20, y: preview.frame.maxY + 8, width: w - 40, height: 24)
        evLabel.frame = CGRect(x: (w - 110) / 2, y: preview.frame.minY + 10, width: 110, height: 24)
    }

    private func buildUI() {
        preview.isUserInteractionEnabled = true
        view.addSubview(preview)
        preview.addGestureRecognizer(UITapGestureRecognizer(target: self, action: #selector(tapFocus(_:))))
        preview.addGestureRecognizer(UIPanGestureRecognizer(target: self, action: #selector(panBias(_:))))
        focusBox.layer.borderColor = UIColor.systemYellow.cgColor
        focusBox.layer.borderWidth = 1.5
        focusBox.frame = CGRect(x: 0, y: 0, width: 76, height: 76)
        focusBox.isHidden = true
        focusBox.isUserInteractionEnabled = false
        preview.addSubview(focusBox)
        shutter.accessibilityLabel = "拍照"
        shutter.addTarget(self, action: #selector(shutterTapped), for: .touchUpInside)
        view.addSubview(shutter)
        flip.setImage(UIImage(systemName: "arrow.triangle.2.circlepath.camera"), for: .normal)
        flip.tintColor = .white
        flip.accessibilityLabel = "切换镜头"
        flip.addTarget(self, action: #selector(flipTapped), for: .touchUpInside)
        view.addSubview(flip)
        close.setImage(UIImage(systemName: "xmark"), for: .normal)
        close.tintColor = .white
        close.accessibilityLabel = "关闭"
        close.addTarget(self, action: #selector(closeTapped), for: .touchUpInside)
        view.addSubview(close)
        for l in [badge, message, evLabel] {
            l.textColor = .white
            l.textAlignment = .center
            l.clipsToBounds = true
            l.layer.cornerRadius = 12
            l.backgroundColor = UIColor.black.withAlphaComponent(0.4)
            view.addSubview(l)
        }
        badge.font = .boldSystemFont(ofSize: 12)
        message.font = .systemFont(ofSize: 13)
        message.backgroundColor = UIColor.black.withAlphaComponent(0.7)
        message.isHidden = true
        evLabel.font = .monospacedDigitSystemFont(ofSize: 12, weight: .medium)
        evLabel.isHidden = true
        lensRow.axis = .horizontal
        lensRow.alignment = .center
        lensRow.distribution = .equalSpacing
        view.addSubview(lensRow)
    }

    private func say(_ t: String?) {
        message.isHidden = t == nil
        if let t = t { message.text = t }
    }

    private func rebuildLensButtons() {
        lensRow.arrangedSubviews.forEach { $0.removeFromSuperview() }
        let wide = backLenses.first { $0.deviceType == .builtInWideAngleCamera }
        let all = backLenses.count > 1 ? backLenses : []
        for (i, d) in all.enumerated() {
            let b = UIButton(type: .system)
            b.setTitle(CaptureCaps.label(d, wide: wide), for: .normal)
            b.titleLabel?.font = .boldSystemFont(ofSize: 13)
            let on = d === device
            b.setTitleColor(on ? .systemYellow : .white, for: .normal)
            b.backgroundColor = UIColor.black.withAlphaComponent(0.45)
            b.layer.cornerRadius = 18
            b.tag = i
            b.widthAnchor.constraint(equalToConstant: 44).isActive = true
            b.heightAnchor.constraint(equalToConstant: 36).isActive = true
            b.addTarget(self, action: #selector(lensTapped(_:)), for: .touchUpInside)
            lensRow.addArrangedSubview(b)
        }
        lensRow.layoutMargins = .zero
        lensRow.spacing = 14
        view.setNeedsLayout()
        lensRow.distribution = .fill
    }

    // MARK: session

    private func select(_ d: AVCaptureDevice, initial: Bool = false) {
        device = d
        startedAt = Date()
        evLabel.isHidden = true
        focusBox.isHidden = true
        shutter.isEnabled = false
        let wide = backLenses.first { $0.deviceType == .builtInWideAngleCamera }
        sessionQ.async { [weak self] in
            guard let self = self else { return }
            self.session.beginConfiguration()
            self.session.sessionPreset = .photo
            if let i = self.input { self.session.removeInput(i) }
            guard let inp = try? AVCaptureDeviceInput(device: d), self.session.canAddInput(inp) else {
                self.session.commitConfiguration()
                DispatchQueue.main.async { self.finish(error: "cannot open the camera") }
                return
            }
            self.session.addInput(inp)
            self.input = inp
            if !self.session.outputs.contains(self.photoOutput), self.session.canAddOutput(self.photoOutput) { self.session.addOutput(self.photoOutput) }
            self.session.commitConfiguration()
            // formats are only known with the output connected to this lens: ProRAW first, Bayer otherwise
            var caps = LensCaps(id: d.uniqueID, label: CaptureCaps.label(d, wide: wide), position: d.position == .front ? "front" : "back", type: CaptureCaps.typeName(d))
            caps.bayerFormat = self.photoOutput.availableRawPhotoPixelFormatTypes.first { AVCapturePhotoOutput.isBayerRAWPixelFormat($0) }
            caps.bayer = self.photoOutput.availableRawPhotoPixelFormatTypes.filter { AVCapturePhotoOutput.isBayerRAWPixelFormat($0) }.map { CaptureCaps.fourCC($0) }
            self.photoOutput.isAppleProRAWEnabled = self.photoOutput.isAppleProRAWSupported  // after the session is configured
            self.proRawOn = self.photoOutput.isAppleProRAWEnabled
            caps.proRaw = self.proRawOn
            caps.proRawFormat = self.photoOutput.availableRawPhotoPixelFormatTypes.first { AVCapturePhotoOutput.isAppleProRAWPixelFormat($0) }
            if let f = caps.rawFormat { caps.rawThumbJpeg = AVCapturePhotoSettings(rawPixelFormatType: f).availableRawEmbeddedThumbnailPhotoCodecTypes.contains(.jpeg) }
            if let m = d.activeFormat.supportedMaxPhotoDimensions.max(by: { Int($0.width) * Int($0.height) < Int($1.width) * Int($1.height) }) {
                self.photoOutput.maxPhotoDimensions = m
                caps.maxW = Int(m.width); caps.maxH = Int(m.height)
            }
            self.photoOutput.maxPhotoQualityPrioritization = .quality
            self.applyOrientation(to: self.photoOutput.connection(with: .video), orientation: .portrait)
            if !self.session.isRunning { self.session.startRunning() }
            let st = Strategy(key: "\(d.uniqueID)|\(CaptureCaps.osBuild)", modes: caps.modes)
            DispatchQueue.main.async {
                self.lens = caps
                self.strategy = st
                self.setBadge()
                self.rebuildLensButtons()
                self.flip.isHidden = self.frontLens == nil || (self.backLenses.isEmpty)
                self.applyPreviewOrientation()
                clog("lens \(caps.label) \(caps.type) \(d.position == .front ? "front" : "back") raw=\(caps.rawKind) bayer=\(caps.bayer) proraw=\(caps.proRaw) maxPx=\(caps.maxW)x\(caps.maxH) mode=\(st.mode.rawValue)")
                self.waitSettled(minDelay: 0.7) { self.shutter.isEnabled = !self.busy }
                if initial, self.autoShutterMs > 0 {
                    DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(self.autoShutterMs)) { self.shutterTapped() }
                }
            }
        }
    }

    private func applyPreviewOrientation() {
        guard let c = preview.previewLayer.connection else { return }
        applyOrientation(to: c, orientation: .portrait)
    }

    private func applyOrientation(to c: AVCaptureConnection?, orientation o: AVCaptureVideoOrientation) {
        guard let c = c else { return }
        if #available(iOS 17.0, *) {
            let angle: CGFloat
            switch o {
            case .portrait: angle = 90
            case .landscapeRight: angle = 0
            case .landscapeLeft: angle = 180
            case .portraitUpsideDown: angle = 270
            @unknown default: angle = 90
            }
            if c.isVideoRotationAngleSupported(angle) { c.videoRotationAngle = angle }
        } else if c.isVideoOrientationSupported {
            c.videoOrientation = o
        }
    }

    @objc private func deviceRotated() {
        switch UIDevice.current.orientation {
        case .portrait: orientation = .portrait
        case .portraitUpsideDown: orientation = .portraitUpsideDown
        case .landscapeLeft: orientation = .landscapeRight  // the camera's orientation is mirrored against the device's
        case .landscapeRight: orientation = .landscapeLeft
        default: break  // face up / down / unknown: keep the last one
        }
    }

    // MARK: settle (the iOS counterpart of Android's 700 ms settle: wait until exposure / focus / white balance stop adjusting)

    private func waitSettled(minDelay: TimeInterval, timeout: TimeInterval = 2.5, done: @escaping () -> Void) {
        let begin = Date()
        func poll() {
            guard !finished else { return }
            let d = device
            let adjusting = (d?.isAdjustingExposure ?? false) || (d?.isAdjustingFocus ?? false) || (d?.isAdjustingWhiteBalance ?? false)
            let sinceStart = Date().timeIntervalSince(startedAt), waited = Date().timeIntervalSince(begin)
            if (!adjusting && sinceStart >= minDelay) || waited >= timeout {
                if adjusting { clog("settle: timeout after \(Int(waited * 1000)) ms, still adjusting") } else { clog("settled after \(Int(sinceStart * 1000)) ms since start") }
                return done()
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { poll() }
        }
        poll()
    }

    // MARK: controls

    @objc private func tapFocus(_ g: UITapGestureRecognizer) {
        guard let d = device else { return }
        let p = g.location(in: preview)
        let pt = preview.previewLayer.captureDevicePointConverted(fromLayerPoint: p)
        do {
            try d.lockForConfiguration()
            if d.isFocusPointOfInterestSupported, d.isFocusModeSupported(.continuousAutoFocus) { d.focusPointOfInterest = pt; d.focusMode = .continuousAutoFocus }
            if d.isExposurePointOfInterestSupported, d.isExposureModeSupported(.continuousAutoExposure) { d.exposurePointOfInterest = pt; d.exposureMode = .continuousAutoExposure }
            d.isSubjectAreaChangeMonitoringEnabled = true
            d.unlockForConfiguration()
        } catch { return }
        focusBox.center = p
        focusBox.isHidden = false
        focusBox.alpha = 1
        focusBox.transform = CGAffineTransform(scaleX: 1.4, y: 1.4)
        UIView.animate(withDuration: 0.2) { self.focusBox.transform = .identity }
        UIView.animate(withDuration: 0.4, delay: 1.5, options: []) { self.focusBox.alpha = 0.3 }
        shutter.isEnabled = false
        waitSettled(minDelay: 0, timeout: 1.5) { self.shutter.isEnabled = !self.busy }
    }

    @objc private func panBias(_ g: UIPanGestureRecognizer) {
        guard let d = device else { return }
        if g.state == .began { biasAtPanStart = d.exposureTargetBias }
        let lo = max(d.minExposureTargetBias, -2), hi = min(d.maxExposureTargetBias, 2)
        let v = biasAtPanStart - Float(g.translation(in: preview).y / preview.bounds.height) * 4
        let bias = min(max(v, lo), hi)
        if (try? d.lockForConfiguration()) != nil { d.setExposureTargetBias(bias, completionHandler: nil); d.unlockForConfiguration() }
        evLabel.text = String(format: "EV %+.1f", bias)
        evLabel.isHidden = false
        if g.state == .ended || g.state == .cancelled {
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { if g.state != .changed { self.evLabel.isHidden = abs(bias) < 0.05 } }
        }
    }

    @objc private func subjectAreaChanged() {
        guard let d = device, (try? d.lockForConfiguration()) != nil else { return }
        let c = CGPoint(x: 0.5, y: 0.5)
        if d.isFocusPointOfInterestSupported { d.focusPointOfInterest = c; if d.isFocusModeSupported(.continuousAutoFocus) { d.focusMode = .continuousAutoFocus } }
        if d.isExposurePointOfInterestSupported { d.exposurePointOfInterest = c; if d.isExposureModeSupported(.continuousAutoExposure) { d.exposureMode = .continuousAutoExposure } }
        d.isSubjectAreaChangeMonitoringEnabled = false
        d.unlockForConfiguration()
        focusBox.isHidden = true
    }

    @objc private func lensTapped(_ b: UIButton) {
        guard !busy, b.tag < backLenses.count, backLenses[b.tag] !== device else { return }
        wantFront = false
        select(backLenses[b.tag])
    }

    @objc private func flipTapped() {
        guard !busy else { return }
        if let d = device, d.position == .front {
            guard let w = backLenses.first(where: { $0.deviceType == .builtInWideAngleCamera }) ?? backLenses.first else { return }
            select(w)
        } else if let f = frontLens { select(f) }
    }

    @objc private func closeTapped() { finish(cancel: true) }

    // MARK: interruptions

    @objc private func interrupted(_ n: Notification) {
        let reason = (n.userInfo?[AVCaptureSessionInterruptionReasonKey] as? Int).flatMap { AVCaptureSession.InterruptionReason(rawValue: $0) }
        clog("session interrupted: \(String(describing: reason?.rawValue))")
        switch reason {
        case .videoDeviceNotAvailableInBackground: break
        case .videoDeviceInUseByAnotherClient, .audioDeviceInUseByAnotherClient: say("相机正被其他 App 使用")
        case .videoDeviceNotAvailableWithMultipleForegroundApps: say("多任务下无法使用相机")
        case .videoDeviceNotAvailableDueToSystemPressure: say("设备过热，相机暂不可用")
        default: say("相机被中断")
        }
        shutter.isEnabled = false
    }

    @objc private func interruptionEnded() {
        clog("session interruption ended")
        say(nil)
        startedAt = Date()
        waitSettled(minDelay: 0.7) { self.shutter.isEnabled = !self.busy }
    }

    @objc private func runtimeError(_ n: Notification) {
        let e = n.userInfo?[AVCaptureSessionErrorKey] as? AVError
        clog("runtime error \(String(describing: e?.code.rawValue))")
        if e?.code == .mediaServicesWereReset || e?.code == .sessionWasInterrupted {
            sessionQ.async { self.session.startRunning() }
        } else { say("相机出错，请重试") }
    }

    @objc private func appActive() {
        guard !finished else { return }
        sessionQ.async { if !self.session.isRunning { self.session.startRunning() } }
        startedAt = Date()
    }

    override func viewDidDisappear(_ animated: Bool) {
        super.viewDidDisappear(animated)
        if isBeingDismissed { teardown() }
    }

    private func teardown() {
        UIDevice.current.endGeneratingDeviceOrientationNotifications()
        NotificationCenter.default.removeObserver(self)
        timeoutWork?.cancel()
        let s = session
        sessionQ.async { if s.isRunning { s.stopRunning() } }
    }

    // MARK: shot

    private func badgeText(_ m: CaptureMode) -> String {
        let r = proRawOn && lens?.proRawFormat != nil ? "ProRAW" : "RAW"
        switch m {
        case .rawJpeg: return "\(r)+JPEG"
        case .raw: return r
        case .jpeg: return "JPEG"
        }
    }

    private func setBadge() { badge.text = badgeText(strategy?.mode ?? .jpeg) }

    @objc private func shutterTapped() { takePicture(attempt: 0) }

    private func settings(for mode: CaptureMode) -> AVCapturePhotoSettings? {
        let out = photoOutput
        var s: AVCapturePhotoSettings
        let jpegOK = out.availablePhotoCodecTypes.contains(.jpeg)
        switch mode {
        case .rawJpeg, .raw:
            guard let f = lens?.rawFormat, out.availableRawPhotoPixelFormatTypes.contains(f) else { return nil }
            if mode == .rawJpeg {
                guard jpegOK else { return nil }
                s = AVCapturePhotoSettings(rawPixelFormatType: f, processedFormat: [AVVideoCodecKey: AVVideoCodecType.jpeg])
            } else {
                s = AVCapturePhotoSettings(rawPixelFormatType: f)
                // RAW only: the camera's own rendering rides along (embedded in the DNG when the format allows, plus a JPEG for Dart to embed otherwise)
                if let pf = s.availablePreviewPhotoPixelFormatTypes.first {
                    s.previewPhotoFormat = [kCVPixelBufferPixelFormatTypeKey as String: pf, kCVPixelBufferWidthKey as String: 1280, kCVPixelBufferHeightKey as String: 960]
                }
            }
            if s.availableRawEmbeddedThumbnailPhotoCodecTypes.contains(.jpeg) { s.rawEmbeddedThumbnailPhotoFormat = [AVVideoCodecKey: AVVideoCodecType.jpeg] }
        case .jpeg:
            guard jpegOK else { return nil }
            s = AVCapturePhotoSettings(format: [AVVideoCodecKey: AVVideoCodecType.jpeg])
            if out.maxPhotoQualityPrioritization.rawValue >= AVCapturePhotoOutput.QualityPrioritization.quality.rawValue { s.photoQualityPrioritization = .quality }
        }
        if out.supportedFlashModes.contains(.off) { s.flashMode = .off }  // flash stays off, always
        s.maxPhotoDimensions = out.maxPhotoDimensions
        return s
    }

    private func takePicture(attempt: Int) {
        guard !busy, !finished, let st = strategy else { return }
        let mode = st.mode
        guard let s = settings(for: mode) else {
            clog("\(mode.rawValue) not offered by the output, stepping down")
            if st.failed() != nil { return takePicture(attempt: attempt) }
            return say("拍摄失败，请重试")
        }
        busy = true
        shutter.isEnabled = false
        say(nil)
        shotId += 1
        let f = DateFormatter(); f.locale = Locale(identifier: "en_US_POSIX"); f.dateFormat = "yyyyMMdd_HHmmss"
        let shot = Shot(id: shotId, mode: mode, stamp: f.string(from: Date()), attempt: attempt)
        self.shot = shot
        applyOrientation(to: photoOutput.connection(with: .video), orientation: orientation)
        clog("shot \(shot.id) \(mode.rawValue) \(badgeText(mode)) orientation=\(orientation.rawValue) settle=\(Int(Date().timeIntervalSince(startedAt) * 1000)) ms exposure=\(String(format: "%.4f", device?.exposureDuration.seconds ?? 0))s iso=\(device?.iso ?? 0)")
        let id = shot.id
        let work = DispatchWorkItem { [weak self] in
            guard let self = self, self.shot?.id == id, !self.finished else { return }
            clog("shot \(id): timeout after \(Int(Date().timeIntervalSince(shot.t0) * 1000)) ms (\(mode.rawValue))")
            self.shotFailed(shot)
        }
        timeoutWork?.cancel()
        timeoutWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 20, execute: work)
        photoOutput.capturePhoto(with: s, delegate: self)
    }

    // MARK: AVCapturePhotoCaptureDelegate (callbacks on an internal queue)

    func photoOutput(_ output: AVCapturePhotoOutput, willBeginCaptureFor resolvedSettings: AVCaptureResolvedPhotoSettings) {
        clog("exposure started")
    }

    func photoOutput(_ output: AVCapturePhotoOutput, didFinishProcessingPhoto photo: AVCapturePhoto, error: Error?) {
        DispatchQueue.main.async {
            guard let shot = self.shot, !self.finished else { return }
            if let e = error { shot.error = e; clog("shot \(shot.id): photo error \(e.localizedDescription)"); return }
            let data = self.fileData(photo)
            let w = photo.resolvedSettings.photoDimensions
            clog("shot \(shot.id): \(photo.isRawPhoto ? "raw" : "processed") photo \(data?.count ?? 0) bytes, \(w.width)x\(w.height), +\(Int(Date().timeIntervalSince(shot.t0) * 1000)) ms")
            if photo.isRawPhoto { shot.dng = data } else { shot.jpg = data }
            if photo.isRawPhoto, shot.mode == .raw, let cg = photo.previewCGImageRepresentation() {
                shot.thumb = Self.jpeg(cg)
            }
        }
    }

    func photoOutput(_ output: AVCapturePhotoOutput, didFinishCaptureFor resolvedSettings: AVCaptureResolvedPhotoSettings, error: Error?) {
        DispatchQueue.main.async {
            guard let shot = self.shot, !self.finished else { return }
            if let e = error { shot.error = e; clog("shot \(shot.id): capture error \(e.localizedDescription)") }
            self.timeoutWork?.cancel()
            self.complete(shot)
        }
    }

    /// File bytes of one photo; capture date and (only with an existing location permission) GPS go in with the metadata.
    private func fileData(_ photo: AVCapturePhoto) -> Data? {
        if let gps = Self.gps() {
            let c = GPSCustomizer(photo: photo, gps: gps)
            if let d = photo.fileDataRepresentation(with: c), !d.isEmpty { return d }
        }
        return photo.fileDataRepresentation()
    }

    private final class GPSCustomizer: NSObject, AVCapturePhotoFileDataRepresentationCustomizer {
        let md: [String: Any]
        init(photo: AVCapturePhoto, gps: [String: Any]) { var m = photo.metadata; m[kCGImagePropertyGPSDictionary as String] = gps; md = m }
        func replacementMetadata(for photo: AVCapturePhoto) -> [String: Any]? { md }
    }

    /// Never asks: only a location that is already allowed and already known (a fix not older than 10 minutes).
    private static func gps() -> [String: Any]? {
        let m = CLLocationManager()
        guard m.authorizationStatus == .authorizedWhenInUse || m.authorizationStatus == .authorizedAlways,
              let l = m.location, -l.timestamp.timeIntervalSinceNow < 600 else { return nil }
        let f = DateFormatter(); f.timeZone = TimeZone(identifier: "UTC"); f.dateFormat = "HH:mm:ss.SS"
        let d = DateFormatter(); d.timeZone = TimeZone(identifier: "UTC"); d.dateFormat = "yyyy:MM:dd"
        return [kCGImagePropertyGPSLatitude as String: abs(l.coordinate.latitude), kCGImagePropertyGPSLatitudeRef as String: l.coordinate.latitude >= 0 ? "N" : "S",
                kCGImagePropertyGPSLongitude as String: abs(l.coordinate.longitude), kCGImagePropertyGPSLongitudeRef as String: l.coordinate.longitude >= 0 ? "E" : "W",
                kCGImagePropertyGPSAltitude as String: abs(l.altitude), kCGImagePropertyGPSAltitudeRef as String: l.altitude < 0 ? 1 : 0,
                kCGImagePropertyGPSTimeStamp as String: f.string(from: l.timestamp), kCGImagePropertyGPSDateStamp as String: d.string(from: l.timestamp)]
    }

    private static func jpeg(_ cg: CGImage) -> Data? {
        let d = NSMutableData()
        guard let dst = CGImageDestinationCreateWithData(d, UTType.jpeg.identifier as CFString, 1, nil) else { return nil }
        CGImageDestinationAddImage(dst, cg, [kCGImageDestinationLossyCompressionQuality: 0.9] as CFDictionary)
        return CGImageDestinationFinalize(dst) ? d as Data : nil
    }

    /// Does the DNG carry its own preview (what the engine's exposure anchor reads, tier 1)? ImageIO finds the embedded thumbnail.
    private static func previewInfo(_ d: Data) -> String {
        guard let src = CGImageSourceCreateWithData(d as CFData, nil) else { return "unreadable" }
        let o: [CFString: Any] = [kCGImageSourceCreateThumbnailFromImageIfAbsent: false, kCGImageSourceCreateThumbnailFromImageAlways: false, kCGImageSourceThumbnailMaxPixelSize: 4000]
        if let t = CGImageSourceCreateThumbnailAtIndex(src, 0, o as CFDictionary) { return "embedded preview \(t.width)x\(t.height), images \(CGImageSourceGetCount(src))" }
        return "no embedded preview, images \(CGImageSourceGetCount(src))"
    }

    private static func isJpeg(_ d: Data?) -> Bool { (d?.count ?? 0) > 1000 && d![0] == 0xFF && d![1] == 0xD8 }
    private static func isDng(_ d: Data?) -> Bool {
        guard let d = d, d.count > 1000 else { return false }
        return (d[0] == 0x49 && d[1] == 0x49 && d[2] == 0x2A && d[3] == 0) || (d[0] == 0x4D && d[1] == 0x4D && d[2] == 0 && d[3] == 0x2A)
    }

    private func complete(_ shot: Shot) {
        let st = strategy
        let okJ = Self.isJpeg(shot.jpg), okD = Self.isDng(shot.dng)
        let good: Bool
        switch shot.mode {
        case .jpeg: good = okJ
        case .raw: good = okD
        case .rawJpeg: good = okD && okJ
        }
        guard good, shot.error == nil || shot.mode != .rawJpeg || (okD && okJ) else { return shotFailed(shot) }
        st?.markVerified()
        let dir = NSSearchPathForDirectoriesInDomains(.cachesDirectory, .userDomainMask, true)[0] + "/capture"
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        var r: [String: Any] = ["source": "avfoundation", "mode": shot.mode.rawValue, "lens": lens?.label ?? "", "raw": lens?.rawKind ?? "none"]
        do {
            if okJ, let d = shot.jpg { let p = "\(dir)/DAP_\(shot.stamp).jpg"; try d.write(to: URL(fileURLWithPath: p)); r["jpg"] = p }
            if okD, let d = shot.dng { let p = "\(dir)/DAP_\(shot.stamp).dng"; try d.write(to: URL(fileURLWithPath: p)); r["dng"] = p }
            if shot.mode == .raw, let t = shot.thumb { let p = "\(dir)/DAP_\(shot.stamp).thumb.jpg"; try t.write(to: URL(fileURLWithPath: p)); r["thumb"] = p }
        } catch {
            clog("shot \(shot.id): cannot write the files: \(error.localizedDescription)")
            return shotFailed(shot)
        }
        if let d = shot.dng { clog("dng \(Self.previewInfo(d))") }
        clog("shot \(shot.id) delivered: \(shot.mode.rawValue) dng=\(shot.dng?.count ?? 0) jpg=\(shot.jpg?.count ?? 0) thumb=\(shot.thumb?.count ?? 0), \(Int(Date().timeIntervalSince(shot.t0) * 1000)) ms")
        finish(result: r)
    }

    /// The shot did not produce what it promised: the mode steps down (RAW+JPEG -> RAW -> JPEG, remembered) and the picture is taken again, at most twice.
    private func shotFailed(_ shot: Shot) {
        timeoutWork?.cancel()
        self.shot = nil  // late callbacks of this attempt are ignored
        busy = false
        shutter.isEnabled = true
        guard let st = strategy else { return }
        if st.mode == shot.mode, let next = st.failed() {
            clog("\(shot.mode.rawValue) failed on this lens, now \(next.rawValue)")
            setBadge()
            if shot.attempt < 2 {
                say(next == .raw ? "RAW+JPEG 不可用，改为仅 RAW，正在重拍" : "RAW 不可用，已改为 JPEG，正在重拍")
                return takePicture(attempt: shot.attempt + 1)
            }
        }
        say("拍摄失败，请重试")
    }

    // MARK: finishing

    private func finish(result: [String: Any]) {
        guard !finished else { return }
        finished = true
        dismiss(animated: true) { self.onFinish?(result) }
    }

    private func finish(cancel: Bool) {
        guard !finished else { return }
        finished = true
        dismiss(animated: true) { self.onFinish?(nil) }
    }

    private func finish(error: String) {
        guard !finished else { return }
        finished = true
        let cb = onFinish
        if presentingViewController != nil { dismiss(animated: true) { cb?(FlutterError(code: "capture", message: error, details: nil)) } }
        else { cb?(FlutterError(code: "capture", message: error, details: nil)) }
    }
}

/// RAW+JPEG -> RAW -> JPEG per lens, remembered per lens id + OS build (a wrong verdict heals itself: a mode that fails steps down for good, until the OS build changes).
final class Strategy {
    private let key: String
    private let modes: [CaptureMode]
    private(set) var mode: CaptureMode
    private var verified = false

    init(key: String, modes: [CaptureMode]) {
        self.key = "dapple.capture.v1|" + key
        self.modes = modes
        if let f = DappleCapturePlugin.forcedMode, modes.contains(f) { mode = f }
        else if let s = UserDefaults.standard.string(forKey: self.key), let m = CaptureMode(rawValue: s), modes.contains(m) { mode = m } else { mode = modes[0] }
    }

    func markVerified() {
        verified = true
        if DappleCapturePlugin.forcedMode == nil { UserDefaults.standard.set(mode.rawValue, forKey: key) }
    }

    @discardableResult func failed() -> CaptureMode? {
        var m = mode.lower
        while let x = m, !modes.contains(x) { m = x.lower }
        guard let n = m else { return nil }
        mode = n
        if DappleCapturePlugin.forcedMode == nil { UserDefaults.standard.set(n.rawValue, forKey: key) }
        return n
    }
}
