import CoreVideo
import Flutter
import Metal
import UIKit

// Zero-copy display for the editor on iOS (docs/engine/present.md, docs/ios/metal-backend.md): channel "dapple/view", same methods as the Kotlin
// DappleViewPlugin (available / create / bind / resize / unbind / dispose). One entry = one Flutter texture (FlutterTexture) backed by a ring of
// IOSurface CVPixelBuffers; the engine's Metal presenter (core/engine/present/metal_presenter.mm) writes frames into them through the C struct below.
//   create {w,h}   -> {ok, id}     texture id for Texture(textureId:)
//   bind {id,view} -> {ok, error}  dapple_view_attach_window(view, sink, w, h) with the entry's frame sink (view = the FFI address of the native view)
//   resize {id,w,h}-> {ok}         the surfaces follow this size (the presenter scales the frame into them)
//   unbind / dispose

@_silgen_name("dapple_view_attach_window") private func dapple_view_attach_window_c(_ view: UnsafeMutableRawPointer?, _ window: UnsafeMutableRawPointer?, _ w: Int32, _ h: Int32) -> Int32
@_silgen_name("dapple_view_detach_window") private func dapple_view_detach_window_c(_ view: UnsafeMutableRawPointer?)
@_silgen_name("dapple_view_trim") private func dapple_view_trim_c(_ view: UnsafeMutableRawPointer?, _ level: Int32)

/// C layout shared with metal_presenter.mm (DappleFrameSink).
struct FrameSinkTable {
    var user: UnsafeMutableRawPointer?
    var acquire: (@convention(c) (UnsafeMutableRawPointer?, UnsafeMutableRawPointer?) -> UnsafeMutableRawPointer?)?
    var ready: (@convention(c) (UnsafeMutableRawPointer?) -> Void)?
}

final class DappleTexture: NSObject, FlutterTexture {
    private let lock = NSLock()
    private var ring: [(buf: CVPixelBuffer, tex: MTLTexture?)] = []
    private var state: [Int] = []  // 0 free, 1 being written by the GPU, 2 latest shown
    private var latest = -1
    // Surfaces handed to the GPU that have not completed yet, oldest first. The completion handlers run in commit order, so `ready` always belongs to the
    // front one: with a single `writing` slot a handler that ran after the next `acquire` published the surface that was still being drawn into (a torn frame)
    // and leaked the finished one.
    private var pending: [Int] = []
    private var w: Int, h: Int
    private var allocW = 0, allocH = 0
    var textureId: Int64 = -1
    private var frames = 0
    weak var registry: FlutterTextureRegistry?
    var table = UnsafeMutablePointer<FrameSinkTable>.allocate(capacity: 1)
    var view: UnsafeMutableRawPointer?

    init(w: Int, h: Int) {
        self.w = max(w, 16)
        self.h = max(h, 16)
        super.init()
        table.initialize(to: FrameSinkTable(
            user: Unmanaged.passUnretained(self).toOpaque(),
            acquire: { user, dev in
                guard let user = user, let dev = dev else { return nil }
                return Unmanaged<DappleTexture>.fromOpaque(user).takeUnretainedValue().acquire(device: Unmanaged<AnyObject>.fromOpaque(dev).takeUnretainedValue() as! MTLDevice)
            },
            ready: { user in
                guard let user = user else { return }
                Unmanaged<DappleTexture>.fromOpaque(user).takeUnretainedValue().ready()
            }))
    }

    deinit { table.deallocate() }

    func resize(w: Int, h: Int) {
        lock.lock()
        self.w = max(w, 16)
        self.h = max(h, 16)
        lock.unlock()
    }
    var size: (Int, Int) { lock.lock(); defer { lock.unlock() }; return (w, h) }

