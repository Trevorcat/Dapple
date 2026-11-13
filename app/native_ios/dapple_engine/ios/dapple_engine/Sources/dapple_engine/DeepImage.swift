import CoreGraphics
import Compression
import Foundation
import ImageIO
import UniformTypeIdentifiers

/// HEIC / HEIF / TIFF / WebP ... -> 16-bit RGB PNG in the picture's own colour space (Display P3 stays Display P3, 10-bit HEIC keeps its
/// 10 bits). The engine reads the PNG and honours the space: cICP for the named ones, else the ICC profile (iCCP). Orientation, capture
/// date, camera and GPS travel in an eXIf chunk; the pixels stay in sensor orientation (the engine applies the tag, like for a JPEG).
///
/// Memory: the picture is drawn into a 16-bit context one strip of about 6 MP at a time (48 MB), the strip is packed (36 MB) and
/// deflated into IDAT chunks as it goes: peak = ImageIO's decode of the source + about 90 MB, independent of the output size. A 48 MP
/// photo costs 8 strips; the output is a streamed file, never held in memory. Lost: HDR gain maps (the SDR base image is used), alpha
/// (flattened to white), HDR transfer curves (PQ / HLG sources are drawn into Display P3).
enum DeepImage {
    private static let crcTable: [UInt32] = (0..<256).map { n -> UInt32 in
        var c = UInt32(n)
        for _ in 0..<8 { c = c & 1 != 0 ? 0xEDB88320 ^ (c >> 1) : c >> 1 }
        return c
    }

    private static func crc(_ type: [UInt8], _ data: [UInt8]) -> UInt32 {
        var c: UInt32 = 0xFFFFFFFF
        for b in type { c = crcTable[Int((c ^ UInt32(b)) & 0xFF)] ^ (c >> 8) }
        for b in data { c = crcTable[Int((c ^ UInt32(b)) & 0xFF)] ^ (c >> 8) }
        return c ^ 0xFFFFFFFF
    }

    private static func be32(_ v: UInt32) -> [UInt8] { [UInt8(v >> 24), UInt8((v >> 16) & 0xFF), UInt8((v >> 8) & 0xFF), UInt8(v & 0xFF)] }

    private static func chunk(_ type: String, _ data: [UInt8]) -> Data {
        let t = Array(type.utf8)
        return Data(be32(UInt32(data.count)) + t + data + be32(crc(t, data)))
    }

    /// Streaming raw deflate (Compression's COMPRESSION_ZLIB is RFC 1951 without the zlib header) wrapped as zlib by the caller.
    private final class Deflater {
        private var stream = compression_stream(dst_ptr: UnsafeMutablePointer<UInt8>(bitPattern: 1)!, dst_size: 0, src_ptr: UnsafePointer<UInt8>(bitPattern: 1)!, src_size: 0, state: nil)
        private var buf = [UInt8](repeating: 0, count: 1 << 20)
        private(set) var adlerA: UInt32 = 1
        private var adlerB: UInt32 = 0
        init?() { if compression_stream_init(&stream, COMPRESSION_STREAM_ENCODE, COMPRESSION_ZLIB) != COMPRESSION_STATUS_OK { return nil } }
        deinit { compression_stream_destroy(&stream) }
        var adler: UInt32 { (adlerB << 16) | adlerA }

        func run(_ input: [UInt8], finalize: Bool, _ out: ([UInt8]) -> Void) -> Bool {
            for v in input {
                adlerA = (adlerA + UInt32(v)) % 65521
                adlerB = (adlerB + adlerA) % 65521
            }
            var ok = true
            input.withUnsafeBufferPointer { ib in
                let dummy: [UInt8] = [0]
                dummy.withUnsafeBufferPointer { db in
                    stream.src_ptr = ib.count > 0 ? ib.baseAddress! : db.baseAddress!
                    stream.src_size = ib.count
                    while true {
                        let status: compression_status = buf.withUnsafeMutableBufferPointer { b in
                            stream.dst_ptr = b.baseAddress!
                            stream.dst_size = b.count
                            return compression_stream_process(&stream, finalize ? Int32(COMPRESSION_STREAM_FINALIZE.rawValue) : 0)
                        }
                        let produced = buf.count - stream.dst_size
                        if produced > 0 { out(Array(buf[0..<produced])) }
                        if status == COMPRESSION_STATUS_END { break }
                        if status == COMPRESSION_STATUS_ERROR { ok = false; break }
                        if !finalize && stream.src_size == 0 && stream.dst_size > 0 { break }
                    }
                }
            }
            return ok
        }
    }

    private static func zlibBlob(_ data: [UInt8]) -> [UInt8]? {
        guard let d = Deflater() else { return nil }
        var out: [UInt8] = [0x78, 0x9C]
        guard d.run(data, finalize: true, { out += $0 }) else { return nil }
        return out + be32(d.adler)
    }

    /// The space the pixels are drawn in, and the cICP code points when it is a named one.
    private static func targetSpace(_ img: CGImage) -> (space: CGColorSpace, cicp: (UInt8, UInt8)?, icc: Data?) {
        let srgb = CGColorSpace(name: CGColorSpace.sRGB)!
        let p3 = CGColorSpace(name: CGColorSpace.displayP3)!
        guard let cs = img.colorSpace, cs.model == .rgb else { return (srgb, (1, 13), nil) }
        let name = (cs.name as String?) ?? ""
        if name == (CGColorSpace.sRGB as String) { return (cs, (1, 13), nil) }
        if name == (CGColorSpace.displayP3 as String) { return (cs, (12, 13), nil) }
        if name.contains("PQ") || name.contains("HLG") || name.contains("xtended") || name.contains("inear") { return (p3, (12, 13), nil) }
        if let icc = cs.copyICCData() as Data?, icc.count > 128 { return (cs, nil, icc) }
        return (srgb, (1, 13), nil)
    }

