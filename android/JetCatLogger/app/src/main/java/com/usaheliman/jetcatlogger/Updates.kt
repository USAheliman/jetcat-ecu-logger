// JetCat ECU Logger app :: Updates.kt
//
// App and logger-firmware updates, both from the project's GitHub releases
// (github.com/USAheliman/jetcat-ecu-logger/releases). Mehul runs the "Release" action with a
// version number; it publishes, per release:
//   JetCatLogger.apk          the app
//   firmware-xiao_plus.bin    logger firmware, XIAO battery build
//   firmware-devkitc.bin      logger firmware, DevKitC test board
//   release.json              {"version":"1.0.0","app_version_code":N}
// Dev builds (the "app-latest" pre-release) are never offered.
//
// The flow follows Malcolm Messiter's RXV2App (GPL-3.0-or-later, used with his permission):
//   - App: checked when the home screen opens (at most hourly) and from Settings. A newer
//     release offers "Update"; Android asks once to allow this app to install updates,
//     then shows its installer. "Later" is remembered per release.
//   - Firmware: the phone downloads the .bin with its own internet and sends it to the
//     logger over Bluetooth (/api/bleota/begin, raw 0xA5 chunks, /status, /end, /reboot).
// GPL-3.0-or-later.

package com.usaheliman.jetcatlogger

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings
import android.view.WindowManager
import androidx.appcompat.app.AlertDialog
import androidx.core.content.FileProvider
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

