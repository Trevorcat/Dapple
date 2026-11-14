import Flutter
import ImageIO
import Photos
import PhotosUI
import UIKit
import UniformTypeIdentifiers

// Engine start-up (GPU probe) and the media-library side of the platform interface (docs/ios/platform-interface.md):
//   channel dapple/media         queryNew / copyToFile / saveToGallery / openApp      (same names and result maps as the Kotlin MediaBridge)
//   event channel dapple/media_events                                               (library change notifications, PHPhotoLibraryChangeObserver)
//   channel dapple/photos        authorizationStatus / requestAuthorization / presentLimitedLibraryPicker / pick (PHPicker, no permission needed)
// Ids and uris: "ph:<localIdentifier>:<resource index>" / "ph://<localIdentifier>#<resource index>" (index into PHAssetResource.assetResources(for:)).

@_silgen_name("dapple_gpu_configure") private func dapple_gpu_configure_c(_ dir: UnsafePointer<CChar>?, _ version: UnsafePointer<CChar>?, _ fingerprint: UnsafePointer<CChar>?)
@_silgen_name("dapple_gpu_warmup") private func dapple_gpu_warmup_c()
@_silgen_name("dapple_film_load_pack_memory") private func dapple_film_load_pack_memory_c(_ data: UnsafeRawPointer?, _ size: Int) -> Int32

public final class DappleEnginePlugin: NSObject, FlutterPlugin, FlutterStreamHandler, PHPhotoLibraryChangeObserver, PHPickerViewControllerDelegate {
    private var events: FlutterEventSink?
    /// Set by the Dart test-run code (ios_autotest / ios_debug_open) for the lifetime of one test launch; never persisted.
    private static var testRunActive = false
    private var pickResult: FlutterResult?
    private var pickDest: String = ""
    private var pickKnown: Set<String> = []
    private let io = DispatchQueue(label: "dapple.photos.io", qos: .userInitiated)

    private static var viewPlugin: DappleViewPlugin?
    private static var capturePlugin: DappleCapturePlugin?
    private static var livePlugin: DappleLivePlugin?
    private static var usbCamPlugin: DappleUsbCamPlugin?

    public static func register(with registrar: FlutterPluginRegistrar) {
        let p = DappleEnginePlugin()
        viewPlugin = DappleViewPlugin(registrar: registrar)  // dapple/view (zero-copy display)
        capturePlugin = DappleCapturePlugin(registrar: registrar)  // dapple/capture (AVFoundation)
        livePlugin = DappleLivePlugin(registrar: registrar)  // dapple/live (video -> Live Photo tool: AVFoundation)
        usbCamPlugin = DappleUsbCamPlugin(registrar: registrar)  // dapple/usbcam (camera on the cable: ImageCaptureCore)
        startGpu()
        FlutterMethodChannel(name: "dapple/media", binaryMessenger: registrar.messenger()).setMethodCallHandler(p.handleMedia)
        FlutterMethodChannel(name: "dapple/photos", binaryMessenger: registrar.messenger()).setMethodCallHandler(p.handlePhotos)
        // one-line ingest outcomes from Dart into the unified log (visible in release builds: Console.app / `log stream`, subsystem-free NSLog)
        FlutterMethodChannel(name: "dapple/log", binaryMessenger: registrar.messenger()).setMethodCallHandler { call, result in
            if call.method == "line", let l = call.arguments as? String { NSLog("dapple ingest: %@", l) }
            result(nil)
        }
        // AI masks (docs/legal/ai-models.md): the models are bundled by the Runner build phase "Bundle AI models" as <app>/ai-models and read in place
        // by the engine (no copy: bundle files are ordinary readable files). `prepare` -> the directory, or null when the app carries no models.
        FlutterMethodChannel(name: "dapple/ai", binaryMessenger: registrar.messenger()).setMethodCallHandler { call, result in
            guard call.method == "prepare" else { result(FlutterMethodNotImplemented); return }
            let dir = (Bundle.main.resourcePath ?? "") + "/ai-models"
            var isDir: ObjCBool = false
            let ok = FileManager.default.fileExists(atPath: dir, isDirectory: &isDir) && isDir.boolValue
                && ((try? FileManager.default.contentsOfDirectory(atPath: dir))?.isEmpty == false)
            result(ok ? dir : nil)
        }
        FlutterEventChannel(name: "dapple/media_events", binaryMessenger: registrar.messenger()).setStreamHandler(p)
        registrar.publish(p)
    }

