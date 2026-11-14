// Wired camera import on iOS: a camera on the phone's cable, read through ImageCaptureCore (ICDeviceBrowser / ICCameraDevice, iOS 13+,
// platform API, no third-party code). Same channels, method names and event maps as the Kotlin UsbCameraPlugin, read by
// app/lib/platform/usb_camera.dart (`ChannelUsbCamera`) and app/lib/services/usb_ingest.dart:
//   dapple/usbcam (MethodChannel)   start {inbox} / stop / status / requestPermission / list {cursor,limit,refresh} / thumbnail {handle} /
//                                   import {handles} / cancelImport / diagnostics
//   dapple/usbcam_events            state {state: none|permission|opening|ready|unplugged|error, reason?, device?, card?}, newShots,
//                                   importQueue, importStart, importProgress, importDone, importFailed, importQueueDone
// Handles are session-local integers handed out here (ImageCaptureCore has no portable integer id); the stable identity is the "key"
// (`usb:<lower-case name>|<size>|<capture second>`, same as CamIdentity.key on Android).
//
// NOT TESTED WITH A REAL CAMERA. It compiles for the device and the simulator; no iPhone with a camera on the cable was available.
// Things only a real camera can show: whether the camera lists its RAW files in `mediaFiles`, how it reports dates, the speed and the
// progress callbacks of `requestDownloadWithOptions`, what happens on a mid-transfer unplug, and whether the system asks for
// permission at `requestContentsAuthorization` (Info.plist strings: NSPhotoLibraryUsageDescription, NSCameraUsageDescription).
//
// All plugin state lives on the main queue (ImageCaptureCore calls the delegates on the main queue; completion blocks of requests may run on
// any queue and hop back before they touch anything).
import Flutter
import ImageCaptureCore
import UIKit

final class DappleUsbCamPlugin: NSObject, FlutterStreamHandler, ICDeviceBrowserDelegate, ICCameraDeviceDelegate {
    private var sink: FlutterEventSink?
    private let browser = ICDeviceBrowser()
    private var inbox: URL?
    private var started = false
    private var camera: ICCameraDevice?
    private var catalogReady = false
    private var lastState: [String: Any] = ["type": "state", "state": "none"]
    private var log: [String] = []

    // session-local handles
    private var nextHandle = 1
    private var fileOfHandle: [Int: ICCameraFile] = [:]
    private var handleOfFile: [ObjectIdentifier: Int] = [:]
    private var order: [ICCameraFile] = []  // newest first, built by list(refresh:)
    private var skippedVideos = 0

    // import queue
    private var importing = false
    private var importCancelled = false
    private var importProgress: Progress?
    private var progressTimer: Timer?

    init(registrar: FlutterPluginRegistrar) {
        super.init()
        browser.delegate = self
        browser.browsedDeviceTypeMask = ICDeviceTypeMask(rawValue: ICDeviceTypeMask.camera.rawValue | ICDeviceLocationTypeMask.local.rawValue)!
        FlutterMethodChannel(name: "dapple/usbcam", binaryMessenger: registrar.messenger()).setMethodCallHandler { [weak self] call, result in
            self?.handle(call, result)
        }
        FlutterEventChannel(name: "dapple/usbcam_events", binaryMessenger: registrar.messenger()).setStreamHandler(self)
    }

    // MARK: stream

    func onListen(withArguments arguments: Any?, eventSink events: @escaping FlutterEventSink) -> FlutterError? { sink = events; return nil }
    func onCancel(withArguments arguments: Any?) -> FlutterError? { sink = nil; return nil }

    private func emit(_ type: String, _ data: [String: Any] = [:]) {
        var m = data
        m["type"] = type
        if type == "state" { lastState = m }
        let send: () -> Void = { [weak self] in self?.sink?(m) }
        if Thread.isMainThread { send() } else { DispatchQueue.main.async(execute: send) }
    }

    private func state(_ s: String, reason: String? = nil, extra: [String: Any] = [:]) {
        var d = extra
        d["state"] = s
        if let r = reason { d["reason"] = r }
        emit("state", d)
    }

