// Live Photo export (the iOS counterpart of the Android Motion Photo): a Motion Photo JPEG made by the engine (JPEG still + MP4 clip appended, XMP
// GCamera:MicroVideoOffset / Item:Length) is split again into the two resources Photos wants and saved as ONE asset through PHAssetCreationRequest:
//   - the still (JPEG) carries the Apple maker-note content identifier (ImageIO kCGImagePropertyMakerAppleDictionary key "17"),
//   - the clip (MOV, samples copied without re-encoding) carries `com.apple.quicktime.content.identifier` with the same id and a timed-metadata
//     track `com.apple.quicktime.still-image-time` that marks the frame the still shows.
// Used by DappleEnginePlugin.saveToGallery for files named `*_MP.jpg` (the same Dart call the Android side uses, MediaBridge.saveToGallery).
import AVFoundation
import Foundation
import ImageIO
import UniformTypeIdentifiers

enum LivePhoto {
    struct Parts {
        let stillURL: URL
        let movieURL: URL
        let contentId: String
    }

    /// Offset of the embedded MP4 in a Motion Photo JPEG and the cover's presentation time (microseconds, -1 = not recorded); nil = not a Motion Photo.
    static func locateClip(in data: Data) -> (offset: Int, ptsUs: Int64)? {
        // the XMP packet sits in the first segments of the JPEG
        let head = data.prefix(min(data.count, 256 * 1024))
        guard let text = String(data: head, encoding: .isoLatin1) else { return nil }
        func attr(_ key: String, after: String.Index? = nil) -> Int64? {
            guard let r = text.range(of: key + "=\"", range: (after ?? text.startIndex)..<text.endIndex) else { return nil }
            let rest = text[r.upperBound...]
            guard let e = rest.firstIndex(of: "\"") else { return nil }
            return Int64(rest[rest.startIndex..<e])
        }
        var len: Int64?
        if let r = text.range(of: "Item:Semantic=\"MotionPhoto\"") {
            len = attr("Item:Length", after: r.lowerBound)
        }
        if len == nil || len == 0 { len = attr("GCamera:MicroVideoOffset") }
        guard let l = len, l > 16, Int(l) < data.count else { return nil }
        let off = data.count - Int(l)
        // an MP4 / MOV starts with a box whose type is `ftyp`
        guard off >= 0, off + 8 <= data.count, data[off + 4] == 0x66, data[off + 5] == 0x74, data[off + 6] == 0x79, data[off + 7] == 0x70 else { return nil }
        return (off, attr("GCamera:MicroVideoPresentationTimestampUs") ?? -1)
    }