    /// The GPU probe (Metal self-test + film check, persisted verdicts) starts with the app, like the Kotlin side does.
    static func startGpu() {
        // Android's default is "auto" (GPU once the probe passed, CPU until then); the engine's default elsewhere is off, so the app asks for it
        setenv("DAPPLE_GPU", "auto", 0)
        let caches = NSSearchPathForDirectoriesInDomains(.cachesDirectory, .userDomainMask, true).first ?? NSTemporaryDirectory()
        let dir = caches + "/dapple-gpu"
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        var u = utsname()
        uname(&u)
        let machine = withUnsafePointer(to: &u.machine) { $0.withMemoryRebound(to: CChar.self, capacity: 1) { String(cString: $0) } }
        let fp = "\(UIDevice.current.systemName) \(UIDevice.current.systemVersion) \(machine)"
        let ver = (Bundle.main.infoDictionary?["CFBundleVersion"] as? String) ?? "0"
        dapple_gpu_configure_c(dir, ver, fp)
        // The GPU verdict includes a film frame compared with the CPU (gpu_context.cpp "film check"): that needs the film pack, which Dart only loads
        // later, so the pack bundled as a Flutter asset is read here before the probe starts (Dart installs the same bytes again, harmless).
        let pack = Bundle.main.bundlePath + "/Frameworks/App.framework/flutter_assets/assets/film/dapple.dfp"
        if let d = FileManager.default.contents(atPath: pack) { _ = d.withUnsafeBytes { dapple_film_load_pack_memory_c($0.baseAddress, d.count) } }
        dapple_gpu_warmup_c()
    }

    // MARK: dapple/media

    private func handleMedia(_ call: FlutterMethodCall, _ result: @escaping FlutterResult) {
        let a = call.arguments as? [String: Any] ?? [:]
        switch call.method {
        case "queryNew":
            let limit = (a["limit"] as? Int) ?? 100
            let since = (a["sinceSec"] as? Int) ?? 0
            io.async { let r = Self.queryNew(limit: limit, sinceSec: since); DispatchQueue.main.async { result(r) } }
        case "copyToFile":
            guard let uri = a["uri"] as? String, let dest = a["dest"] as? String else { return result(FlutterError(code: "args", message: "uri/dest", details: nil)) }
            io.async { Self.copyToFile(uri: uri, dest: dest) { r in DispatchQueue.main.async { result(r) } } }
        case "saveToGallery":
            guard let path = a["path"] as? String else { return result(FlutterError(code: "args", message: "path", details: nil)) }
            let name = (a["name"] as? String) ?? (path as NSString).lastPathComponent
            Self.saveToGallery(path: path, name: name) { r in DispatchQueue.main.async { result(r) } }
        case "toPng16":
            // any ImageIO image (HEIC, TIFF ...) -> 16-bit PNG in its own colour space (DeepImage.swift); used for files that did not come through Photos (Files app, Documents)
            guard let src = a["src"] as? String, let dest = a["dest"] as? String else { return result(FlutterError(code: "args", message: "src/dest", details: nil)) }
            io.async {
                let ok = DeepImage.convert(src: src, dst: dest)
                DispatchQueue.main.async { result(ok ? dest : FlutterError(code: "convert", message: "cannot convert \((src as NSString).lastPathComponent)", details: nil)) }
            }
        case "openApp":
            result(false)  // Android package names do not exist here
        default:
            result(FlutterMethodNotImplemented)
        }
    }

    /// JPEG, PNG and RAW resources are read as they are (the engine honours the colour space and depth they carry); anything else
    /// (HEIC ...) is converted to a 16-bit PNG by copyToFile.
    private static func passthrough(_ r: PHAssetResource) -> Bool { isRaw(r) || isStill(UTType(r.uniformTypeIdentifier)) }

    private static func isStill(_ t: UTType?) -> Bool { t?.conforms(to: .jpeg) == true || t?.conforms(to: .png) == true }

