// Channel `dapple/live` on iOS: the video side of the Motion / Live Photo tool (same method names and result maps as the Kotlin LiveBridge, read by
// app/lib/services/live_bridge.dart): listVideos, findVideo, info, probe, probeEmbedded, thumbs, frame, trim, capabilities, codecReport, coverHdr.
// Videos are PhotoKit assets ("ph://<localIdentifier>") or plain files; decoding and encoding go through AVFoundation (VideoToolbox underneath).
// Differences to Android, all reported to the caller rather than hidden: `trim` ignores the still's edit (the clip is the recording itself), the cover is
// always an SDR frame (coverHdr copies the SDR still), no S-Log3 / HLG source interpretation (`slogDetected` is 0).
import AVFoundation
import Flutter
import Photos
import UIKit

final class DappleLivePlugin {
    private let q = DispatchQueue(label: "dapple.live", qos: .userInitiated)

    init(registrar: FlutterPluginRegistrar) {
        FlutterMethodChannel(name: "dapple/live", binaryMessenger: registrar.messenger()).setMethodCallHandler { [weak self] call, result in
            guard let self = self else { return result(FlutterMethodNotImplemented) }
            self.q.async {
                do {
                    let v = try self.handle(call)
                    DispatchQueue.main.async { result(v) }
                } catch let e as LiveError {
                    DispatchQueue.main.async { result(FlutterError(code: e.code, message: e.message, details: nil)) }
                } catch {
                    DispatchQueue.main.async { result(FlutterError(code: "live", message: error.localizedDescription, details: nil)) }
                }
            }
        }
    }

    struct LiveError: Error { let code: String; let message: String }

    private func arg<T>(_ c: FlutterMethodCall, _ k: String) throws -> T {
        guard let v = (c.arguments as? [String: Any])?[k] as? T else { throw LiveError(code: "args", message: "missing \(k)") }
        return v
    }
    private func opt<T>(_ c: FlutterMethodCall, _ k: String) -> T? { (c.arguments as? [String: Any])?[k] as? T }

    private func handle(_ c: FlutterMethodCall) throws -> Any? {
        switch c.method {
        case "listVideos": return listVideos(limit: opt(c, "limit") ?? 50)
        case "findVideo": return findVideo(stem: try arg(c, "stem"))
        case "info": return try info(uri: try arg(c, "uri"))
        case "probe": return try probe(uri: try arg(c, "uri"))
        case "probeEmbedded": return try probeEmbedded(path: try arg(c, "path"))
        case "capabilities": return ["main10Encoder": NSNull(), "ultraHdr": false, "videoLib": false] as [String: Any]
        case "codecReport": return ""
        case "thumbs":
            let t: [NSNumber] = try arg(c, "timesMs")
            return try thumbs(uri: try arg(c, "uri"), times: t.map { $0.int64Value }, maxEdge: opt(c, "maxEdge") ?? 160, dir: try arg(c, "destDir"), prefix: opt(c, "prefix") ?? "t")
        case "frame":
            return try frame(uri: try arg(c, "uri"), timeMs: (opt(c, "timeMs") as NSNumber?)?.int64Value ?? 0, dest: try arg(c, "dest"),
                             maxEdge: opt(c, "maxEdge") ?? 0, quality: opt(c, "quality") ?? 92)
        case "coverHdr":
            let sdr: String = try arg(c, "sdr"), out: String = try arg(c, "out")
            if sdr != out { try? FileManager.default.removeItem(atPath: out); try FileManager.default.copyItem(atPath: sdr, toPath: out) }
            return ["path": out, "written": false, "reason": 1, "peak": 0, "gainMax": 0] as [String: Any]
        case "trim":
            return try trim(uri: try arg(c, "uri"), startMs: (opt(c, "startMs") as NSNumber?)?.int64Value ?? 0,
                            durationMs: (opt(c, "durationMs") as NSNumber?)?.int64Value ?? 3000, dest: try arg(c, "dest"), mute: opt(c, "mute") ?? false)
        default: throw LiveError(code: "nope", message: "unknown method")
        }
    }

    // MARK: assets

    private static func phId(_ uri: String) -> String? {
        guard uri.hasPrefix("ph://") else { return nil }
        let r = uri.dropFirst(5)
        return String(r.split(separator: "#", maxSplits: 1, omittingEmptySubsequences: false).first ?? "")
    }

    private func authorized() -> Bool {
        let s = PHPhotoLibrary.authorizationStatus(for: .readWrite)
        return s == .authorized || s == .limited
    }

    private func phAsset(_ id: String) -> PHAsset? { PHAsset.fetchAssets(withLocalIdentifiers: [id], options: nil).firstObject }

