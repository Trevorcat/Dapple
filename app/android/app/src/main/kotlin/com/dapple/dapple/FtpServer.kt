package com.dapple.dapple

import java.io.BufferedReader
import java.io.File
import java.io.IOException
import java.io.InputStreamReader
import java.io.OutputStream
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketTimeoutException
import java.text.SimpleDateFormat
import java.util.Collections
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

/**
 * A small, upload-oriented FTP server (RFC 959 + 2428 EPSV + 3659 MLSD/SIZE/MDTM subset) for cameras.
 *
 * Written for Dapple; no third-party code. Licensed with the app (the repository's own licence).
 *
 * Scope: one user with a password, rooted in [root] (every path is canonicalised and must stay inside it),
 * passive mode on a fixed port range (so it can be forwarded / firewalled), active mode only back to the
 * client's own address. Uploads are written to a hidden ".name.part" file and renamed to their final name only when
 * the transfer completes, so the app never sees a half-written photo. Downloads, deletes and removals are refused:
 * a camera only uploads.
 */
class FtpServer(
    private val root: File,
    private val port: Int,
    private val user: String,
    private val pass: String,
    private val passivePorts: IntRange,
    private val listener: Listener,
) {
    interface Listener {
        fun onClients(count: Int) {}
        fun onUploadStart(name: String) {}
        fun onUploadProgress(name: String, bytes: Long) {}
        fun onUploadDone(file: File) {}
        fun onUploadFailed(name: String) {}
        fun onLog(line: String) {}
    }

    private var server: ServerSocket? = null
    private val running = AtomicBoolean(false)
    private val pool = Executors.newCachedThreadPool()
    private val clients = Collections.synchronizedSet(HashSet<Socket>())
    private val clientCount = AtomicInteger(0)

    val boundPort: Int get() = server?.localPort ?: -1

    @Throws(IOException::class)
    fun start() {
        root.mkdirs()
        val s = ServerSocket()
        s.reuseAddress = true
        s.bind(InetSocketAddress(port))
        server = s
        running.set(true)
        pool.execute {
            while (running.get()) {
                try {
                    val c = s.accept()
                    if (clientCount.get() >= MAX_CLIENTS) {
                        try { c.getOutputStream().write("421 Too many connections\r\n".toByteArray()) } catch (_: Exception) {}
                        c.close()
                        continue
                    }
                    clients.add(c)
                    listener.onClients(clientCount.incrementAndGet())
                    pool.execute {
                        try { Session(c).run() } catch (_: Exception) {} finally {
                            clients.remove(c)
                            try { c.close() } catch (_: Exception) {}
                            listener.onClients(clientCount.decrementAndGet())
                        }
                    }
                } catch (e: IOException) {
                    if (running.get()) listener.onLog("accept: ${e.message}")
                }
            }
        }
    }

    fun stop() {
        running.set(false)
        try { server?.close() } catch (_: Exception) {}
        synchronized(clients) { clients.forEach { try { it.close() } catch (_: Exception) {} } }
        pool.shutdownNow()
    }

    private inner class Session(private val ctl: Socket) {
        private val input = BufferedReader(InputStreamReader(ctl.getInputStream(), Charsets.UTF_8))
        private val out: OutputStream = ctl.getOutputStream()
        private var userOk = false
        private var authed = false
        private var cwd = "/"
        private var pasv: ServerSocket? = null
        private var activeAddr: InetSocketAddress? = null
        private var renameFrom: File? = null
        private var failedLogins = 0

        private fun reply(line: String) {
            out.write((line + "\r\n").toByteArray(Charsets.UTF_8))
            out.flush()
        }

        private fun multi(code: Int, head: String, lines: List<String>, tail: String) {
            val sb = StringBuilder("$code-$head\r\n")
            for (l in lines) sb.append(' ').append(l).append("\r\n")
            sb.append("$code $tail\r\n")
            out.write(sb.toString().toByteArray(Charsets.UTF_8))
            out.flush()
        }

        fun run() {
            ctl.soTimeout = CONTROL_TIMEOUT_MS
            reply("220 Dapple FTP ready")
            while (true) {
                val line = try { input.readLine() } catch (_: SocketTimeoutException) { reply("421 Timeout"); return } ?: return
                if (line.isBlank()) continue
                val sp = line.indexOf(' ')
                val cmd = (if (sp < 0) line else line.substring(0, sp)).uppercase(Locale.ROOT)
                val arg = if (sp < 0) "" else line.substring(sp + 1).trim()
                if (!dispatch(cmd, arg)) return
            }
        }

        private fun dispatch(cmd: String, arg: String): Boolean {
            when (cmd) {
                "USER" -> {
                    userOk = arg == user
                    authed = false
                    reply("331 Password required")
                }
                "PASS" -> {
                    if (userOk && arg == pass) {
                        authed = true
                        reply("230 Logged in")
                    } else {
                        authed = false
                        failedLogins++
                        try { Thread.sleep(800L * failedLogins) } catch (_: InterruptedException) {}
                        reply("530 Login incorrect")
                        if (failedLogins >= 5) return false
                    }
                }
                "QUIT" -> { reply("221 Bye"); return false }
                "NOOP" -> reply("200 OK")
                "SYST" -> reply("215 UNIX Type: L8")
                "FEAT" -> multi(211, "Features:", listOf("UTF8", "EPSV", "MLST type*;size*;modify*;", "SIZE", "MDTM"), "End")
                "OPTS" -> reply(if (arg.uppercase(Locale.ROOT).startsWith("UTF8")) "200 UTF8 on" else "501 Not supported")
                "HELP" -> reply("214 Dapple camera upload server")
                else -> {
                    if (!authed) {
                        reply("530 Please log in")
                        return true
                    }
                    authedCommand(cmd, arg)
                }
            }
            return true
        }

        private fun authedCommand(cmd: String, arg: String) {
            when (cmd) {
                "PWD", "XPWD" -> reply("257 \"$cwd\" is the current directory")
                "CWD", "XCWD" -> {
                    val f = resolve(arg)
                    if (f != null && f.isDirectory) { cwd = virtual(f); reply("250 OK") } else reply("550 No such directory")
                }
                "CDUP", "XCUP" -> {
                    val f = resolve("..")
                    if (f != null && f.isDirectory) { cwd = virtual(f); reply("250 OK") } else reply("550 No such directory")
                }
                "TYPE" -> reply("200 Type set")
                "MODE" -> reply(if (arg.uppercase(Locale.ROOT) == "S") "200 Stream" else "504 Only stream")
                "STRU" -> reply(if (arg.uppercase(Locale.ROOT) == "F") "200 File" else "504 Only file")
                "ALLO" -> reply("200 OK")
                "REST" -> reply(if (arg == "0") "350 Restarting at 0" else "501 Resume is not supported")
                "PASV" -> pasvCmd(false)
                "EPSV" -> pasvCmd(true)
                "PORT" -> portCmd(arg)
                "EPRT" -> eprtCmd(arg)
                "LIST", "NLST", "MLSD" -> listCmd(cmd, arg)
                "STOR", "APPE", "STOU" -> storCmd(arg)
                "SIZE" -> {
                    val f = resolve(arg)
                    if (f != null && f.isFile) reply("213 ${f.length()}") else reply("550 No such file")
                }
                "MDTM" -> {
                    val f = resolve(arg)
                    if (f != null && f.exists()) reply("213 ${mdtm(f.lastModified())}") else reply("550 No such file")
                }
                "MKD", "XMKD" -> {
                    val f = resolve(arg)
                    if (f == null) reply("550 Invalid path")
                    else if (f.exists()) reply("550 Exists")
                    else if (f.mkdirs()) reply("257 \"${virtual(f)}\" created") else reply("550 Cannot create")
                }
                "RNFR" -> {
                    val f = resolve(arg)
                    if (f != null && f.exists() && !f.name.startsWith(".")) { renameFrom = f; reply("350 Ready for RNTO") } else reply("550 No such file")
                }
                "RNTO" -> {
                    val from = renameFrom
                    val to = resolve(arg)
                    renameFrom = null
                    if (from != null && to != null && !to.exists() && from.renameTo(to)) reply("250 Renamed") else reply("550 Rename failed")
                }
                "ABOR" -> reply("226 Nothing to abort")
                "RETR", "DELE", "RMD", "XRMD" -> reply("550 Not permitted")
                else -> reply("502 Command not implemented")
            }
        }

        // ---- paths ----
        private fun resolve(arg: String): File? {
            var a = arg
            if (a.startsWith("-")) a = a.substringAfter(' ', "") // "LIST -la"
            val vp = if (a.startsWith("/")) a else (if (cwd == "/") "/$a" else "$cwd/$a")
            val parts = ArrayList<String>()
            for (p in vp.split('/')) {
                when (p) {
                    "", "." -> {}
                    ".." -> if (parts.isNotEmpty()) parts.removeAt(parts.size - 1)
                    else -> parts.add(p)
                }
            }
            val f = File(root, parts.joinToString("/"))
            return try {
                val rc = root.canonicalPath
                val fc = f.canonicalPath
                if (fc == rc || fc.startsWith(rc + File.separator)) f else null
            } catch (_: IOException) { null }
        }

        private fun virtual(f: File): String {
            val rc = root.canonicalPath
            val fc = f.canonicalPath
            val rel = fc.removePrefix(rc).replace(File.separatorChar, '/')
            return if (rel.isEmpty()) "/" else rel
        }

        // ---- data connections ----
        private fun closeData() {
            try { pasv?.close() } catch (_: Exception) {}
            pasv = null
            activeAddr = null
        }

        private fun pasvCmd(extended: Boolean) {
            closeData()
            var s: ServerSocket? = null
            for (p in passivePorts.shuffled()) {
                try {
                    s = ServerSocket()
                    s.reuseAddress = true
                    s.bind(InetSocketAddress(p))
                    break
                } catch (_: IOException) { s = null }
            }
            if (s == null) { reply("425 No passive port available"); return }
            s.soTimeout = DATA_ACCEPT_TIMEOUT_MS
            pasv = s
            if (extended) {
                reply("229 Entering Extended Passive Mode (|||${s.localPort}|)")
            } else {
                val ip = (ctl.localAddress as? java.net.Inet4Address)?.hostAddress ?: "127.0.0.1"
                val p = s.localPort
                reply("227 Entering Passive Mode (${ip.replace('.', ',')},${p shr 8},${p and 255})")
            }
        }

        private fun portCmd(arg: String) {
            val n = arg.split(',').mapNotNull { it.trim().toIntOrNull() }
            if (n.size != 6) { reply("501 Bad PORT"); return }
            setActive("${n[0]}.${n[1]}.${n[2]}.${n[3]}", n[4] * 256 + n[5])
        }

        private fun eprtCmd(arg: String) {
            val d = arg.firstOrNull() ?: run { reply("501 Bad EPRT"); return }
            val f = arg.split(d)
            if (f.size < 4) { reply("501 Bad EPRT"); return }
            setActive(f[2], f[3].toIntOrNull() ?: -1)
        }

        private fun setActive(host: String, p: Int) {
            closeData()
            // Only the client's own address: FTP bounce protection.
            if (p !in 1024..65535 || host != ctl.inetAddress.hostAddress) { reply("501 Refused (address must match the client)"); return }
            activeAddr = InetSocketAddress(host, p)
            reply("200 PORT ok")
        }

        private fun openData(): Socket? {
            val s = pasv
            val a = activeAddr
            return try {
                when {
                    s != null -> {
                        val d = s.accept()
                        if (d.inetAddress != ctl.inetAddress) { d.close(); null } else d
                    }
                    a != null -> Socket().also { it.connect(a, 10_000) }
                    else -> null
                }
            } catch (_: Exception) { null } finally {
                try { pasv?.close() } catch (_: Exception) {}
                pasv = null
                activeAddr = null
            }
        }

        // ---- LIST ----
        private fun listCmd(cmd: String, arg: String) {
            val target = resolve(if (arg.isBlank() || arg.startsWith("-")) "." else arg)
            if (target == null || !target.exists()) { reply("550 No such file or directory"); return }
            reply("150 Opening data connection")
            val d = openData()
            if (d == null) { reply("425 Cannot open data connection"); return }
            try {
                d.use {
                    val files = (if (target.isDirectory) target.listFiles()?.toList() ?: emptyList() else listOf(target))
                        .filter { !it.name.startsWith(".") }.sortedBy { it.name }
                    val sb = StringBuilder()
                    for (f in files) {
                        when (cmd) {
                            "NLST" -> sb.append(f.name).append("\r\n")
                            "MLSD" -> sb.append("type=${if (f.isDirectory) "dir" else "file"};size=${f.length()};modify=${mdtm(f.lastModified())}; ${f.name}\r\n")
                            else -> {
                                val df = SimpleDateFormat("MMM dd HH:mm", Locale.US).format(Date(f.lastModified()))
                                sb.append(String.format(Locale.US, "%s 1 ftp ftp %12d %s %s\r\n", if (f.isDirectory) "drwxr-xr-x" else "-rw-r--r--", f.length(), df, f.name))
                            }
                        }
                    }
                    it.getOutputStream().write(sb.toString().toByteArray(Charsets.UTF_8))
                }
                reply("226 Transfer complete")
            } catch (_: Exception) { reply("426 Transfer aborted") }
        }

        // ---- STOR ----
        private fun storCmd(arg: String) {
            val f = resolve(arg)
            if (f == null || arg.isBlank() || f.isDirectory || f.name.startsWith(".")) { reply("553 Invalid file name"); return }
            f.parentFile?.mkdirs()
            val part = File(f.parentFile, ".${f.name}.part")
            reply("150 Ready to receive")
            val d = openData()
            if (d == null) { reply("425 Cannot open data connection"); return }
            val name = f.name
            listener.onUploadStart(name)
            var total = 0L
            var ok = false
            try {
                d.soTimeout = DATA_IDLE_TIMEOUT_MS
                d.getInputStream().use { ins ->
                    part.outputStream().use { fo ->
                        val buf = ByteArray(256 * 1024)
                        var lastReport = 0L
                        while (true) {
                            val n = ins.read(buf)
                            if (n < 0) break
                            fo.write(buf, 0, n)
                            total += n
                            if (total - lastReport >= 512 * 1024) { lastReport = total; listener.onUploadProgress(name, total) }
                        }
                        fo.flush()
                        try { fo.fd.sync() } catch (_: Exception) {}
                    }
                }
                ok = true
            } catch (e: Exception) {
                listener.onLog("upload $name aborted after $total bytes: ${e.message}")
            } finally {
                try { d.close() } catch (_: Exception) {}
            }
            if (!ok || total == 0L) {
                part.delete()
                listener.onUploadFailed(name)
                reply(if (ok) "550 Empty upload" else "426 Transfer aborted")
                return
            }
            val dest = uniqueName(f)
            if (part.renameTo(dest)) {
                reply("226 Transfer complete")
                listener.onUploadDone(dest)
            } else {
                part.delete()
                listener.onUploadFailed(name)
                reply("550 Cannot store file")
            }
        }

        private fun uniqueName(f: File): File {
            if (!f.exists()) return f
            val base = f.nameWithoutExtension
            val ext = f.extension
            var i = 1
            while (true) {
                val c = File(f.parentFile, if (ext.isEmpty()) "${base}_$i" else "${base}_$i.$ext")
                if (!c.exists()) return c
                i++
            }
        }
    }

    private fun mdtm(ms: Long): String {
        val f = SimpleDateFormat("yyyyMMddHHmmss", Locale.US)
        f.timeZone = TimeZone.getTimeZone("UTC")
        return f.format(Date(ms))
    }

    companion object {
        const val MAX_CLIENTS = 6
        const val CONTROL_TIMEOUT_MS = 10 * 60 * 1000
        const val DATA_ACCEPT_TIMEOUT_MS = 30 * 1000
        const val DATA_IDLE_TIMEOUT_MS = 60 * 1000
    }
}