    private static func isRaw(_ r: PHAssetResource) -> Bool {
        if let t = UTType(r.uniformTypeIdentifier), t.conforms(to: .rawImage) { return true }
        return (r.originalFilename as NSString).pathExtension.lowercased() == "dng"
    }

    /// The resource the engine should read: the RAW (alternate photo of a RAW+JPEG pair, or the DNG of a ProRAW asset), else the original photo.
    private static func pickResource(_ rs: [PHAssetResource]) -> (Int, PHAssetResource)? {
        if let i = rs.firstIndex(where: { ($0.type == .alternatePhoto || $0.type == .photo) && isRaw($0) }) { return (i, rs[i]) }
        if let i = rs.firstIndex(where: { $0.type == .photo }) { return (i, rs[i]) }
        return nil
    }

    private static func queryNew(limit: Int, sinceSec: Int) -> [[String: Any]] {
        let st = PHPhotoLibrary.authorizationStatus(for: .readWrite)
        NSLog("dapple photos: queryNew authorization %d", st.rawValue)
        guard st == .authorized || st == .limited else { return [] }
        let o = PHFetchOptions()
        o.predicate = NSPredicate(format: "mediaType == %d AND creationDate >= %@", PHAssetMediaType.image.rawValue, Date(timeIntervalSince1970: TimeInterval(sinceSec)) as NSDate)
        o.sortDescriptors = [NSSortDescriptor(key: "creationDate", ascending: true)]  // oldest first, like the MediaStore listing
        o.fetchLimit = limit
        let res = PHAsset.fetchAssets(with: o)
        NSLog("dapple photos: status %d, %d assets match (since %d)", st.rawValue, res.count, sinceSec)
        // what Dapple itself saved (captures, exports: album "Dapple") is not an arrival: the capture flow ingests it from its own files
        var own = Set<String>()
        let ao = PHFetchOptions(); ao.predicate = NSPredicate(format: "localizedTitle == %@", "Dapple")
        PHAssetCollection.fetchAssetCollections(with: .album, subtype: .albumRegular, options: ao).enumerateObjects { a, _, _ in
            PHAsset.fetchAssets(in: a, options: nil).enumerateObjects { x, _, _ in own.insert(x.localIdentifier) }
        }
        if let id = UserDefaults.standard.string(forKey: albumKey), let a = PHAssetCollection.fetchAssetCollections(withLocalIdentifiers: [id], options: nil).firstObject {
            PHAsset.fetchAssets(in: a, options: nil).enumerateObjects { x, _, _ in own.insert(x.localIdentifier) }
        }
        var out: [[String: Any]] = []
        res.enumerateObjects { asset, _, _ in
            if own.contains(asset.localIdentifier) { return }
            let rs = PHAssetResource.assetResources(for: asset)
            guard let (idx, r) = pickResource(rs) else { return }
            let ms = Int((asset.creationDate ?? Date()).timeIntervalSince1970 * 1000)
            out.append([
                "id": "ph:\(asset.localIdentifier):\(idx)",
                "name": passthrough(r) ? r.originalFilename : (r.originalFilename as NSString).deletingPathExtension + ".png",  // HEIC etc. arrive converted (16-bit PNG)
                "addedMs": ms,
                "uri": "ph://\(asset.localIdentifier)#\(idx)",
                "size": 0,
            ])
        }
        return out
    }

    private static func parse(uri: String) -> (String, Int)? {
        guard uri.hasPrefix("ph://"), let h = uri.lastIndex(of: "#") else { return nil }
        let id = String(uri[uri.index(uri.startIndex, offsetBy: 5)..<h])
        guard let idx = Int(uri[uri.index(after: h)...]) else { return nil }
        return (id, idx)
    }