    private func note(_ s: String) {
        log.append(s)
        if log.count > 300 { log.removeFirst(log.count - 300) }
    }

    // MARK: channel

    private func handle(_ call: FlutterMethodCall, _ result: @escaping FlutterResult) {
        let a = call.arguments as? [String: Any] ?? [:]
        switch call.method {
        case "start":
            let dir = (a["inbox"] as? String) ?? NSTemporaryDirectory()
            let url = URL(fileURLWithPath: dir, isDirectory: true)
            try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
            sweepParts(url)
            inbox = url
            begin(userAsked: false)
            result(true)
        case "stop":
            started = false
            closeSession("stop")
            browser.stop()
            result(true)
        case "status":
            result(lastState)
        case "requestPermission":
            begin(userAsked: true)
            result(true)
        case "list":
            guard camera != nil, catalogReady else { return result(FlutterError(code: "no_camera", message: "no camera", details: nil)) }
            result(listPage(cursor: (a["cursor"] as? Int) ?? 0, limit: (a["limit"] as? Int) ?? 60, refresh: (a["refresh"] as? Bool) ?? false))
        case "thumbnail":
            guard let h = a["handle"] as? Int, let f = fileOfHandle[h] else { return result(nil) }
            f.requestThumbnailData(options: [.imageSourceThumbnailMaxPixelSize: 256]) { data, _ in
                DispatchQueue.main.async { result(data.map { FlutterStandardTypedData(bytes: $0) }) }
            }
        case "import":
            guard camera != nil else { return result(FlutterError(code: "no_camera", message: "no camera", details: nil)) }
            let hs = (a["handles"] as? [Int]) ?? []
            result(true)
            startImport(hs.compactMap { fileOfHandle[$0] })
        case "cancelImport":
            importCancelled = true
            importProgress?.cancel()
            result(true)
        case "diagnostics":
            result(diagnostics())
        default:
            result(FlutterMethodNotImplemented)
        }
    }

    // MARK: browsing, permission, session

    /// Starts browsing once the system lets this app read cameras. The first call shows the system prompt ("permission" state).
    private func begin(userAsked: Bool) {
        if started && !userAsked { return }
        started = true
        let status = browser.contentsAuthorizationStatus
        note("contents authorization: \(status.rawValue)")
        if status == .authorized { browser.start(); lookAtDevices(); return }
        if status == .denied || status == .restricted {
            state("error", reason: "permission_denied")
            if userAsked, let u = URL(string: UIApplication.openSettingsURLString) { UIApplication.shared.open(u) }
            return
        }
        state("permission")
        browser.requestContentsAuthorization { [weak self] st in
            DispatchQueue.main.async {
                guard let self = self, self.started else { return }
                self.note("contents authorization answered: \(st.rawValue)")
                if st == .authorized { self.browser.start(); self.lookAtDevices() } else { self.state("error", reason: "permission_denied") }
            }
        }
    }

    private func lookAtDevices() {
        if camera != nil { return }
        if let cam = browser.devices?.compactMap({ $0 as? ICCameraDevice }).first { open(cam) } else if (lastState["state"] as? String) != "unplugged" { state("none") }
    }

    private func open(_ cam: ICCameraDevice) {
        guard camera == nil else { return }
        note("camera '\(cam.name ?? "")' transport=\(cam.transportType ?? "?") usb vid=\(cam.usbVendorID) pid=\(cam.usbProductID) locked=\(cam.isLocked) restricted=\(cam.isAccessRestrictedAppleDevice)")
        camera = cam
        catalogReady = false
        resetHandles()
        cam.delegate = self
        state("opening")
        cam.requestOpenSession()
    }

    private func resetHandles() {
        nextHandle = 1
        fileOfHandle = [:]
        handleOfFile = [:]
        order = []
        skippedVideos = 0
    }

    private func closeSession(_ why: String) {
        note("session closed: \(why)")
        importCancelled = true
        importProgress?.cancel()
        if let c = camera, c.hasOpenSession { c.requestCloseSession() }
        camera = nil
        catalogReady = false
    }

