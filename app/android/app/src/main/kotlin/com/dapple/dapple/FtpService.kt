package com.dapple.dapple

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import java.io.File
import java.net.Inet4Address
import java.net.NetworkInterface

/** Events from the FTP service to whoever listens (the activity's Flutter engine, when alive). */
object FtpBus {
    interface Sink { fun send(event: Map<String, Any?>) }

    @Volatile var sink: Sink? = null
    @Volatile var running = false
    @Volatile var clients = 0
    @Volatile var port = 0
    @Volatile var lastError: String? = null

    fun emit(event: Map<String, Any?>) {
        val s = sink ?: return
        Handler(Looper.getMainLooper()).post { s.send(event) }
    }

    /** Non-loopback IPv4 addresses with their interface names (wlan0, ap0, swlan0, ...). */
    fun addresses(): List<Map<String, String>> {
        val out = ArrayList<Map<String, String>>()
        try {
            for (ni in NetworkInterface.getNetworkInterfaces()) {
                if (!ni.isUp || ni.isLoopback) continue
                for (a in ni.inetAddresses) {
                    if (a is Inet4Address && !a.isLoopbackAddress) out.add(mapOf("iface" to ni.name, "ip" to (a.hostAddress ?: "")))
                }
            }
        } catch (_: Exception) {}
        return out
    }
}

/**
 * Foreground service hosting [FtpServer] so a camera can upload while Dapple is in the background.
 * Type dataSync (a transfer of the user's photos); Android 15 limits dataSync to ~6 h per start, after which
 * the service stops itself and the app offers to start it again.
 */
class FtpService : Service() {
    private var server: FtpServer? = null
    private var wake: PowerManager.WakeLock? = null
    private var wifi: WifiManager.WifiLock? = null
    private var uploadingName: String? = null
    private var received = 0

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        val port = intent?.getIntExtra("port", 2121) ?: 2121
        val user = intent?.getStringExtra("user") ?: "dapple"
        val pass = intent?.getStringExtra("pass") ?: ""
        val root = File(intent?.getStringExtra("root") ?: File(filesDir, "ftp_inbox").path)
        if (pass.isEmpty()) {
            FtpBus.lastError = "password is empty"
            stopSelf()
            return START_NOT_STICKY
        }
        startForegroundCompat(notification(getString(R.string.ftp_waiting, port)))
        server?.stop()
        try {
            val s = FtpServer(root, port, user, pass, PASSIVE_FIRST..PASSIVE_LAST, object : FtpServer.Listener {
                override fun onClients(count: Int) {
                    FtpBus.clients = count
                    FtpBus.emit(mapOf("type" to "clients", "count" to count))
                    update()
                }
                override fun onUploadStart(name: String) {
                    uploadingName = name
                    FtpBus.emit(mapOf("type" to "start", "name" to name))
                    update()
                }
                override fun onUploadProgress(name: String, bytes: Long) {
                    FtpBus.emit(mapOf("type" to "progress", "name" to name, "bytes" to bytes))
                }
                override fun onUploadDone(file: File) {
                    uploadingName = null
                    received++
                    FtpBus.emit(mapOf("type" to "done", "name" to file.name, "path" to file.path, "size" to file.length()))
                    update()
                }
                override fun onUploadFailed(name: String) {
                    uploadingName = null
                    FtpBus.emit(mapOf("type" to "failed", "name" to name))
                    update()
                }
                override fun onLog(line: String) { android.util.Log.i("DappleFtp", line) }
            })
            s.start()
            server = s
            FtpBus.port = port
            FtpBus.running = true
            FtpBus.lastError = null
            acquireLocks()
            FtpBus.emit(mapOf("type" to "state", "running" to true))
        } catch (e: Exception) {
            FtpBus.lastError = e.message ?: e.toString()
            FtpBus.running = false
            FtpBus.emit(mapOf("type" to "state", "running" to false, "error" to FtpBus.lastError))
            stopSelf()
            return START_NOT_STICKY
        }
        return START_REDELIVER_INTENT
    }

    override fun onDestroy() {
        server?.stop()
        server = null
        releaseLocks()
        FtpBus.running = false
        FtpBus.clients = 0
        FtpBus.emit(mapOf("type" to "state", "running" to false))
        super.onDestroy()
    }

    override fun onTimeout(startId: Int, fgsType: Int) {
        // Android 15: dataSync time budget used up.
        FtpBus.lastError = "timeout"
        stopSelf()
    }

    private fun acquireLocks() {
        try {
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            wake = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "dapple:ftp").apply { setReferenceCounted(false); acquire() }
            val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            @Suppress("DEPRECATION")
            wifi = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "dapple:ftp").apply { setReferenceCounted(false); acquire() }
        } catch (_: Exception) {}
    }

    private fun releaseLocks() {
        try { wake?.takeIf { it.isHeld }?.release() } catch (_: Exception) {}
        try { wifi?.takeIf { it.isHeld }?.release() } catch (_: Exception) {}
        wake = null
        wifi = null
    }

    private fun update() {
        val text = when {
            uploadingName != null -> getString(R.string.ftp_receiving, uploadingName)
            received > 0 -> getString(R.string.ftp_received, received)
            else -> getString(R.string.ftp_waiting, FtpBus.port)
        }
        (getSystemService(NOTIFICATION_SERVICE) as NotificationManager).notify(NOTIF_ID, notification(text))
    }

    private fun notification(text: String): Notification {
        val nm = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= 26 && nm.getNotificationChannel(CHANNEL) == null) {
            nm.createNotificationChannel(NotificationChannel(CHANNEL, getString(R.string.ftp_channel), NotificationManager.IMPORTANCE_LOW))
        }
        val open = PendingIntent.getActivity(
            this, 0, packageManager.getLaunchIntentForPackage(packageName), PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        val stop = PendingIntent.getService(
            this, 1, Intent(this, FtpService::class.java).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE
        )
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(applicationInfo.icon)
            .setContentTitle(getString(R.string.ftp_title))
            .setContentText(text)
            .setContentIntent(open)
            .setOngoing(true)
            .addAction(Notification.Action.Builder(null, getString(R.string.ftp_stop), stop).build())
            .build()
    }

    private fun startForegroundCompat(n: Notification) {
        if (Build.VERSION.SDK_INT >= 29) startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        else startForeground(NOTIF_ID, n)
    }

    companion object {
        const val ACTION_STOP = "com.dapple.dapple.FTP_STOP"
        const val CHANNEL = "dapple_ftp"
        const val NOTIF_ID = 2121
        // Fixed passive range (forwardable / firewall friendly).
        const val PASSIVE_FIRST = 2122
        const val PASSIVE_LAST = 2150
    }
}