    /// Writes the two resources of [path] into the temp directory; nil when [path] is not a Motion Photo or the clip cannot be paired.
    static func prepare(path: String) -> Parts? {
        guard let data = try? Data(contentsOf: URL(fileURLWithPath: path), options: .mappedIfSafe), let loc = locateClip(in: data) else { return nil }
        let id = UUID().uuidString
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("livephoto-\(id)", isDirectory: true)
        do { try FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true) } catch { return nil }
        let stillURL = tmp.appendingPathComponent("IMG.jpg"), mp4URL = tmp.appendingPathComponent("clip.mp4"), movURL = tmp.appendingPathComponent("IMG.mov")
        do {
            try data.subdata(in: loc.offset..<data.count).write(to: mp4URL)
            guard writeStill(jpeg: data.subdata(in: 0..<loc.offset), contentId: id, to: stillURL) else { throw CocoaError(.fileWriteUnknown) }
            let stillTime = loc.ptsUs >= 0 ? CMTime(value: loc.ptsUs, timescale: 1_000_000) : .zero
            guard writeMovie(mp4: mp4URL, contentId: id, stillTime: stillTime, to: movURL) else { throw CocoaError(.fileWriteUnknown) }
            try? FileManager.default.removeItem(at: mp4URL)
            return Parts(stillURL: stillURL, movieURL: movURL, contentId: id)
        } catch {
            try? FileManager.default.removeItem(at: tmp)
            return nil
        }
    }

    static func cleanup(_ p: Parts) { try? FileManager.default.removeItem(at: p.stillURL.deletingLastPathComponent()) }

    /// The still with the Apple content identifier in its maker note; everything else (EXIF, ICC profile, orientation) is carried over.
    static func writeStill(jpeg: Data, contentId: String, to url: URL) -> Bool {
        guard let src = CGImageSourceCreateWithData(jpeg as CFData, nil), let type = CGImageSourceGetType(src),
              let dst = CGImageDestinationCreateWithURL(url as CFURL, type, 1, nil) else { return false }
        var props = (CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [CFString: Any]) ?? [:]
        var apple = (props[kCGImagePropertyMakerAppleDictionary] as? [String: Any]) ?? [:]
        apple["17"] = contentId
        props[kCGImagePropertyMakerAppleDictionary] = apple
        props[kCGImageDestinationLossyCompressionQuality] = 0.97  // the still is re-encoded once (ImageIO has no lossless maker-note write); 0.97 keeps it visually identical
        CGImageDestinationAddImageFromSource(dst, src, 0, props as CFDictionary)
        return CGImageDestinationFinalize(dst)
    }

    /// MP4 -> MOV with the content identifier and the still-image-time track; the compressed samples are copied, nothing is re-encoded.
    static func writeMovie(mp4: URL, contentId: String, stillTime: CMTime, to url: URL) -> Bool {
        let asset = AVURLAsset(url: mp4)
        guard let reader = try? AVAssetReader(asset: asset), let writer = try? AVAssetWriter(outputURL: url, fileType: .mov) else { return false }
        guard let vTrack = asset.tracks(withMediaType: .video).first else { return false }
        let idItem = AVMutableMetadataItem()
        idItem.keySpace = .quickTimeMetadata
        idItem.key = AVMetadataKey.quickTimeMetadataKeyContentIdentifier as NSString
        idItem.value = contentId as NSString
        idItem.dataType = "com.apple.metadata.datatype.UTF-8"
        writer.metadata = [idItem]

        var pairs: [(AVAssetReaderTrackOutput, AVAssetWriterInput)] = []
        func addTrack(_ t: AVAssetTrack) {
            let out = AVAssetReaderTrackOutput(track: t, outputSettings: nil)
            out.alwaysCopiesSampleData = false
            guard reader.canAdd(out) else { return }
            let hint = (t.formatDescriptions as? [CMFormatDescription])?.first
            let inp = AVAssetWriterInput(mediaType: t.mediaType, outputSettings: nil, sourceFormatHint: hint)
            inp.transform = t.preferredTransform
            inp.expectsMediaDataInRealTime = false
            guard writer.canAdd(inp) else { return }
            reader.add(out); writer.add(inp); pairs.append((out, inp))
        }
        addTrack(vTrack)
        if let a = asset.tracks(withMediaType: .audio).first { addTrack(a) }

        let spec: [String: Any] = [
            kCMMetadataFormatDescriptionMetadataSpecificationKey_Identifier as String: "mdta/com.apple.quicktime.still-image-time",
            kCMMetadataFormatDescriptionMetadataSpecificationKey_DataType as String: "com.apple.metadata.datatype.int8",
        ]
        var fd: CMFormatDescription?
        CMMetadataFormatDescriptionCreateWithMetadataSpecifications(allocator: kCFAllocatorDefault, metadataType: kCMMetadataFormatType_Boxed,
                                                                    metadataSpecifications: [spec] as CFArray, formatDescriptionOut: &fd)
        let metaIn = AVAssetWriterInput(mediaType: .metadata, outputSettings: nil, sourceFormatHint: fd)
        let adaptor = AVAssetWriterInputMetadataAdaptor(assetWriterInput: metaIn)
        guard writer.canAdd(metaIn) else { return false }
        writer.add(metaIn)

        guard reader.startReading(), writer.startWriting() else { return false }
        writer.startSession(atSourceTime: .zero)
        let still = AVMutableMetadataItem()
        still.identifier = AVMetadataIdentifier("mdta/com.apple.quicktime.still-image-time")
        still.dataType = "com.apple.metadata.datatype.int8"
        still.value = NSNumber(value: 0)
        adaptor.append(AVTimedMetadataGroup(items: [still], timeRange: CMTimeRange(start: stillTime, duration: CMTime(value: 1, timescale: 600))))
        metaIn.markAsFinished()

        let group = DispatchGroup()
        for (i, pair) in pairs.enumerated() {
            group.enter()
            let q = DispatchQueue(label: "dapple.livephoto.copy\(i)")
            pair.1.requestMediaDataWhenReady(on: q) {
                while pair.1.isReadyForMoreMediaData {
                    if let s = pair.0.copyNextSampleBuffer() { if !pair.1.append(s) { pair.1.markAsFinished(); group.leave(); return } }
                    else { pair.1.markAsFinished(); group.leave(); return }
                }
            }
        }
        group.wait()
        let done = DispatchSemaphore(value: 0)
        writer.finishWriting { done.signal() }
        done.wait()
        return writer.status == .completed && reader.status != .failed
    }
}
