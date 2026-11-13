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
    private var pickResult: FlutterResult?
    private var pickDest: String = ""
    private var pickKnown: Set<String> = []
    private let io = DispatchQueue(label: "dapple.photos.io", qos: .userInitiated)

    private static var viewPlugin: DappleViewPlugin?

    public static func register(with registrar: FlutterPluginRegistrar) {
        let p = DappleEnginePlugin()
        viewPlugin = DappleViewPlugin(registrar: registrar)  // dapple/view (zero-copy display)
        startGpu()
        FlutterMethodChannel(name: "dapple/media", binaryMessenger: registrar.messenger()).setMethodCallHandler(p.handleMedia)
        FlutterMethodChannel(name: "dapple/photos", binaryMessenger: registrar.messenger()).setMethodCallHandler(p.handlePhotos)
        // one-line ingest outcomes from Dart into the unified log (visible in release builds: Console.app / `log stream`, subsystem-free NSLog)
        FlutterMethodChannel(name: "dapple/log", binaryMessenger: registrar.messenger()).setMethodCallHandler { call, result in
            if call.method == "line", let l = call.arguments as? String { NSLog("dapple ingest: %@", l) }
            result(nil)
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
        case "toJpeg":
            // any ImageIO image (HEIC, PNG, TIFF, wide-gamut JPEG) -> sRGB JPEG; used for files that did not come through Photos (Files app, Documents)
            guard let src = a["src"] as? String, let dest = a["dest"] as? String else { return result(FlutterError(code: "args", message: "src/dest", details: nil)) }
            io.async {
                let ok = Self.convertToJpeg(src: src, dst: dest)
                DispatchQueue.main.async { result(ok ? dest : FlutterError(code: "convert", message: "cannot convert \((src as NSString).lastPathComponent)", details: nil)) }
            }
        case "openApp":
            result(false)  // Android package names do not exist here
        default:
            result(FlutterMethodNotImplemented)
        }
    }

    /// JPEG and RAW resources are read as they are; anything else (HEIC ...) is converted to JPEG by copyToFile.
    private static func passthrough(_ r: PHAssetResource) -> Bool { isRaw(r) || UTType(r.uniformTypeIdentifier)?.conforms(to: .jpeg) == true }

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
        var out: [[String: Any]] = []
        res.enumerateObjects { asset, _, _ in
            let rs = PHAssetResource.assetResources(for: asset)
            guard let (idx, r) = pickResource(rs) else { return }
            let ms = Int((asset.creationDate ?? Date()).timeIntervalSince1970 * 1000)
            out.append([
                "id": "ph:\(asset.localIdentifier):\(idx)",
                "name": passthrough(r) ? r.originalFilename : (r.originalFilename as NSString).deletingPathExtension + ".jpg",  // HEIC etc. arrive converted
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

    /// Writes the resource to `dest` (the app ignores the returned path): JPEG and RAW unchanged, anything else (HEIC ...) converted to JPEG.
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
        let raw = isRaw(r), jpeg = UTType(r.uniformTypeIdentifier)?.conforms(to: .jpeg) == true
        PHAssetResourceManager.default().writeData(for: r, toFile: URL(fileURLWithPath: tmp), options: o) { err in
            if let err = err { return done(FlutterError(code: "write", message: err.localizedDescription, details: nil)) }
            if place(tmp: tmp, dest: dest, raw: raw, jpeg: jpeg) { done(dest) }
            else { done(FlutterError(code: "convert", message: "cannot convert \(r.originalFilename) to JPEG", details: nil)) }
        }
    }

    /// True when the file's pixels can go to the engine as they are: no profile or an sRGB one. The engine reads every JPEG / PNG as sRGB,
    /// so a Display P3 (or Adobe RGB ...) source has to be converted, not just copied.
    static func isSRGBLike(_ url: URL) -> Bool {
        guard let s = CGImageSourceCreateWithURL(url as CFURL, nil),
              let p = CGImageSourceCopyPropertiesAtIndex(s, 0, nil) as? [CFString: Any] else { return true }
        if (p[kCGImagePropertyDepth] as? Int ?? 8) > 8 { return false }  // 16-bit sources are reduced to 8 bit by the conversion, not by the engine
        guard let name = p[kCGImagePropertyProfileName] as? String else { return true }
        return name.lowercased().contains("srgb")
    }

    /// HEIC / HEIF / PNG / TIFF / wide-gamut JPEG -> 8-bit sRGB JPEG (quality 0.95) with ImageIO: the decoder honours every profile and
    /// the HEIC container, the engine needs no HEIF library. Capture date, camera, GPS and the EXIF orientation tag are copied (the
    /// pixels stay in sensor orientation, the engine applies the tag like for any JPEG). Lost: colours outside sRGB (clipped), bit depth
    /// beyond 8, HDR gain maps (the SDR base image is used), alpha (flattened).
    static func convertToJpeg(src: String, dst: String) -> Bool {
        let url = URL(fileURLWithPath: src)
        guard let s = CGImageSourceCreateWithURL(url as CFURL, nil),
              let img = CGImageSourceCreateImageAtIndex(s, 0, [kCGImageSourceShouldCacheImmediately: false] as CFDictionary),
              let srgb = CGColorSpace(name: CGColorSpace.sRGB),
              let ctx = CGContext(data: nil, width: img.width, height: img.height, bitsPerComponent: 8, bytesPerRow: 0, space: srgb,
                                  bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue),
              let d = CGImageDestinationCreateWithURL(URL(fileURLWithPath: dst) as CFURL, UTType.jpeg.identifier as CFString, 1, nil) else { return false }
        ctx.setFillColor(CGColor(red: 1, green: 1, blue: 1, alpha: 1))
        ctx.fill(CGRect(x: 0, y: 0, width: img.width, height: img.height))
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: img.width, height: img.height))
        guard let out = ctx.makeImage() else { return false }
        var props = (CGImageSourceCopyPropertiesAtIndex(s, 0, nil) as? [CFString: Any]) ?? [:]
        for k in [kCGImagePropertyProfileName, kCGImagePropertyPixelWidth, kCGImagePropertyPixelHeight, kCGImagePropertyDepth,
                  kCGImagePropertyColorModel, kCGImagePropertyHasAlpha, kCGImagePropertyIsFloat, kCGImagePropertyIsIndexed] { props.removeValue(forKey: k) }
        props[kCGImageDestinationLossyCompressionQuality] = 0.95
        CGImageDestinationAddImage(d, out, props as CFDictionary)
        return CGImageDestinationFinalize(d)
    }

    /// Moves a freshly written original to `dest`: RAW and sRGB JPEG unchanged, everything else converted by [convertToJpeg].
    static func place(tmp: String, dest: String, raw: Bool, jpeg: Bool) -> Bool {
        try? FileManager.default.removeItem(atPath: dest)
        if raw || (jpeg && isSRGBLike(URL(fileURLWithPath: tmp))) {
            do { try FileManager.default.moveItem(atPath: tmp, toPath: dest); return true } catch { return false }
        }
        let ok = convertToJpeg(src: tmp, dst: dest)
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
            PHPhotoLibrary.shared().performChanges({
                let req = PHAssetCreationRequest.forAsset()
                let o = PHAssetResourceCreationOptions()
                o.originalFilename = name
                req.addResource(with: .photo, fileURL: URL(fileURLWithPath: path), options: o)
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
            result(statusName(PHPhotoLibrary.authorizationStatus(for: .readWrite)))
        case "requestAuthorization":
            // test runs (simulator / device scripts) drop Documents/DAPPLE_OPEN: no system prompt then, nobody can answer it
            let docs = NSSearchPathForDirectoriesInDomains(.documentDirectory, .userDomainMask, true).first ?? ""
            if FileManager.default.fileExists(atPath: docs + "/DAPPLE_OPEN") || FileManager.default.fileExists(atPath: docs + "/autotest.json") {
                return result(statusName(PHPhotoLibrary.authorizationStatus(for: .readWrite)))
            }
            PHPhotoLibrary.requestAuthorization(for: .readWrite) { s in DispatchQueue.main.async { self.registerObserverIfAllowed(); result(self.statusName(s)) } }
        case "presentLimitedLibraryPicker":
            if let vc = topViewController() { PHPhotoLibrary.shared().presentLimitedLibraryPicker(from: vc) }
            result(nil)
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
                // RAW and sRGB JPEG are copied as they are; HEIC / PNG / TIFF and wide-gamut JPEG are converted (colour managed, see convertToJpeg)
                let keep = ut?.conforms(to: .rawImage) == true || (ut?.conforms(to: .jpeg) == true && Self.isSRGBLike(url))
                let finalPath = "\(dest)/\(Int(Date().timeIntervalSince1970 * 1000) + n)_\(stem).\(keep ? ext : "jpg")"
                if keep { try? FileManager.default.copyItem(at: url, to: URL(fileURLWithPath: finalPath)) }
                else if !Self.convertToJpeg(src: url.path, dst: finalPath) {
                    NSLog("dapple pick: %@ convert failed", stem)
                    lock.lock(); out.append(["key": key, "name": stem0, "dup": false, "error": "copy"]); lock.unlock()
                    return
                }
                let size = ((try? FileManager.default.attributesOfItem(atPath: finalPath))?[.size] as? NSNumber)?.intValue ?? 0
                if size == 0 {
                    lock.lock(); out.append(["key": key, "name": stem0, "dup": false, "error": "copy"]); lock.unlock()
                    return
                }
                lock.lock(); out.append(["key": key, "name": "\(stem).\(keep ? ext : "jpg")", "dup": false, "path": finalPath, "size": size]); lock.unlock()
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
