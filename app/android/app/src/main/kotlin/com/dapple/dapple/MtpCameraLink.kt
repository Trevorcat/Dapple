package com.dapple.dapple

import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.mtp.MtpConstants
import android.mtp.MtpDevice
import android.mtp.MtpEvent
import android.os.CancellationSignal
import android.os.OperationCanceledException
import android.os.ParcelFileDescriptor
import java.io.File

/** The real camera: android.mtp.MtpDevice over an opened UsbDeviceConnection (platform API only, no third-party PTP code). */
class MtpCameraLink private constructor(
    private val usb: UsbManager,
    private val device: UsbDevice,
    private val mtp: MtpDevice,
    override val info: CamInfo,
) : CameraLink {
    @Volatile private var closed = false

    override fun storages(): List<CamStorage> {
        val ids = mtp.storageIds ?: return emptyList()
        return ids.map { id ->
            val s = mtp.getStorageInfo(id)
            if (s == null) { UsbDiag.log("getStorageInfo(0x${Integer.toHexString(id)}) returned null"); CamStorage(id, "", "", 0, 0) }
            else CamStorage(id, s.description ?: "", s.volumeIdentifier ?: "", s.maxCapacity, s.freeSpace)
        }
    }

    /** Whole storage in one call (parent 0 = all objects, PTP); when a camera answers that with nothing, walk the folders from the root. */
    override fun handles(storageId: Int): IntArray {
        guard()
        val all = mtp.getObjectHandles(storageId, 0, 0)
        if (all != null && all.isNotEmpty()) return all
        UsbDiag.log("getObjectHandles(all) empty/null for 0x${Integer.toHexString(storageId)}; walking folders from the root")
        val out = ArrayList<Int>()
        val stack = ArrayDeque<Int>().apply { add(-1) } // 0xFFFFFFFF = root
        var guard = 0
        while (stack.isNotEmpty() && guard++ < 5000) {
            val p = stack.removeLast()
            val hs = mtp.getObjectHandles(storageId, 0, p) ?: continue
            for (h in hs) {
                out.add(h)
                if (mtp.getObjectInfo(h)?.format == MtpConstants.FORMAT_ASSOCIATION) stack.add(h)
            }
        }
        return out.toIntArray()
    }

    override fun objectInfo(handle: Int): CamObject? {
        guard()
        val o = mtp.getObjectInfo(handle) ?: return null
        val date = if (o.dateCreated > 0) o.dateCreated else o.dateModified
        return CamObject(handle, o.storageId, o.parent, o.format, o.name ?: "", o.compressedSizeLong, date)
    }

    override fun thumbnail(handle: Int): ByteArray? {
        guard()
        return mtp.getThumbnail(handle)
    }

    override fun importTo(handle: Int, dest: File): Boolean {
        guard()
        val pfd = ParcelFileDescriptor.open(dest, ParcelFileDescriptor.MODE_READ_WRITE or ParcelFileDescriptor.MODE_CREATE or ParcelFileDescriptor.MODE_TRUNCATE)
        val ok = try { mtp.importFile(handle, pfd) } finally { try { pfd.close() } catch (_: Exception) {} }
        if (!ok && !alive()) throw CameraGoneException("importFile failed and the device is gone")
        return ok
    }

    override fun readEvent(cancel: CancelFlag): CamEvent? {
        val sig = CancellationSignal()
        cancel.hook = { sig.cancel() }
        if (cancel.cancelled) return null
        return try {
            val e: MtpEvent = mtp.readEvent(sig)
            CamEvent(e.eventCode, e.parameter1, e.parameter2, e.parameter3)
        } catch (e: OperationCanceledException) {
            null
        } catch (e: java.io.IOException) {
            if (cancel.cancelled || closed) null else if (!alive()) throw CameraGoneException("readEvent: device gone", e) else throw e
        }
    }

    override fun alive(): Boolean = !closed && usb.deviceList.containsKey(device.deviceName)

    private fun guard() { if (closed) throw CameraGoneException("link closed") }

    override fun close() {
        if (closed) return
        closed = true
        try { mtp.close() } catch (_: Exception) {}
    }

    companion object {
        /** Opens and describes the device; null (with the reason logged and returned in [fail]) when it is not usable as a camera. */
        fun open(usb: UsbManager, device: UsbDevice, fail: (String) -> Unit): MtpCameraLink? {
            val conn = try { usb.openDevice(device) } catch (e: Exception) { UsbDiag.log("openDevice threw ${e.message}"); null }
            if (conn == null) { fail("busy"); return null }
            val mtp = MtpDevice(device)
            if (!mtp.open(conn)) {
                UsbDiag.log("MtpDevice.open returned false (device class/interfaces above; the camera may be in a mode that is not still-image transfer)")
                conn.close()
                fail("open_failed")
                return null
            }
            val di = mtp.deviceInfo
            if (di == null) { UsbDiag.log("getDeviceInfo returned null"); mtp.close(); fail("open_failed"); return null }
            val info = CamInfo(
                di.manufacturer ?: "", di.model ?: "", di.version ?: "", di.serialNumber ?: "",
                di.operationsSupported?.toList() ?: emptyList(), di.eventsSupported?.toList() ?: emptyList(),
            )
            return MtpCameraLink(usb, device, mtp, info)
        }
    }
}
