package com.dapple.dapple

import java.io.BufferedOutputStream
import java.io.Closeable
import java.io.File
import java.io.FileOutputStream
import java.io.OutputStream
import java.util.zip.CRC32
import java.util.zip.Deflater
import java.util.zip.DeflaterOutputStream

/** The colour space a PNG16 is tagged with: cICP code points (primaries, transfer), or Adobe RGB as cHRM + gAMA (cICP has no code for it). */
class PngSpace(val label: String, val cicp: IntArray?, val adobe: Boolean = false)

/**
 * Streaming writer for 16-bit RGB PNG: rows go through a [Deflater] into IDAT chunks as they arrive, so the output is never in memory.
 * Chunks before IDAT: cICP (or cHRM + gAMA), eXIf (a TIFF block with orientation, capture date, camera, GPS). The engine reads exactly these.
 */
class Png16Writer(file: File, private val w: Int, private val h: Int, space: PngSpace, exifTiff: ByteArray?) : Closeable {
    private val out = BufferedOutputStream(FileOutputStream(file), 1 shl 16)
    private val idat = IdatStream(out)
    private val z = DeflaterOutputStream(idat, Deflater(4), 1 shl 16)

    init {
        out.write(byteArrayOf(0x89.toByte(), 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A))
        chunk(out, "IHDR", be32(w) + be32(h) + byteArrayOf(16, 2, 0, 0, 0))
        space.cicp?.let { chunk(out, "cICP", byteArrayOf(it[0].toByte(), it[1].toByte(), 0, 1)) }
        if (space.adobe) {
            chunk(out, "gAMA", be32(45455))
            // white D65, red, green, blue (x, y) * 100000
            chunk(out, "cHRM", intArrayOf(31270, 32900, 64000, 33000, 21000, 71000, 15000, 6000).fold(ByteArray(0)) { a, v -> a + be32(v) })
        }
        exifTiff?.let { chunk(out, "eXIf", it) }
    }

    /** [scanlines]: `rows` lines of (filter byte 0 + w * 6 bytes big-endian RGB16). */
    fun write(scanlines: ByteArray, length: Int) = z.write(scanlines, 0, length)

    override fun close() {
        z.finish()
        idat.flushChunk()
        chunk(out, "IEND", ByteArray(0))
        out.close()
    }

    private class IdatStream(val sink: OutputStream) : OutputStream() {
        private val buf = ByteArray(256 * 1024)
        private var n = 0
        override fun write(b: Int) {
            if (n == buf.size) flushChunk()
            buf[n++] = b.toByte()
        }
        override fun write(b: ByteArray, off: Int, len: Int) {
            var o = off
            var left = len
            while (left > 0) {
                if (n == buf.size) flushChunk()
                val k = minOf(left, buf.size - n)
                System.arraycopy(b, o, buf, n, k)
                n += k; o += k; left -= k
            }
        }
        fun flushChunk() {
            if (n > 0) chunk(sink, "IDAT", buf.copyOf(n))
            n = 0
        }
    }

    companion object {
        fun be32(v: Int) = byteArrayOf((v ushr 24).toByte(), (v ushr 16).toByte(), (v ushr 8).toByte(), v.toByte())

        fun chunk(o: OutputStream, type: String, data: ByteArray) {
            val t = type.toByteArray(Charsets.US_ASCII)
            o.write(be32(data.size))
            o.write(t)
            o.write(data)
            val crc = CRC32()
            crc.update(t)
            crc.update(data)
            o.write(be32(crc.value.toInt()))
        }

        /** The TIFF block of the first APP1 "Exif" segment of a JPEG, or null. */
        fun exifOfJpeg(b: ByteArray): ByteArray? {
            var pos = 2
            while (pos + 4 < b.size && b[pos] == 0xFF.toByte()) {
                val marker = b[pos + 1].toInt() and 0xFF
                if (marker == 0xDA || marker == 0xD9) break
                val len = ((b[pos + 2].toInt() and 0xFF) shl 8) or (b[pos + 3].toInt() and 0xFF)
                if (marker == 0xE1 && len > 16 && pos + 2 + len <= b.size &&
                    String(b, pos + 4, 4, Charsets.US_ASCII) == "Exif" && b[pos + 8].toInt() == 0 && b[pos + 9].toInt() == 0) {
                    return b.copyOfRange(pos + 10, pos + 2 + len)
                }
                pos += 2 + len
            }
            return null
        }
    }
}