    private func resourceName(_ a: PHAsset) -> (String, Int) {
        let r = PHAssetResource.assetResources(for: a).first { $0.type == .video || $0.type == .fullSizeVideo } ?? PHAssetResource.assetResources(for: a).first
        let size = (r?.value(forKey: "fileSize") as? NSNumber)?.intValue ?? 0
        return (r?.originalFilename ?? "video.mov", size)
    }

    private func row(_ a: PHAsset) -> [String: Any] {
        let (name, size) = resourceName(a)
        return ["id": a.localIdentifier, "uri": "ph://\(a.localIdentifier)#1", "name": name, "durationMs": Int(a.duration * 1000), "size": size,
                "addedMs": Int((a.creationDate ?? Date()).timeIntervalSince1970 * 1000), "width": a.pixelWidth, "height": a.pixelHeight]
    }

    private func listVideos(limit: Int) -> [[String: Any]] {
        guard authorized() else { return [] }
        let o = PHFetchOptions()
        o.sortDescriptors = [NSSortDescriptor(key: "creationDate", ascending: false)]
        o.fetchLimit = limit
        o.predicate = NSPredicate(format: "mediaType = %d", PHAssetMediaType.video.rawValue)
        var out: [[String: Any]] = []
        PHAsset.fetchAssets(with: o).enumerateObjects { a, _, _ in out.append(self.row(a)) }
        return out
    }

    private func findVideo(stem: String) -> [String: Any]? {
        for v in listVideos(limit: 200) {
            let n = (v["name"] as? String) ?? ""
            if (n as NSString).deletingPathExtension == stem { return v }
        }
        return nil
    }

    private func loadAsset(_ uri: String) throws -> AVAsset {
        if let id = Self.phId(uri) {
            guard let a = phAsset(id) else { throw LiveError(code: "live", message: "video not found") }
            let o = PHVideoRequestOptions()
            o.version = .current
            o.deliveryMode = .highQualityFormat
            o.isNetworkAccessAllowed = true
            var got: AVAsset?
            let sem = DispatchSemaphore(value: 0)
            PHImageManager.default().requestAVAsset(forVideo: a, options: o) { av, _, _ in got = av; sem.signal() }
            sem.wait()
            guard let av = got else { throw LiveError(code: "live", message: "video could not be opened") }
            return av
        }
        let p = uri.hasPrefix("file://") ? (URL(string: uri)?.path ?? uri) : uri
        return AVURLAsset(url: URL(fileURLWithPath: p))
    }

    private func rotation(_ t: CGAffineTransform) -> Int {
        let deg = Int((atan2(t.b, t.a) * 180 / .pi).rounded())
        return ((deg % 360) + 360) % 360
    }

    private func info(uri: String) throws -> [String: Any] {
        let asset = try loadAsset(uri)
        var m: [String: Any] = ["decodable": false, "slogDetected": 0, "captureGamma": ""]
        if let t = asset.tracks(withMediaType: .video).first {
            let s = t.naturalSize
            m["width"] = Int(s.width); m["height"] = Int(s.height); m["rotation"] = rotation(t.preferredTransform)
            m["decodable"] = t.isDecodable && asset.isPlayable
            m["fps"] = Double(t.nominalFrameRate)
            if let fd = (t.formatDescriptions as? [CMFormatDescription])?.first {
                m["format"] = fourCC(CMFormatDescriptionGetMediaSubType(fd))
            }
        }
        m["durationMs"] = Int(CMTimeGetSeconds(asset.duration) * 1000)
        return m
    }

    private func fourCC(_ v: FourCharCode) -> String {
        String(bytes: [UInt8((v >> 24) & 255), UInt8((v >> 16) & 255), UInt8((v >> 8) & 255), UInt8(v & 255)], encoding: .ascii) ?? "?"
    }

    private func probe(uri: String) throws -> [String: Any] {
        var m = try info(uri: uri)
        m["how"] = (m["decodable"] as? Bool) == true ? "hardware" : "none"
        m["hardware"] = true
        return m
    }

    private func probeEmbedded(path: String) throws -> [String: Any] {
        guard let data = try? Data(contentsOf: URL(fileURLWithPath: path), options: .mappedIfSafe), let loc = LivePhoto.locateClip(in: data) else {
            return ["decodable": false]
        }
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("probe-\(UUID().uuidString).mp4")
        defer { try? FileManager.default.removeItem(at: tmp) }
        try data.subdata(in: loc.offset..<data.count).write(to: tmp)
        return try probe(uri: tmp.path)
    }

    // MARK: frames

    private func generator(_ asset: AVAsset, maxEdge: Int, exact: Bool) -> AVAssetImageGenerator {
        let g = AVAssetImageGenerator(asset: asset)
        g.appliesPreferredTrackTransform = true
        if maxEdge > 0 { g.maximumSize = CGSize(width: maxEdge, height: maxEdge) }
        let tol = exact ? CMTime.zero : CMTime(value: 100, timescale: 1000)
        g.requestedTimeToleranceBefore = tol
        g.requestedTimeToleranceAfter = tol
        return g
    }