    private func gone() {
        guard camera != nil else { return }
        note("camera gone (importing=\(importing))")
        importCancelled = true
        importProgress?.cancel()
        camera = nil
        catalogReady = false
        if let d = inbox { sweepParts(d) }
        state("unplugged")
        // another camera may already be on the bus
        DispatchQueue.main.async { [weak self] in self?.lookAtDevices() }
    }

    func deviceBrowser(_ browser: ICDeviceBrowser, didAdd device: ICDevice, moreComing: Bool) {
        guard let cam = device as? ICCameraDevice else { return }
        DispatchQueue.main.async { [weak self] in self?.open(cam) }
    }

    func deviceBrowser(_ browser: ICDeviceBrowser, didRemove device: ICDevice, moreGoing: Bool) {
        DispatchQueue.main.async { [weak self] in
            if let self = self, let c = self.camera, c === device { self.gone() }
        }
    }

    func deviceBrowserDidCancelSuspendOperations(_ browser: ICDeviceBrowser) {}
    func deviceBrowserWillSuspendOperations(_ browser: ICDeviceBrowser) { note("browser will suspend (app goes to the background)") }
    func deviceBrowserDidSuspendOperations(_ browser: ICDeviceBrowser) {}
    func deviceBrowserDidResumeOperations(_ browser: ICDeviceBrowser) {
        note("browser resumed")
        DispatchQueue.main.async { [weak self] in self?.lookAtDevices() }
    }

    // MARK: ICDeviceDelegate / ICCameraDeviceDelegate

    func didRemove(_ device: ICDevice) { DispatchQueue.main.async { [weak self] in if self?.camera === device { self?.gone() } } }

    func device(_ device: ICDevice, didOpenSessionWithError error: Error?) {
        DispatchQueue.main.async { [weak self] in
            guard let self = self, self.camera === device else { return }
            if let e = error {
                self.note("open session failed: \(e.localizedDescription)")
                self.camera = nil
                self.state("error", reason: "open_failed")
            }
        }
    }

    func device(_ device: ICDevice, didCloseSessionWithError error: Error?) {
        DispatchQueue.main.async { [weak self] in
            guard let self = self, self.camera === device else { return }
            self.note("session ended by the camera: \(error?.localizedDescription ?? "no error")")
            self.gone()
        }
    }

    func deviceDidBecomeReady(_ device: ICDevice) {}

    func device(_ device: ICDevice, didEncounterError error: Error?) { note("device error: \(error?.localizedDescription ?? "?")") }

    func deviceDidBecomeReady(withCompleteContentCatalog device: ICCameraDevice) {
        DispatchQueue.main.async { [weak self] in
            guard let self = self, self.camera === device else { return }
            let files = self.mediaFiles(of: device)
            self.note("catalog complete: \(files.count) media files")
            if device.isLocked { self.note("camera reports its storage locked") }
            self.catalogReady = true
            self.state("ready", extra: [
                "device": ["manufacturer": "", "model": device.name ?? device.productKind ?? "", "serial": device.uuidString ?? ""],
                "card": ["total": files.count, "storages": [["id": 0]]],
            ])
        }
    }

    func cameraDevice(_ camera: ICCameraDevice, didAdd items: [ICCameraItem]) {
        DispatchQueue.main.async { [weak self] in
            guard let self = self, self.camera === camera, self.catalogReady else { return }
            let files = items.compactMap { $0 as? ICCameraFile }.filter { Self.accepted($0) }
            if files.isEmpty { return }
            for f in files { _ = self.handle(for: f) }
            self.note("new shots: \(files.map { $0.name ?? "?" }.joined(separator: ", "))")
            self.emit("newShots", ["groups": self.groups(of: files).map { self.groupMap($0) }, "total": self.mediaFiles(of: camera).count])
        }
    }