    /// Writes the resource to `dest` (the app ignores the returned path): JPEG, PNG and RAW unchanged, anything else (HEIC ...) converted to a 16-bit PNG.
    private static func copyToFile(uri: String, dest: String, done: @escaping (Any) -> Void) {
        guard let (id, idx) = parse(uri: uri), let asset = PHAsset.fetchAssets(withLocalIdentifiers: [id], options: nil).firstObject else {
            return done(FlutterError(code: "notfound", message: "asset not found: \(uri)", details: nil))
        }
        let rs = PHAssetResource.assetResources(for: asset)
        guard idx < rs.count else { return done(FlutterError(code: "notfound", message: "resource", details: nil)) }
        let r = rs[idx]
        let o = PHAssetResourceRequestOptions()
        o.isNetworkAccessAllowed = true  // iCloud originals
        let tmp = NSTemporaryDirectory() + UUID().uuidString + "." + (r.originalFilename as NSString).pathExtension
        try? FileManager.default.removeItem(atPath: tmp)
        let raw = isRaw(r), jpeg = Self.isStill(UTType(r.uniformTypeIdentifier))
        PHAssetResourceManager.default().writeData(for: r, toFile: URL(fileURLWithPath: tmp), options: o) { err in
            if let err = err { return done(FlutterError(code: "write", message: err.localizedDescription, details: nil)) }
            if place(tmp: tmp, dest: dest, raw: raw, jpeg: jpeg) { done(dest) }
            else { done(FlutterError(code: "convert", message: "cannot convert \(r.originalFilename) to PNG", details: nil)) }
        }
    }