    private func writeJpeg(_ img: CGImage, to path: String, quality: Double) throws {
        guard let d = UIImage(cgImage: img).jpegData(compressionQuality: CGFloat(quality)) else { throw LiveError(code: "live", message: "jpeg encode failed") }
        try d.write(to: URL(fileURLWithPath: path))
    }

    private func thumbs(uri: String, times: [Int64], maxEdge: Int, dir: String, prefix: String) throws -> [Any] {
        let asset = try loadAsset(uri)
        let g = generator(asset, maxEdge: maxEdge, exact: false)
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        var out: [Any] = []
        for t in times {
            let path = (dir as NSString).appendingPathComponent("\(prefix)_\(t).jpg")
            if let img = try? g.copyCGImage(at: CMTime(value: t, timescale: 1000), actualTime: nil), (try? writeJpeg(img, to: path, quality: 0.8)) != nil {
                out.append(path)
            } else { out.append(NSNull()) }
        }
        return out
    }

    private func frame(uri: String, timeMs: Int64, dest: String, maxEdge: Int, quality: Int) throws -> [String: Any] {
        let asset = try loadAsset(uri)
        if let t = asset.tracks(withMediaType: .video).first, !t.isDecodable { throw LiveError(code: "unsupported", message: "unsupported video: \(fourCC((t.formatDescriptions.first.map { CMFormatDescriptionGetMediaSubType($0 as! CMFormatDescription) }) ?? 0))") }
        let g = generator(asset, maxEdge: maxEdge, exact: true)
        let img: CGImage
        do { img = try g.copyCGImage(at: CMTime(value: timeMs, timescale: 1000), actualTime: nil) }
        catch { throw LiveError(code: "live", message: "frame decode failed: \(error.localizedDescription)") }
        try? FileManager.default.createDirectory(atPath: (dest as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
        try writeJpeg(img, to: dest, quality: Double(quality) / 100.0)
        return ["path": dest, "kind": "sdr"]
    }

    // MARK: trim

    /// [startMs, startMs + durationMs) of the video as an H.264 MP4 (re-encoded, so the cut is frame exact); [mute] drops the audio.
    private func trim(uri: String, startMs: Int64, durationMs: Int64, dest: String, mute: Bool) throws -> [String: Any] {
        let asset = try loadAsset(uri)
        guard let vt = asset.tracks(withMediaType: .video).first else { throw LiveError(code: "live", message: "no video track") }
        let total = Int64(CMTimeGetSeconds(asset.duration) * 1000)
        let start = max(0, min(startMs, max(0, total - 200)))
        let dur = max(200, min(durationMs, total - start))
        let range = CMTimeRange(start: CMTime(value: start, timescale: 1000), duration: CMTime(value: dur, timescale: 1000))
        let comp = AVMutableComposition()
        guard let cv = comp.addMutableTrack(withMediaType: .video, preferredTrackID: kCMPersistentTrackID_Invalid) else { throw LiveError(code: "live", message: "composition") }
        try cv.insertTimeRange(range, of: vt, at: .zero)
        cv.preferredTransform = vt.preferredTransform
        var audio = false
        if !mute, let at = asset.tracks(withMediaType: .audio).first, let ca = comp.addMutableTrack(withMediaType: .audio, preferredTrackID: kCMPersistentTrackID_Invalid) {
            try? ca.insertTimeRange(range, of: at, at: .zero)
            audio = true
        }
        try? FileManager.default.removeItem(atPath: dest)
        try? FileManager.default.createDirectory(atPath: (dest as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
        guard let ex = AVAssetExportSession(asset: comp, presetName: AVAssetExportPresetHighestQuality) else { throw LiveError(code: "live", message: "no export session") }
        ex.outputURL = URL(fileURLWithPath: dest)
        ex.outputFileType = .mp4
        ex.shouldOptimizeForNetworkUse = false
        let sem = DispatchSemaphore(value: 0)
        ex.exportAsynchronously { sem.signal() }
        sem.wait()
        guard ex.status == .completed else { throw LiveError(code: "live", message: "trim failed: \(ex.error?.localizedDescription ?? "unknown")") }
        let fps = Double(vt.nominalFrameRate > 0 ? vt.nominalFrameRate : 30)
        let info: [String: Any] = [
            "path": dest, "actualStartMs": Int(start), "durationMs": Int(dur), "frames": Int(Double(dur) / 1000.0 * fps), "audio": audio,
            "videoCodec": "h264", "videoProfile": "High", "bitDepth": 8, "encoder": "VideoToolbox", "decoder": "VideoToolbox", "hardwareDecode": true,
            "audioHow": audio ? "AAC" : "", "codecSummary": "AVFoundation 重新编码（H.264）",
        ]
        return info
    }
}