    func cameraDevice(_ camera: ICCameraDevice, didReceiveThumbnail thumbnail: CGImage?, for item: ICCameraItem, error: Error?) {}
    func cameraDevice(_ camera: ICCameraDevice, didReceiveMetadata metadata: [AnyHashable: Any]?, for item: ICCameraItem, error: Error?) {}
    func cameraDevice(_ camera: ICCameraDevice, didRemove items: [ICCameraItem]) {}
    func cameraDevice(_ camera: ICCameraDevice, didRenameItems items: [ICCameraItem]) {}
    func cameraDeviceDidChangeCapability(_ camera: ICCameraDevice) {}
    func cameraDevice(_ camera: ICCameraDevice, didReceivePTPEvent eventData: Data) {}
    func cameraDevice(_ camera: ICCameraDevice, didCompleteDeleteFilesWithError error: Error?) {}
    func cameraDeviceDidRemoveAccessRestriction(_ device: ICDevice) {}
    func cameraDeviceDidEnableAccessRestriction(_ device: ICDevice) {
        DispatchQueue.main.async { [weak self] in
            guard let self = self, self.camera === device else { return }
            self.note("camera restricts access (locked, or waiting for the trust prompt)")
            self.state("error", reason: "no_storage")
        }
    }

    // MARK: files

    private static let rawExt: Set<String> = ["arw", "raf", "dng", "cr2", "cr3", "nef", "orf", "rw2"]
    private static let jpegExt: Set<String> = ["jpg", "jpeg"]
    private static let heifExt: Set<String> = ["heif", "heic", "hif"]
    private static let videoExt: Set<String> = ["mp4", "mov", "mxf", "avi", "mts", "m2ts", "mkv", "lrv"]

    private static func ext(_ n: String) -> String { (n as NSString).pathExtension.lowercased() }
    private static func stem(_ n: String) -> String { ((n as NSString).deletingPathExtension).lowercased() }

    private static func kind(_ f: ICCameraFile) -> String {
        let e = ext(f.name ?? "")
        if rawExt.contains(e) || (f.isRaw && !jpegExt.contains(e) && !heifExt.contains(e)) { return "raw" }
        if jpegExt.contains(e) { return "jpeg" }
        if heifExt.contains(e) { return "heif" }
        if videoExt.contains(e) { return "video" }
        return "other"
    }

    private static func accepted(_ f: ICCameraFile) -> Bool { ["raw", "jpeg", "heif"].contains(kind(f)) }

    private func dateMs(_ f: ICCameraFile) -> Int64 {
        let d = f.exifCreationDate ?? f.fileCreationDate ?? f.creationDate ?? f.fileModificationDate
        return Int64((d?.timeIntervalSince1970 ?? 0) * 1000)
    }

    private func key(_ f: ICCameraFile) -> String { "usb:\((f.name ?? "").lowercased())|\(Int64(f.fileSize))|\(dateMs(f) / 1000)" }

    private func dir(_ f: ICCameraFile) -> String { f.parentFolder?.name ?? "" }

    /// Every still on the camera (the paired RAW of a JPEG is added when the camera lists it only as a pair).
    private func mediaFiles(of cam: ICCameraDevice) -> [ICCameraFile] {
        var out: [ICCameraFile] = []
        var seen = Set<ObjectIdentifier>()
        func add(_ f: ICCameraFile) { if seen.insert(ObjectIdentifier(f)).inserted { out.append(f) } }
        for i in cam.mediaFiles ?? [] {
            guard let f = i as? ICCameraFile else { continue }
            add(f)
            if let p = f.pairedRawImage { add(p) }
        }
        return out.filter { Self.accepted($0) }
    }

    private func handle(for f: ICCameraFile) -> Int {
        if let h = handleOfFile[ObjectIdentifier(f)] { return h }
        let h = nextHandle
        nextHandle += 1
        fileOfHandle[h] = f
        handleOfFile[ObjectIdentifier(f)] = h
        return h
    }

    private func fileMap(_ f: ICCameraFile) -> [String: Any] {
        ["handle": handle(for: f), "name": f.name ?? "", "size": Int64(f.fileSize), "dateMs": dateMs(f), "key": key(f), "kind": Self.kind(f)]
    }

    private struct Group { let dir: String; let stem: String; var files: [ICCameraFile]; var dateMs: Int64 }