class Updates(
    private val act: MainActivity,
    private val ble: LoggerBle,
    /** Bluetooth request to the logger: (method, path) -> (HTTP code, body) or null on no reply. */
    private val bleSync: (String, String) -> Pair<Int, ByteArray>?,
    /** Run JavaScript in the pages (on the UI thread). */
    private val js: (String) -> Unit,
    private val toast: (String) -> Unit,
) {
    data class Release(val tag: String, val version: String, val notes: String, val date: String,
                       val assets: Map<String, String>)

    @Volatile var busy = false            // a firmware install owns the Bluetooth link
        private set
    private var lastAutoCheck = 0L
    private var pendingApk: Pair<String, String>? = null   // url, version: waiting for install permission

    // ── GitHub ──────────────────────────────────────────────────────
    private fun httpGet(url: String, maxBytes: Int = 8 * 1024 * 1024,
                        progress: ((Int, Int) -> Unit)? = null): ByteArray {
        var u = url
        for (hop in 0 until 6) {
            val c = URL(u).openConnection() as HttpURLConnection
            c.connectTimeout = 15000
            c.readTimeout = 30000
            c.instanceFollowRedirects = false    // GitHub hands assets over to another host
            c.setRequestProperty("User-Agent", "JetCatLogger-app")
            c.setRequestProperty("Accept", if (u.contains("api.github.com")) "application/vnd.github+json" else "*/*")
            val code = c.responseCode
            if (code in 300..399) {
                u = c.getHeaderField("Location") ?: throw Exception("redirect without a location")
                c.disconnect()
                continue
            }
            if (code != 200) { c.disconnect(); throw Exception("HTTP $code from ${URL(u).host}") }
            val total = c.contentLength
            try {
                c.inputStream.use { ins ->
                    val out = ByteArrayOutputStream(if (total > 0) total else 65536)
                    val buf = ByteArray(16384)
                    while (true) {
                        val n = ins.read(buf)
                        if (n < 0) break
                        out.write(buf, 0, n)
                        if (out.size() > maxBytes) throw Exception("download too large")
                        progress?.invoke(out.size(), total)
                    }
                    return out.toByteArray()
                }
            } finally { c.disconnect() }
        }
        throw Exception("too many redirects")
    }

    /** Published releases, newest first (drafts and the dev pre-release left out). */
    private fun releases(): List<Release> {
        val arr = JSONArray(String(httpGet(BuildConfig.RELEASES_API + "?per_page=20")))
        val out = ArrayList<Release>()
        for (i in 0 until arr.length()) {
            val r = arr.getJSONObject(i)
            if (r.optBoolean("draft") || r.optBoolean("prerelease")) continue
            val assets = HashMap<String, String>()
            r.optJSONArray("assets")?.let { a ->
                for (k in 0 until a.length()) {
                    val o = a.getJSONObject(k)
                    assets[o.getString("name")] = o.getString("browser_download_url")
                }
            }
            val tag = r.getString("tag_name")
            out.add(Release(tag, tag.removePrefix("v"), r.optString("body", ""),
                r.optString("published_at", "").take(10), assets))
        }
        return out
    }

    // ── App update ──────────────────────────────────────────────────
    private fun installedCode(): Long {
        val pi = act.packageManager.getPackageInfo(act.packageName, 0)
        return if (Build.VERSION.SDK_INT >= 28) pi.longVersionCode else @Suppress("DEPRECATION") pi.versionCode.toLong()
    }
    fun versionName(): String =
        runCatching { act.packageManager.getPackageInfo(act.packageName, 0).versionName }.getOrNull() ?: "?"

    /** manual = from the Settings button: says so when there is nothing newer, and ignores "Later". */
    fun checkApp(manual: Boolean) {
        if (!manual && System.currentTimeMillis() - lastAutoCheck < 3_600_000) return
        lastAutoCheck = System.currentTimeMillis()
        Thread {
            try {
                val latest = releases().firstOrNull { it.assets.containsKey("JetCatLogger.apk") && it.assets.containsKey("release.json") }
                if (latest == null) { if (manual) act.runOnUiThread { toast("No app releases published yet.") }; return@Thread }
                val info = JSONObject(String(httpGet(latest.assets.getValue("release.json"))))
                val code = info.optLong("app_version_code", 0)
                val prefs = act.getSharedPreferences("updates", 0)
                act.runOnUiThread {
                    when {
                        code <= installedCode() ->
                            if (manual) toast("This app (${versionName()}) is up to date. Latest release: ${latest.version}.")
                        !manual && prefs.getLong("later", 0) == code -> {}
                        else -> AlertDialog.Builder(act)
                            .setTitle("App update ${latest.version}")
                            .setMessage((latest.notes.ifBlank { "A newer version of the app is available." }) +
                                "\n\nInstalled: ${versionName()}")
                            .setPositiveButton("Update") { _, _ -> installApk(latest.assets.getValue("JetCatLogger.apk"), latest.version) }
                            .setNegativeButton("Later") { _, _ -> prefs.edit().putLong("later", code).apply() }
                            .show()
                    }
                }
            } catch (e: Exception) {
                if (manual) act.runOnUiThread { toast("Could not check for updates: ${e.message}") }
            }
        }.start()
    }

    private fun installApk(url: String, version: String) {
        // One-time gate: Android must allow this app to install updates. Send the user to
        // our entry in Settings; resumePending() carries on when they come back.
        if (!act.packageManager.canRequestPackageInstalls()) {
            pendingApk = url to version
            toast("Allow JetCat Logger to install updates, then come back.")
            act.startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${act.packageName}")))
            return
        }
        toast("Downloading app $version…")
        Thread {
            try {
                val bytes = httpGet(url, maxBytes = 64 * 1024 * 1024)
                val dir = File(act.cacheDir, "updates").apply { mkdirs() }
                dir.listFiles()?.forEach { it.delete() }
                val f = File(dir, "JetCatLogger-$version.apk")
                f.writeBytes(bytes)
                val uri = FileProvider.getUriForFile(act, "${act.packageName}.fileprovider", f)
                act.runOnUiThread {
                    act.startActivity(Intent(Intent.ACTION_VIEW)
                        .setDataAndType(uri, "application/vnd.android.package-archive")
                        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK))
                }
            } catch (e: Exception) {
                act.runOnUiThread { toast("App download failed: ${e.message}") }
            }
        }.start()
    }

    /** From onResume: back from the "install unknown apps" setting. */
    fun resumePending() {
        val p = pendingApk ?: return
        if (!act.packageManager.canRequestPackageInstalls()) return
        pendingApk = null
        installApk(p.first, p.second)
    }

    // ── Firmware ────────────────────────────────────────────────────
    /** Release list for the Settings page: window.__fwReleases({releases:[...]}) or {error}. */
    fun firmwareList() {
        Thread {
            val out = JSONObject()
            try {
                val arr = JSONArray()
                for (r in releases()) {
                    val boards = JSONArray()
                    for (name in r.assets.keys)
                        if (name.startsWith("firmware-") && name.endsWith(".bin"))
                            boards.put(name.removePrefix("firmware-").removeSuffix(".bin"))
                    if (boards.length() == 0) continue
                    arr.put(JSONObject().put("tag", r.tag).put("version", r.version).put("date", r.date)
                        .put("notes", r.notes).put("boards", boards))
                }
                out.put("releases", arr)
            } catch (e: Exception) {
                out.put("error", e.message ?: e.javaClass.simpleName)
            }
            js("window.__fwReleases&&window.__fwReleases(${out})")
        }.start()
    }

    private fun progress(phase: String, msg: String, done: Long = 0, total: Long = 0) {
        val o = JSONObject().put("phase", phase).put("msg", msg).put("done", done).put("total", total)
        js("window.__fwProgress&&window.__fwProgress(${o})")
    }

    private fun bleOk(method: String, path: String): JSONObject {
        val r = bleSync(method, path) ?: throw Exception("no reply from the logger over Bluetooth")
        val body = String(r.second)
        val j = runCatching { JSONObject(body) }.getOrElse { JSONObject() }
        if (r.first != 200) throw Exception(j.optString("error").ifBlank { "logger said HTTP ${r.first}" })
        return j
    }

    /** Download release [tag]'s firmware for [board] and send it to the logger over Bluetooth. */
    fun installFirmware(tag: String, board: String) {
        if (busy) return
        busy = true
        act.runOnUiThread { act.window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON) }
        Thread {
            try {
                progress("download", "Downloading firmware with the phone's internet…")
                val rel = releases().firstOrNull { it.tag == tag } ?: throw Exception("release $tag not found")
                val url = rel.assets["firmware-$board.bin"] ?: throw Exception("release ${rel.version} has no firmware for $board")
                val fw = httpGet(url) { got, total ->
                    progress("download", "Downloading firmware ${rel.version}…", got.toLong(), total.toLong())
                }
                if (fw.size < 100_000 || (fw[0].toInt() and 0xFF) != 0xE9) throw Exception("downloaded file is not an ESP32 firmware image")

                progress("send", "Sending firmware ${rel.version} over Bluetooth…", 0, fw.size.toLong())
                bleOk("POST", "/api/bleota/begin?type=fw&size=${fw.size}")
                val chunk = ble.otaChunkSize()
                var off = 0
                var stalls = 0
                while (off < fw.size) {
                    // A batch of raw chunks [0xA5][offset LE][data], then ask how far it got:
                    // anything lost is simply sent again from there.
                    val batch = ArrayList<ByteArray>()
                    var o = off
                    while (o < fw.size && batch.size < 32) {
                        val n = minOf(chunk, fw.size - o)
                        val b = ByteArray(5 + n)
                        b[0] = 0xA5.toByte()
                        b[1] = (o and 0xFF).toByte(); b[2] = ((o shr 8) and 0xFF).toByte()
                        b[3] = ((o shr 16) and 0xFF).toByte(); b[4] = ((o shr 24) and 0xFF).toByte()
                        System.arraycopy(fw, o, b, 5, n)
                        batch.add(b)
                        o += n
                    }
                    val latch = CountDownLatch(1)
                    var err: Throwable? = null
                    ble.otaSend(batch) { r -> err = r.exceptionOrNull(); latch.countDown() }
                    if (!latch.await(60, TimeUnit.SECONDS)) throw Exception("Bluetooth stopped accepting data")
                    err?.let { throw Exception("Bluetooth: ${it.message}") }
                    val st = bleOk("GET", "/api/bleota/status")
                    st.optString("error").takeIf { it.isNotBlank() }?.let { throw Exception("logger: $it") }
                    val got = st.optLong("got", -1).toInt()
                    if (got < 0) throw Exception("logger lost the transfer")
                    stalls = if (got <= off) stalls + 1 else 0
                    if (stalls >= 5) throw Exception("the transfer stopped moving")
                    off = got
                    progress("send", "Sending firmware ${rel.version} over Bluetooth…", off.toLong(), fw.size.toLong())
                }
                progress("finish", "Checking the new firmware…")
                bleOk("POST", "/api/bleota/end")
                runCatching { bleSync("POST", "/api/bleota/reboot") }   // the reply may die with the restart
                progress("done", "Installed ${rel.version}. The logger is restarting - tap it in the list to reconnect.")
                Thread.sleep(4000)
                act.runOnUiThread { act.goHome() }
            } catch (e: Throwable) {
                progress("error", "Firmware update failed: ${e.message ?: e.javaClass.simpleName}. The logger keeps its current firmware.")
            } finally {
                busy = false
                act.runOnUiThread { act.window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON) }
            }
        }.start()
    }
}