    /// Moves a freshly written original to `dest`: RAW, JPEG and PNG unchanged (`jpeg` = a still the engine reads itself), everything
    /// else converted by [DeepImage.convert].
    static func place(tmp: String, dest: String, raw: Bool, jpeg: Bool) -> Bool {
        try? FileManager.default.removeItem(atPath: dest)
        // the cache's src folder does not exist on a fresh install (the Dart side only names the path)
        try? FileManager.default.createDirectory(atPath: (dest as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
        if raw || jpeg {
            do { try FileManager.default.moveItem(atPath: tmp, toPath: dest); return true } catch { return false }
        }
        let ok = DeepImage.convert(src: tmp, dst: dest)
        try? FileManager.default.removeItem(atPath: tmp)
        return ok
    }

    private static let albumKey = "dapple.album.id"

    /// Adds a finished file to the Photos library (album "Dapple"; needs add-only access at least). Result: the new asset's "ph://" uri.
    private static func saveToGallery(path: String, name: String, done: @escaping (Any) -> Void) {
        PHPhotoLibrary.requestAuthorization(for: .addOnly) { st in
            guard st == .authorized || st == .limited else { return done(FlutterError(code: "denied", message: "no permission to add photos", details: nil)) }
            var assetPh: PHObjectPlaceholder?
            var albumPh: PHObjectPlaceholder?
            // A Motion Photo made by the engine (`*_MP.jpg`: JPEG + MP4) is saved as a Live Photo: still + paired MOV with one content identifier, one asset.
            let live: LivePhoto.Parts? = name.hasSuffix("_MP.jpg") ? LivePhoto.prepare(path: path) : nil
            PHPhotoLibrary.shared().performChanges({
                let req = PHAssetCreationRequest.forAsset()
                let o = PHAssetResourceCreationOptions()
                o.originalFilename = name
                if let l = live {
                    req.addResource(with: .photo, fileURL: l.stillURL, options: o)
                    let mo = PHAssetResourceCreationOptions()
                    mo.originalFilename = (name as NSString).deletingPathExtension + ".mov"
                    req.addResource(with: .pairedVideo, fileURL: l.movieURL, options: mo)
                } else {
                    req.addResource(with: .photo, fileURL: URL(fileURLWithPath: path), options: o)
                }
                assetPh = req.placeholderForCreatedAsset
                guard let ph = assetPh else { return }
                var album: PHAssetCollection?
                if let id = UserDefaults.standard.string(forKey: albumKey) { album = PHAssetCollection.fetchAssetCollections(withLocalIdentifiers: [id], options: nil).firstObject }
                if let a = album, let cr = PHAssetCollectionChangeRequest(for: a) { cr.addAssets([ph] as NSArray) }
                else {
                    let cr = PHAssetCollectionChangeRequest.creationRequestForAssetCollection(withTitle: "Dapple")
                    cr.addAssets([ph] as NSArray)
                    albumPh = cr.placeholderForCreatedAssetCollection
                }
            }) { ok, err in
                if let l = live { LivePhoto.cleanup(l) }
                if let a = albumPh?.localIdentifier { UserDefaults.standard.set(a, forKey: albumKey) }
                if ok, let id = assetPh?.localIdentifier { done("ph://\(id)#0") }
                else { done(FlutterError(code: "save", message: err?.localizedDescription ?? "save failed", details: nil)) }
            }
        }
    }

    // MARK: dapple/photos

    private func statusName(_ s: PHAuthorizationStatus) -> String {
        switch s {
        case .authorized: return "authorized"
        case .limited: return "limited"
        case .denied: return "denied"
        case .restricted: return "restricted"
        default: return "notDetermined"
        }
    }

    /// Read access as the app sees it. "仅添加" in Settings leaves the read-write level at denied while the add-only level is authorized:
    /// that is saving allowed, not a refusal, and is reported as "addOnly".
    private func readStatusName() -> String {
        let rw = PHPhotoLibrary.authorizationStatus(for: .readWrite)
        if rw == .denied && PHPhotoLibrary.authorizationStatus(for: .addOnly) == .authorized { return "addOnly" }
        return statusName(rw)
    }

    private func topViewController() -> UIViewController? {
        var vc = UIApplication.shared.connectedScenes.compactMap { ($0 as? UIWindowScene)?.windows.first(where: { $0.isKeyWindow })?.rootViewController }.first
        while let p = vc?.presentedViewController { vc = p }
        return vc
    }

    private func handlePhotos(_ call: FlutterMethodCall, _ result: @escaping FlutterResult) {
        let a = call.arguments as? [String: Any] ?? [:]
        switch call.method {
        case "authorizationStatus":
            registerObserverIfAllowed()
            result(readStatusName())
        case "testRun":
            Self.testRunActive = (a["active"] as? Bool) ?? false
            result(nil)
        case "requestAuthorization":
            // only while this launch is a test run (the Dart side consumed a one-shot trigger and said so): nobody can answer the prompt then.
            // A trigger file in Documents never matters by itself: a normal launch always shows the system prompt.
            if Self.testRunActive {
                return result(readStatusName())
            }
            PHPhotoLibrary.requestAuthorization(for: .readWrite) { s in DispatchQueue.main.async { self.registerObserverIfAllowed(); result(s == .denied ? self.readStatusName() : self.statusName(s)) } }
        case "presentLimitedLibraryPicker":
            if let vc = topViewController() { PHPhotoLibrary.shared().presentLimitedLibraryPicker(from: vc) }
            result(nil)
        case "pickVideoFile":
            DappleVideoPicker.shared.pick(from: topViewController(), result: result)
        case "pick":
            // System picker (PHPicker): runs out of process, needs no permission. The chosen originals are copied into `dest` (the app inbox).
            guard pickResult == nil, let vc = topViewController() else { return result(FlutterError(code: "busy", message: "picker not available", details: nil)) }
            pickResult = result
            pickDest = (a["dest"] as? String) ?? NSTemporaryDirectory()
            pickKnown = Set((a["known"] as? [String]) ?? [])
            var cfg = PHPickerConfiguration(photoLibrary: PHPhotoLibrary.shared())
            cfg.selectionLimit = 0
            cfg.filter = .images
            cfg.preferredAssetRepresentationMode = .current  // the original file, no transcoding
            let p = PHPickerViewController(configuration: cfg)
            p.delegate = self
            vc.present(p, animated: true)
        default:
            result(FlutterMethodNotImplemented)
        }
    }

    public func picker(_ picker: PHPickerViewController, didFinishPicking results: [PHPickerResult]) {
        picker.dismiss(animated: true)
        let cb = pickResult
        let dest = pickDest
        let known = pickKnown
        pickResult = nil
        let group = DispatchGroup()
        let lock = NSLock()
        var out: [[String: Any]] = []
        try? FileManager.default.createDirectory(atPath: dest, withIntermediateDirectories: true)
        for (n, r) in results.enumerated() {
            let p = r.itemProvider
            let ids = p.registeredTypeIdentifiers
            // RAW first (DNG / ProRAW / camera RAW), then JPEG, then whatever image type is there
            let tid = ids.first(where: { UTType($0)?.conforms(to: .rawImage) == true }) ?? ids.first(where: { UTType($0)?.conforms(to: .jpeg) == true }) ?? ids.first(where: { UTType($0)?.conforms(to: .image) == true })
            // Identity: the Photos asset id, the same one the automatic scan builds its ids from ("ph:<id>:<resource>"), so a photo that is
            // already in the library is recognised and not copied again.
            let aid = r.assetIdentifier
            let key = aid.map { "ph:\($0)" } ?? "imp:\(p.suggestedName ?? "photo")_\(n)"
            let stem0 = p.suggestedName ?? "photo"
            if known.contains(where: { $0 == key || $0.hasPrefix(key + ":") }) {
                lock.lock(); out.append(["key": key, "name": stem0, "dup": true]); lock.unlock()
                continue
            }
            guard let type = tid else { lock.lock(); out.append(["key": key, "name": stem0, "dup": false, "error": "unsupported"]); lock.unlock(); continue }
            group.enter()
            p.loadFileRepresentation(forTypeIdentifier: type) { url, _ in
                defer { group.leave() }
                guard let url = url else {
                    lock.lock(); out.append(["key": key, "name": stem0, "dup": false, "error": "copy"]); lock.unlock()
                    return
                }
                let ut = UTType(type)
                let ext = url.pathExtension.isEmpty ? (ut?.preferredFilenameExtension ?? "bin") : url.pathExtension
                let stem = (p.suggestedName ?? url.deletingPathExtension().lastPathComponent)
                // RAW, JPEG and PNG are copied as they are (the engine reads their colour space and depth); HEIC / TIFF ... become 16-bit PNG (DeepImage)
                let keep = ut?.conforms(to: .rawImage) == true || Self.isStill(ut)
                let finalPath = "\(dest)/\(Int(Date().timeIntervalSince1970 * 1000) + n)_\(stem).\(keep ? ext : "png")"
                if keep { try? FileManager.default.copyItem(at: url, to: URL(fileURLWithPath: finalPath)) }
                else if !DeepImage.convert(src: url.path, dst: finalPath) {
                    NSLog("dapple pick: %@ convert failed", stem)
                    lock.lock(); out.append(["key": key, "name": stem0, "dup": false, "error": "copy"]); lock.unlock()
                    return
                }
                let size = ((try? FileManager.default.attributesOfItem(atPath: finalPath))?[.size] as? NSNumber)?.intValue ?? 0
                if size == 0 {
                    lock.lock(); out.append(["key": key, "name": stem0, "dup": false, "error": "copy"]); lock.unlock()
                    return
                }
                lock.lock(); out.append(["key": key, "name": "\(stem).\(keep ? ext : "png")", "dup": false, "path": finalPath, "size": size]); lock.unlock()
            }
        }
        group.notify(queue: .main) { cb?(out) }
    }

    // MARK: change notifications

    public func onListen(withArguments arguments: Any?, eventSink events: @escaping FlutterEventSink) -> FlutterError? {
        self.events = events
        wantObserver = true
        registerObserverIfAllowed()
        return nil
    }

    private var wantObserver = false, observing = false

    /// Registering a PHPhotoLibraryChangeObserver while the access is undetermined makes Photos show the permission prompt on its own, so it waits
    /// until the user has answered the app's own request (requestAuthorization).
    private func registerObserverIfAllowed() {
        guard wantObserver, !observing else { return }
        let st = PHPhotoLibrary.authorizationStatus(for: .readWrite)
        guard st == .authorized || st == .limited else { return }
        PHPhotoLibrary.shared().register(self)
        observing = true
    }

    public func onCancel(withArguments arguments: Any?) -> FlutterError? {
        wantObserver = false
        if observing { PHPhotoLibrary.shared().unregisterChangeObserver(self); observing = false }
        events = nil
        return nil
    }

    public func photoLibraryDidChange(_ changeInstance: PHChange) {
        DispatchQueue.main.async { self.events?(true) }
    }
}

public enum DappleEngine {
    // Referencing one engine symbol makes the linker keep the load command of dapple.framework, so dyld loads it at launch and
    // DynamicLibrary.process() sees the C API (dart:ffi); app/lib/native/dapple_bindings.dart falls back to opening it by name.
    public static var version: String { dapple_version_c().map { String(cString: $0) } ?? "" }
}
@_silgen_name("dapple_version") private func dapple_version_c() -> UnsafePointer<CChar>?
