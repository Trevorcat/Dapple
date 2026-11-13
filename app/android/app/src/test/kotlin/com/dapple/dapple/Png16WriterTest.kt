package com.dapple.dapple

import java.io.File
import java.util.zip.Inflater
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class Png16WriterTest {
    private fun scanlines(w: Int, h: Int, v: (Int, Int, Int) -> Int): ByteArray {
        val out = ByteArray(h * (1 + w * 6))
        var o = 0
        for (y in 0 until h) {
            out[o++] = 0
            for (x in 0 until w) for (c in 0 until 3) {
                val u = v(x, y, c)
                out[o++] = (u shr 8).toByte(); out[o++] = u.toByte()
            }
        }
        return out
    }

    @Test fun sixteenBitSamplesAndColourChunksSurviveTheRoundTrip() {
        val f = File.createTempFile("p16", ".png")
        try {
            val w = 700  // wider than one IDAT buffer of rows once deflated data is incompressible
            val h = 40
            val data = scanlines(w, h) { x, y, c -> (x * 91 + y * 977 + c * 12345) and 0xFFFF }
            Png16Writer(f, w, h, PngSpace("Display P3", intArrayOf(12, 13)), byteArrayOf(0x49, 0x49, 0x2A, 0, 8, 0, 0, 0, 0, 0)).use {
                it.write(data, data.size / 2)  // two writes: the stream is continuous
                it.write(data.copyOfRange(data.size / 2, data.size), data.size - data.size / 2)
            }
            val bytes = f.readBytes()
            val text = String(bytes, Charsets.ISO_8859_1)
            assertTrue(text.contains("cICP"))
            assertTrue(text.contains("eXIf"))
            // reassemble IDAT, inflate, compare with what was written
            val idat = java.io.ByteArrayOutputStream()
            var pos = 8
            while (pos < bytes.size) {
                val len = ((bytes[pos].toInt() and 0xFF) shl 24) or ((bytes[pos + 1].toInt() and 0xFF) shl 16) or
                    ((bytes[pos + 2].toInt() and 0xFF) shl 8) or (bytes[pos + 3].toInt() and 0xFF)
                if (String(bytes, pos + 4, 4, Charsets.US_ASCII) == "IDAT") idat.write(bytes, pos + 8, len)
                pos += 12 + len
            }
            val inf = Inflater()
            inf.setInput(idat.toByteArray())
            val raw = ByteArray(data.size)
            var got = 0
            while (got < raw.size && !inf.finished()) got += inf.inflate(raw, got, raw.size - got)
            assertEquals(data.size, got)
            assertTrue(raw.contentEquals(data))
        } finally {
            f.delete()
        }
    }

    @Test fun adobeRgbIsTaggedWithPrimariesAndGamma() {
        val f = File.createTempFile("p16a", ".png")
        try {
            Png16Writer(f, 1, 1, PngSpace("Adobe RGB", null, adobe = true), null).use { it.write(scanlines(1, 1) { _, _, _ -> 1000 }, 7) }
            val text = String(f.readBytes(), Charsets.ISO_8859_1)
            assertTrue(text.contains("gAMA") && text.contains("cHRM"))
            assertTrue(!text.contains("cICP"))
        } finally {
            f.delete()
        }
    }
}