    private func rebuild(device: MTLDevice) {
        ring.removeAll()
        state.removeAll()
        latest = -1
        pending.removeAll()
        for _ in 0..<3 {
            var pb: CVPixelBuffer?
            let attrs: [CFString: Any] = [
                kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA,
                kCVPixelBufferWidthKey: w, kCVPixelBufferHeightKey: h,
                kCVPixelBufferIOSurfacePropertiesKey: [:] as [String: Any],
                kCVPixelBufferMetalCompatibilityKey: true,
            ]
            guard CVPixelBufferCreate(kCFAllocatorDefault, w, h, kCVPixelFormatType_32BGRA, attrs as CFDictionary, &pb) == kCVReturnSuccess, let buf = pb,
                  let surf = CVPixelBufferGetIOSurface(buf)?.takeUnretainedValue() else { continue }
            let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: w, height: h, mipmapped: false)
            td.usage = [.shaderRead, .shaderWrite]
            td.storageMode = .shared
            ring.append((buf, device.makeTexture(descriptor: td, iosurface: surf, plane: 0)))
            state.append(0)
        }
        allocW = w
        allocH = h
    }

    /// Called on the engine's render thread: the next free surface (unretained), or nil.
    func acquire(device: MTLDevice) -> UnsafeMutableRawPointer? {
        lock.lock()
        defer { lock.unlock() }
        if ring.isEmpty || allocW != w || allocH != h { rebuild(device: device) }
        guard let i = state.firstIndex(of: 0), let t = ring[i].tex else { return nil }
        state[i] = 1
        pending.append(i)
        return Unmanaged.passUnretained(t as AnyObject).toOpaque()
    }

    /// The GPU finished the surface acquired last: it becomes the one Flutter shows.
    func ready() {
        lock.lock()
        if !pending.isEmpty {
            let done = pending.removeFirst()
            if latest >= 0, latest < state.count { state[latest] = 0 }
            state[done] = 2
            latest = done
        }
        frames += 1
        let n = frames
        let (sw, sh) = (w, h)
        lock.unlock()
        if n == 1 || n % 60 == 0 { NSLog("dapple view: %d frames shown through the Metal presenter (surface %dx%d)", n, sw, sh) }
        DispatchQueue.main.async { [weak self] in
            guard let s = self else { return }
            s.registry?.textureFrameAvailable(s.textureId)
        }
    }

    func copyPixelBuffer() -> Unmanaged<CVPixelBuffer>? {
        lock.lock()
        defer { lock.unlock() }
        guard latest >= 0, latest < ring.count else { return nil }
        return Unmanaged.passRetained(ring[latest].buf)
    }
}

public final class DappleViewPlugin: NSObject {
    private var entries: [Int64: DappleTexture] = [:]
    private let registry: FlutterTextureRegistry

    init(registrar: FlutterPluginRegistrar) {
        registry = registrar.textures()
        super.init()
        let ch = FlutterMethodChannel(name: "dapple/view", binaryMessenger: registrar.messenger())
        ch.setMethodCallHandler(handle)
        NotificationCenter.default.addObserver(forName: UIApplication.didReceiveMemoryWarningNotification, object: nil, queue: .main) { [weak self] _ in
            for e in self?.entries.values ?? [:].values { if let v = e.view { dapple_view_trim_c(v, 1) } }
        }
    }

    private func handle(_ call: FlutterMethodCall, _ result: @escaping FlutterResult) {
        let a = call.arguments as? [String: Any] ?? [:]
        switch call.method {
        case "available":
            NSLog("dapple view: available")
            result(true)
        case "create":
            let t = DappleTexture(w: (a["w"] as? Int) ?? 1080, h: (a["h"] as? Int) ?? 1080)
            t.registry = registry
            t.textureId = registry.register(t)
            entries[t.textureId] = t
            result(["ok": true, "id": t.textureId])
        case "bind":
            guard let id = (a["id"] as? NSNumber)?.int64Value, let t = entries[id], let v = (a["view"] as? NSNumber)?.intValue else { return result(["ok": false, "error": "bad arguments"]) }
            t.view = UnsafeMutableRawPointer(bitPattern: v)
            let (w, h) = t.size
            let rc = dapple_view_attach_window_c(t.view, UnsafeMutableRawPointer(t.table), Int32(w), Int32(h))
            NSLog("dapple view: bind %lld -> attach rc %d (surface %dx%d)", id, rc, w, h)
            result(rc == 0 ? ["ok": true] : ["ok": false, "error": "attach failed (\(rc))"])
        case "resize":
            guard let id = (a["id"] as? NSNumber)?.int64Value, let t = entries[id] else { return result(["ok": false, "error": "unknown texture"]) }
            t.resize(w: (a["w"] as? Int) ?? 0, h: (a["h"] as? Int) ?? 0)
            result(["ok": true])
        case "unbind":
            if let id = (a["id"] as? NSNumber)?.int64Value, let t = entries[id], let v = t.view { dapple_view_detach_window_c(v); t.view = nil }
            result(nil)
        case "dispose":
            if let id = (a["id"] as? NSNumber)?.int64Value, let t = entries.removeValue(forKey: id) {
                if let v = t.view { dapple_view_detach_window_c(v) }
                registry.unregisterTexture(id)
            }
            result(nil)
        default:
            result(FlutterMethodNotImplemented)
        }
    }
}