    /// RAW + JPEG with the same name in the same folder = one exposure; JPEG / HEIF first (instant preview), then RAW.
    private func groups(of files: [ICCameraFile]) -> [Group] {
        var order: [String] = []
        var m: [String: Group] = [:]
        for f in files {
            let k = dir(f).lowercased() + "/" + Self.stem(f.name ?? "")
            if m[k] == nil { m[k] = Group(dir: dir(f), stem: Self.stem(f.name ?? ""), files: [], dateMs: 0); order.append(k) }
            m[k]!.files.append(f)
            m[k]!.dateMs = max(m[k]!.dateMs, dateMs(f))
        }
        return order.map { k in
            var g = m[k]!
            g.files.sort { (Self.kind($0) == "raw" ? 1 : 0) < (Self.kind($1) == "raw" ? 1 : 0) }
            return g
        }
    }

    private func groupMap(_ g: Group) -> [String: Any] {
        ["gid": g.dir.lowercased() + "/" + g.stem, "stem": g.stem, "dir": g.dir, "dateMs": g.dateMs, "files": g.files.map { fileMap($0) }]
    }

    private func listPage(cursor: Int, limit: Int, refresh: Bool) -> [String: Any] {
        guard let cam = camera else { return [:] }
        if refresh || order.isEmpty {
            let all = mediaFiles(of: cam)
            skippedVideos = (cam.mediaFiles ?? []).compactMap { $0 as? ICCameraFile }.filter { Self.kind($0) == "video" }.count
            order = all.sorted { dateMs($0) > dateMs($1) }
        }
        let end = min(order.count, cursor + max(1, limit))
        let slice = cursor < end ? Array(order[cursor..<end]) : []
        let gs = groups(of: slice).sorted { $0.dateMs > $1.dateMs }
        return ["groups": gs.map { groupMap($0) }, "next": end, "done": end >= order.count, "total": order.count, "skippedVideos": skippedVideos, "cancelled": false]
    }

    // MARK: import