    /// The EXIF TIFF block for the eXIf chunk: ImageIO writes the properties into a 1 x 1 JPEG, its APP1 payload is taken.
    private static func exifBlock(_ props: [CFString: Any]) -> [UInt8]? {
        guard let ctx = CGContext(data: nil, width: 1, height: 1, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue),
              let one = ctx.makeImage() else { return nil }
        let data = NSMutableData()
        guard let d = CGImageDestinationCreateWithData(data, UTType.jpeg.identifier as CFString, 1, nil) else { return nil }
        var p = props
        for k in [kCGImagePropertyProfileName, kCGImagePropertyPixelWidth, kCGImagePropertyPixelHeight, kCGImagePropertyDepth,
                  kCGImagePropertyColorModel, kCGImagePropertyHasAlpha, kCGImagePropertyIsFloat, kCGImagePropertyIsIndexed] { p.removeValue(forKey: k) }
        CGImageDestinationAddImage(d, one, p as CFDictionary)
        guard CGImageDestinationFinalize(d) else { return nil }
        let b = [UInt8](data as Data)
        var pos = 2
        while pos + 4 < b.count, b[pos] == 0xFF {
            let marker = b[pos + 1]
            if marker == 0xDA || marker == 0xD9 { break }
            let len = Int(b[pos + 2]) << 8 | Int(b[pos + 3])
            if marker == 0xE1, len > 16, pos + 2 + len <= b.count, Array(b[(pos + 4)..<(pos + 10)]) == [0x45, 0x78, 0x69, 0x66, 0, 0] {
                return Array(b[(pos + 10)..<(pos + 2 + len)])
            }
            pos += 2 + len
        }
        return nil
    }

    static func convert(src: String, dst: String) -> Bool {
        guard let s = CGImageSourceCreateWithURL(URL(fileURLWithPath: src) as CFURL, nil),
              let img = CGImageSourceCreateImageAtIndex(s, 0, [kCGImageSourceShouldCacheImmediately: false] as CFDictionary) else { return false }
        let w = img.width, h = img.height
        guard w > 0, h > 0 else { return false }
        let t = targetSpace(img)
        let info = CGImageAlphaInfo.noneSkipLast.rawValue | CGBitmapInfo.byteOrder16Big.rawValue
        try? FileManager.default.removeItem(atPath: dst)
        guard FileManager.default.createFile(atPath: dst, contents: nil), let fh = FileHandle(forWritingAtPath: dst) else { return false }
        defer { try? fh.close() }

        var ihdr = be32(UInt32(w)) + be32(UInt32(h))
        ihdr += [16, 2, 0, 0, 0]
        fh.write(Data([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]))
        fh.write(chunk("IHDR", ihdr))
        if let c = t.cicp { fh.write(chunk("cICP", [c.0, c.1, 0, 1])) }
        if let icc = t.icc, let z = zlibBlob([UInt8](icc)) { fh.write(chunk("iCCP", Array("icc".utf8) + [0, 0] + z)) }
        let props = (CGImageSourceCopyPropertiesAtIndex(s, 0, nil) as? [CFString: Any]) ?? [:]
        if let ex = exifBlock(props) { fh.write(chunk("eXIf", ex)) }

        guard let df = Deflater() else { return false }
        var first = true
        let emit: ([UInt8]) -> Void = { part in
            fh.write(chunk("IDAT", first ? [0x78, 0x9C] + part : part))
            first = false
        }
        let white = CGColor(colorSpace: t.space, components: [1, 1, 1, 1]) ?? CGColor(red: 1, green: 1, blue: 1, alpha: 1)
        let stripRows = max(1, min(h, 6_000_000 / w))
        let rowBytes = 1 + w * 6
        var y0 = 0
        while y0 < h {
            let rows = min(stripRows, h - y0)
            guard let ctx = CGContext(data: nil, width: w, height: rows, bitsPerComponent: 16, bytesPerRow: 0, space: t.space, bitmapInfo: info) else { return false }
            ctx.interpolationQuality = .none
            ctx.setFillColor(white)
            ctx.fill(CGRect(x: 0, y: 0, width: w, height: rows))
            ctx.draw(img, in: CGRect(x: 0, y: -(h - y0 - rows), width: w, height: h))
            guard let base = ctx.data?.assumingMemoryBound(to: UInt8.self) else { return false }
            let bpr = ctx.bytesPerRow
            var raw = [UInt8](repeating: 0, count: rows * rowBytes)
            raw.withUnsafeMutableBufferPointer { rb in
                for r in 0..<rows {
                    let o = r * rowBytes
                    rb[o] = 0
                    let row = base + r * bpr
                    for x in 0..<w {
                        let d = o + 1 + 6 * x, sIdx = 8 * x
                        rb[d] = row[sIdx]; rb[d + 1] = row[sIdx + 1]; rb[d + 2] = row[sIdx + 2]
                        rb[d + 3] = row[sIdx + 3]; rb[d + 4] = row[sIdx + 4]; rb[d + 5] = row[sIdx + 5]
                    }
                }
            }
            y0 += rows
            guard df.run(raw, finalize: false, emit) else { return false }
        }
        guard df.run([], finalize: true, emit) else { return false }
        fh.write(chunk("IDAT", be32(df.adler)))  // zlib trailer
        fh.write(chunk("IEND", []))
        return true
    }
}