    /// One file after another into the inbox as "name.part", renamed when whole, so a partial file is never visible and an unplug leaves nothing behind.
    private func startImport(_ files: [ICCameraFile]) {
        guard !importing, let dir = inbox else { return }
        importing = true
        importCancelled = false
        let photos = Set(files.map { self.dir($0).lowercased() + "/" + Self.stem($0.name ?? "") })
        var photoIndex: [String: Int] = [:]
        for f in files { let g = self.dir(f).lowercased() + "/" + Self.stem(f.name ?? ""); if photoIndex[g] == nil { photoIndex[g] = photoIndex.count + 1 } }
        emit("importQueue", ["files": files.count, "photos": photos.count])
        UIApplication.shared.isIdleTimerDisabled = true  // a long transfer must not be cut by the screen lock
        var done = 0, failed = 0, consecutive = 0
        var aborted: String?
        var queue = files[...]

        func finish() {
            importing = false
            importProgress = nil
            progressTimer?.invalidate()
            progressTimer = nil
            UIApplication.shared.isIdleTimerDisabled = false
            sweepParts(dir)
            emit("importQueueDone", ["imported": done, "failed": failed, "cancelled": importCancelled && aborted == nil, "aborted": aborted as Any])
            if aborted == "gone" { emit("gone") }
        }

        func next() {
            guard let f = queue.popFirst() else { return finish() }
            if importCancelled || camera == nil { return finish() }
            let name = f.name ?? "file"
            let size = Int64(f.fileSize)
            let g = self.dir(f).lowercased() + "/" + Self.stem(name)
            let base: [String: Any] = ["key": key(f), "name": name, "size": size, "index": photoIndex[g] ?? 1, "total": photos.count]
            let free = (try? dir.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?.volumeAvailableCapacityForImportantUsage ?? 0
            if free > 0 && free < size + 200 * 1024 * 1024 {
                note("import \(name): not enough space (\(free) free, \(size) needed)")
                emit("importFailed", base.merging(["reason": "space"]) { $1 })
                failed += 1
                aborted = "space"
                return finish()
            }
            var outName = name
            let existing = dir.appendingPathComponent(outName)
            if let have = (try? FileManager.default.attributesOfItem(atPath: existing.path))?[.size] as? Int64 {
                if have == size {
                    note("import \(name): already in the inbox, not copied again")
                    emit("importDone", base.merging(["path": existing.path, "dateMs": dateMs(f), "outName": outName, "again": true]) { $1 })
                    done += 1
                    consecutive = 0
                    return next()
                }
                outName = (name as NSString).deletingPathExtension + "-\(dateMs(f) / 1000)." + (name as NSString).pathExtension
            }
            let partName = outName + ".part"
            try? FileManager.default.removeItem(at: dir.appendingPathComponent(partName))
            emit("importStart", base)
            let t0 = Date()
            let options: [ICDownloadOption: Any] = [.downloadsDirectoryURL: dir, .saveAsFilename: partName, .overwrite: true]
            let progress = f.requestDownload(options: options) { saved, error in
                DispatchQueue.main.async {
                    self.progressTimer?.invalidate()
                    self.progressTimer = nil
                    let part = dir.appendingPathComponent(saved ?? partName)
                    let final = dir.appendingPathComponent(outName)
                    let have = (try? FileManager.default.attributesOfItem(atPath: part.path))?[.size] as? Int64 ?? 0
                    if error == nil, have > 0 {
                        try? FileManager.default.removeItem(at: final)
                        do {
                            try FileManager.default.moveItem(at: part, to: final)
                            let ms = Int(Date().timeIntervalSince(t0) * 1000)
                            self.note("import \(name): \(have) B in \(ms) ms")
                            self.emit("importDone", base.merging(["path": final.path, "dateMs": self.dateMs(f), "outName": outName, "again": false, "ms": ms]) { $1 })
                            done += 1
                            consecutive = 0
                            return next()
                        } catch { self.note("import \(name): rename failed: \(error.localizedDescription)") }
                    }
                    try? FileManager.default.removeItem(at: part)
                    if self.importCancelled && self.camera != nil { return finish() }
                    if self.camera == nil || self.camera?.hasOpenSession == false {
                        self.note("import \(name): failed and the camera is gone")
                        self.emit("importFailed", base.merging(["reason": "gone"]) { $1 })
                        failed += 1
                        aborted = "gone"
                        return finish()
                    }
                    self.note("import \(name): failed (\(error?.localizedDescription ?? "empty file"))")
                    self.emit("importFailed", base.merging(["reason": "failed"]) { $1 })
                    failed += 1
                    consecutive += 1
                    if consecutive >= 2 { aborted = "failed"; return finish() }
                    next()
                }
            }
            importProgress = progress
            // progress: poll the NSProgress, only changes are sent (the Dart side turns bytes / size into a fraction)
            var last: Int64 = -1
            progressTimer?.invalidate()
            progressTimer = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { [weak self] _ in
                guard let self = self, let p = self.importProgress else { return }
                let bytes = p.completedUnitCount > 0 && p.totalUnitCount > 1000 ? min(size, p.completedUnitCount * size / max(1, p.totalUnitCount)) : Int64(p.fractionCompleted * Double(size))
                if bytes != last { last = bytes; self.emit("importProgress", ["name": name, "bytes": bytes, "size": size]) }
            }
        }
        next()
    }

    private func sweepParts(_ dir: URL) {
        for u in (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? [] where u.pathExtension == "part" {
            try? FileManager.default.removeItem(at: u)
        }
    }

    private func diagnostics() -> String {
        var s = "Dapple usb diagnostics\n"
        s += "ios=\(UIDevice.current.systemVersion) phone=\(UIDevice.current.model)\n"
        s += "state=\(lastState["state"] ?? "?") reason=\(lastState["reason"] ?? "-") browsing=\(browser.isBrowsing) suspended=\(browser.isSuspended) contents=\(browser.contentsAuthorizationStatus.rawValue)\n"
        for d in browser.devices ?? [] {
            s += "device '\(d.name ?? "")' kind=\(d.productKind ?? "") transport=\(d.transportType ?? "") vid=\(d.usbVendorID) pid=\(d.usbProductID) session=\(d.hasOpenSession)\n"
        }
        if let c = camera { s += "catalogReady=\(catalogReady) files=\(mediaFiles(of: c).count) skippedVideos=\(skippedVideos) capabilities=\(c.capabilities.joined(separator: ","))\n" }
        return s + "----\n" + log.joined(separator: "\n")
    }
}
