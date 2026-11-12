// JetCat ECU Logger app :: MainActivity.kt
//
// Home screen (loggers found nearby, OneDrive) -> the logger's own web pages in a WebView.
// The pages are bundled in the app (the same data/index.html the logger serves over WiFi).
// Every fetch() the pages make goes through a small JavaScript shim to the Bridge below:
//   - logger connected: the request crosses Bluetooth (Malcolm Messiter's HTTP-over-BLE
//     framing, see LoggerBle.kt), and flight lists, logs and heli names are copied to the
//     phone on the way past;
//   - logger not connected: the request is answered from the copies on the phone, so
//     Fleet, Flights and Trends still work.
// After each connection every flight the phone hasn't got is copied, and new flights are
// queued for OneDrive (OneDrive.kt), uploaded whenever the phone has a connection.
//
// The shim and the WebView handling follow Malcolm's RXV2App MainActivity (GPL-3.0-or-later,
// used with his permission). GPL-3.0-or-later.

package com.usaheliman.jetcatlogger

import android.Manifest
import android.annotation.SuppressLint
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Typeface
import android.net.ConnectivityManager
import android.net.Network
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.util.Base64
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.webkit.*
import android.widget.*
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.FileProvider
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayInputStream
import java.io.File
import java.text.DateFormat
import java.util.Date
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

class MainActivity : AppCompatActivity() {
    private val ORIGIN = "https://jetcat.logger"
    private lateinit var ble: LoggerBle
    private lateinit var store: FlightStore
    private lateinit var drive: OneDrive
    private lateinit var root: FrameLayout
    private var webView: WebView? = null
    private var statusText: TextView? = null
    private var listAdapter: LoggerAdapter? = null
    private var driveText: TextView? = null
    private var driveButton: Button? = null
    @Volatile private var syncing = false
    private lateinit var updates: Updates
    private var signInCancelled = false

    private val connected get() = ble.state is LoggerBle.State.Ready

    // ── Life cycle ──────────────────────────────────────────────────
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        store = FlightStore(this)
        drive = OneDrive(this, BuildConfig.ONEDRIVE_CLIENT_ID)
        ble = LoggerBle(this)
        updates = Updates(this, ble, ::bleSync,
            js = { code -> runOnUiThread { webView?.evaluateJavascript(code, null) } },
            toast = { m -> runOnUiThread { toast(m) } })
        root = FrameLayout(this)
        setContentView(root)
        // Android 15 draws apps edge to edge (under the status and navigation bars): keep the
        // home screen and the pages clear of them, and of any camera cut-out.
        ViewCompat.setOnApplyWindowInsetsListener(root) { v, insets ->
            val b = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            v.setPadding(b.left, b.top, b.right, b.bottom)
            WindowInsetsCompat.CONSUMED
        }
        ble.onFound = { listAdapter?.submit(it) }
        ble.onState = { s -> onBleState(s) }
        showHome()
        requestPerms()
        // Upload whatever is waiting as soon as the phone has a connection.
        runCatching {
            (getSystemService(CONNECTIVITY_SERVICE) as ConnectivityManager)
                .registerDefaultNetworkCallback(object : ConnectivityManager.NetworkCallback() {
                    override fun onAvailable(network: Network) { uploadInBackground() }
                })
        }
    }

    override fun onResume() {
        super.onResume()
        ble.appInForeground = true
        if (webView == null) startScanIfPermitted()
        uploadInBackground()
        updates.resumePending()        // back from allowing app installs
    }

    override fun onPause() {
        super.onPause()
        ble.appInForeground = false
        if (webView == null) ble.stopScan()
    }

    @Deprecated("Back goes from the pages to the home screen")
    override fun onBackPressed() {
        val w = webView
        if (w != null) { if (w.canGoBack()) w.goBack() else showHome() } else super.onBackPressed()
    }

    private fun onBleState(s: LoggerBle.State) {
        when (s) {
            is LoggerBle.State.Ready -> {
                statusText?.text = "Connected to ${s.name}"
                showWeb()
                syncInBackground()
            }
            is LoggerBle.State.Connecting -> statusText?.text = "Connecting to ${s.name}…"
            is LoggerBle.State.Reconnecting -> statusText?.text = "Reconnecting to ${s.name}…"
            is LoggerBle.State.Scanning -> statusText?.text = "Looking for loggers… press the logger's button or plug it into an ECU to wake it."
            is LoggerBle.State.Failed -> {
                statusText?.text = s.msg
                if (webView != null) toast("Logger disconnected - showing the flights on this phone")
            }
            is LoggerBle.State.Idle -> {
                if (webView != null) toast("Logger disconnected - showing the flights on this phone")
                else startScanIfPermitted()
            }
        }
    }

    // ── Permissions ─────────────────────────────────────────────────
    private fun neededPerms(): Array<String> =
        if (Build.VERSION.SDK_INT >= 31) arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
    private fun hasPerms() = neededPerms().all { checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }
    private fun requestPerms() { if (!hasPerms()) requestPermissions(neededPerms(), 1) }
    override fun onRequestPermissionsResult(rc: Int, p: Array<out String>, g: IntArray) {
        super.onRequestPermissionsResult(rc, p, g)
        startScanIfPermitted()
    }
    private fun startScanIfPermitted() {
        if (!hasPerms()) { statusText?.text = "Bluetooth permission is needed to find the logger."; return }
        if (connected || ble.state is LoggerBle.State.Connecting) return
        ble.startScan()
        // Connect straight away to the logger used last time, if it was close enough then.
        val prefs = getSharedPreferences("scanner", MODE_PRIVATE)
        val addr = prefs.getString("lastAddr", null)
        if (addr != null && !autoTried) { autoTried = true; ble.fastConnect("JetCat-Logger", addr) }
    }
    private var autoTried = false

    // ── Home screen (native) ────────────────────────────────────────
    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    private fun showHome() {
        webView?.let { root.removeView(it); it.destroy() }
        webView = null
        root.removeAllViews()
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(28), dp(20), dp(20))
        }
        col.addView(TextView(this).apply { text = "JetCat Logger"; textSize = 26f; setTypeface(typeface, Typeface.BOLD) })
        statusText = TextView(this).apply { textSize = 15f; setPadding(0, dp(8), 0, dp(12)) }
        col.addView(statusText)
        col.addView(TextView(this).apply { text = "Loggers nearby"; textSize = 13f; alpha = 0.7f })
        val list = ListView(this)
        listAdapter = LoggerAdapter()
        list.adapter = listAdapter
        list.setOnItemClickListener { _, _, i, _ -> listAdapter?.item(i)?.let { ble.connect(it) } }
        col.addView(list, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        col.addView(Button(this).apply {
            text = "View flights on this phone (${store.count()})"
            setOnClickListener { showWeb() }
        })
        col.addView(TextView(this).apply { text = "OneDrive"; textSize = 13f; alpha = 0.7f; setPadding(0, dp(16), 0, 0) })
        driveText = TextView(this).apply { textSize = 14f; setPadding(0, dp(4), 0, dp(4)) }
        col.addView(driveText)
        driveButton = Button(this)
        col.addView(driveButton)
        col.addView(TextView(this).apply {
            text = "App version ${updates.versionName()} · tap to check for updates"
            textSize = 12f; alpha = 0.6f; setPadding(0, dp(14), 0, 0)
            setOnClickListener { updates.checkApp(true) }
        })
        root.addView(col)
        updates.checkApp(false)        // at most hourly; silent unless there is a newer release
        refreshDrive()
        statusText?.text = if (connected) "Connected" else "Looking for loggers… press the logger's button or plug it into an ECU to wake it."
        if (connected) ble.disconnect()       // home = leave the logger; it can go back to sleep
        startScanIfPermitted()
    }

    private fun refreshDrive() {
        val t = driveText ?: return
        val b = driveButton ?: return
        when {
            !drive.configured -> { t.text = "OneDrive upload is not set up in this build of the app."; b.visibility = View.GONE }
            !drive.signedIn -> { t.text = "Not signed in. Flights are kept on this phone."; b.text = "Sign in to OneDrive"
                b.visibility = View.VISIBLE; b.setOnClickListener { signIn() } }
            else -> {
                val n = drive.pending().size
                t.text = "Signed in${if (drive.account.isNotEmpty()) " as ${drive.account}" else ""}. " +
                    (if (n > 0) "$n file(s) waiting to upload." else "Everything uploaded.") +
                    (if (drive.lastError.isNotEmpty()) "\nLast problem: ${drive.lastError}" else "")
                b.text = "Upload now"; b.visibility = View.VISIBLE; b.setOnClickListener { uploadInBackground(true) }
            }
        }
    }

    inner class LoggerAdapter : BaseAdapter() {
        private var items: List<LoggerBle.Discovered> = emptyList()
        fun submit(l: List<LoggerBle.Discovered>) { items = l; notifyDataSetChanged() }
        fun item(i: Int) = items.getOrNull(i)
        override fun getCount() = items.size
        override fun getItem(i: Int) = items[i]
        override fun getItemId(i: Int) = i.toLong()
        override fun getView(i: Int, convert: View?, parent: ViewGroup?): View {
            val d = items[i]
            return (convert as? TextView ?: TextView(this@MainActivity)).apply {
                text = "${d.name}    ${LoggerBle.signalWord(d.rssi)} (${d.rssi} dBm)"
                textSize = 17f; setPadding(dp(8), dp(14), dp(8), dp(14))
            }
        }
    }

    // ── Web pages ───────────────────────────────────────────────────
    @SuppressLint("SetJavaScriptEnabled")
    private fun showWeb() {
        if (webView != null) return
        ble.stopScan()
        root.removeAllViews()
        val w = WebView(this)
        webView = w
        w.settings.javaScriptEnabled = true
        w.settings.domStorageEnabled = true
        w.settings.cacheMode = WebSettings.LOAD_NO_CACHE
        w.addJavascriptInterface(Bridge(), "AndroidBle")
        w.addJavascriptInterface(AppBridge(), "AndroidApp")
        // alert()/confirm() show nothing in a WebView without a WebChromeClient.
        w.webChromeClient = object : WebChromeClient() {
            override fun onJsAlert(v: WebView?, url: String?, msg: String?, r: JsResult?): Boolean {
                AlertDialog.Builder(this@MainActivity).setMessage(msg ?: "")
                    .setPositiveButton("OK") { _, _ -> r?.confirm() }.setOnCancelListener { r?.cancel() }.show(); return true
            }
            override fun onJsConfirm(v: WebView?, url: String?, msg: String?, r: JsResult?): Boolean {
                AlertDialog.Builder(this@MainActivity).setMessage(msg ?: "")
                    .setPositiveButton("OK") { _, _ -> r?.confirm() }.setNegativeButton("Cancel") { _, _ -> r?.cancel() }
                    .setOnCancelListener { r?.cancel() }.show(); return true
            }
        }
        w.webViewClient = object : WebViewClient() {
            override fun onRenderProcessGone(view: WebView, detail: RenderProcessGoneDetail): Boolean {
                webView = null
                root.post { runCatching { root.removeView(view); view.destroy() }; showWeb() }
                return true
            }
            override fun shouldInterceptRequest(view: WebView, req: WebResourceRequest): WebResourceResponse? {
                if (req.url.host != Uri.parse(ORIGIN).host) return null
                val path = req.url.path ?: "/"
                if (path == "/" || path == "/index.html") {
                    val html = assets.open("index.html").use { it.readBytes() }
                    return WebResourceResponse("text/html", "utf-8", 200, "OK",
                        mapOf("Cache-Control" to "no-store"), ByteArrayInputStream(SHIM.toByteArray() + html))
                }
                return WebResourceResponse("text/plain", "utf-8", 404, "Not Found", emptyMap(), ByteArrayInputStream(ByteArray(0)))
            }
            override fun shouldOverrideUrlLoading(view: WebView, req: WebResourceRequest): Boolean {
                // The logger's /ota page is for WiFi only; nothing else leaves the app.
                return req.url.host != Uri.parse(ORIGIN).host
            }
        }
        root.addView(w)
        w.loadUrl("$ORIGIN/")
    }

    private fun resolveJs(id: Int, code: Int, type: String, body: ByteArray) = runOnUiThread {
        val b64 = Base64.encodeToString(body, Base64.NO_WRAP)
        webView?.evaluateJavascript("window.__bleResolve($id,$code,${JSONObject.quote(type)},${JSONObject.quote(b64)})", null)
    }
    private fun json(o: Any) = o.toString().toByteArray()

    // ── fetch() bridge ──────────────────────────────────────────────
    inner class Bridge {
        @JavascriptInterface
        fun request(id: Int, method: String, url: String, headersJson: String, body: String) {
            val uri = Uri.parse(if (url.startsWith("http")) url else "$ORIGIN$url")
            var pq = uri.encodedPath ?: "/"
            if (!uri.encodedQuery.isNullOrEmpty()) pq += "?" + uri.encodedQuery
            val path = uri.path ?: "/"
            val m = method.uppercase()
            if (!connected) { answerFromPhone(id, m, path, uri); return }
            ble.request(m, pq, emptyMap(), if (body.isEmpty()) null else body.toByteArray()) { res ->
                val r = res.getOrNull()
                if (r == null || r.code >= 500) {
                    // Link dropped mid-request: fall back to the phone's copies.
                    answerFromPhone(id, m, path, uri); return@request
                }
                val code = if (r.code == 0) 200 else r.code
                var out = r.body
                if (code == 200) runCatching {
                    when {
                        m == "GET" && path == "/api/flights.json" -> {
                            val arr = JSONArray(String(r.body))
                            keepFlights(arr)
                            out = json(store.merged(arr))
                        }
                        m == "GET" && path == "/api/helis.json" -> {
                            store.putHelis(JSONObject(String(r.body))); out = json(store.helis())
                        }
                        m == "POST" && path == "/api/heli" -> {
                            store.putHelis(JSONObject(String(r.body))); drive.queue("xlsx")
                        }
                        m == "GET" && path == "/api/flight.csv" ->
                            uri.getQueryParameter("key")?.let { store.putCsv(it, r.body) }
                        else -> {}
                    }
                }
                resolveJs(id, code, r.contentType.ifEmpty { "application/json" }, out)
            }
        }
    }

    /** The logger isn't connected: answer from the copies kept on the phone. */
    private fun answerFromPhone(id: Int, method: String, path: String, uri: Uri) {
        val j = "application/json"
        when {
            method == "GET" && path == "/api/flights.json" -> resolveJs(id, 200, j, json(store.allFlights()))
            method == "GET" && path == "/api/helis.json" -> resolveJs(id, 200, j, json(store.helis()))
            method == "GET" && path == "/api/flight.csv" -> {
                val csv = uri.getQueryParameter("key")?.let { store.csv(it) }
                if (csv != null) resolveJs(id, 200, "text/csv", csv)
                else resolveJs(id, 404, j, json(JSONObject().put("error", "this flight's log isn't on the phone")))
            }
            method == "GET" && path == "/api/status.json" -> resolveJs(id, 200, j, json(JSONObject()
                .put("fw", "").put("state", "Idle").put("downloading", false).put("dump_bytes", 0)
                .put("flights", store.count()).put("via", "phone").put("last", "").put("rtc", false)))
            method == "GET" && path == "/api/console" -> resolveJs(id, 200, j,
                json(JSONObject().put("first", 0).put("next", 0).put("total", 0).put("text", "(logger not connected)\n")))
            method == "POST" && path == "/api/time" -> resolveJs(id, 200, j, json(JSONObject().put("ok", true)))
            else -> resolveJs(id, 503, j, json(JSONObject().put("error",
                "the logger isn't connected - showing the flights on this phone")))
        }
    }

    /** Copies flight summaries from a list the logger sent; new ones are queued for OneDrive. */
    private fun keepFlights(arr: JSONArray): List<String> {
        val fresh = ArrayList<String>()
        for (i in 0 until arr.length()) {
            val f = arr.optJSONObject(i) ?: continue
            val key = f.optString("key"); if (key.isEmpty()) continue
            val old = store.json(key)
            if (old == null) fresh.add(key)
            else if (old.optJSONObject("last_run")?.toString() != f.optJSONObject("last_run")?.toString()) {
                store.dropCsv(key)   // the logger rebuilt this flight: fetch its log again
                drive.queue("xlsx")
            }
            store.putJson(key, f)
        }
        if (fresh.isNotEmpty()) drive.queue("xlsx")
        return fresh
    }

    // ── Sync after connecting ───────────────────────────────────────
    private fun bleSync(method: String, path: String): Pair<Int, ByteArray>? {
        val latch = CountDownLatch(1)
        var out: Pair<Int, ByteArray>? = null
        ble.request(method, path, emptyMap(), null) { res ->
            res.getOrNull()?.let { out = (if (it.code == 0) 200 else it.code) to it.body }
            latch.countDown()
        }
        latch.await(40, TimeUnit.SECONDS)
        return out
    }

    /** For Updates: back to the home screen after a firmware install (the logger restarts). */
    fun goHome() = showHome()

    private fun syncInBackground() {
        if (syncing || updates.busy) return
        syncing = true
        Thread {
            try {
                val list = bleSync("GET", "/api/flights.json") ?: return@Thread
                if (list.first != 200) return@Thread
                val arr = JSONArray(String(list.second))
                keepFlights(arr)
                bleSync("GET", "/api/helis.json")?.let { if (it.first == 200) store.putHelis(JSONObject(String(it.second))) }
                var copied = 0
                for (i in 0 until arr.length()) {
                    if (!connected) break
                    val key = arr.optJSONObject(i)?.optString("key") ?: continue
                    if (store.hasCsv(key)) continue
                    val csv = bleSync("GET", "/api/flight.csv?key=" + Uri.encode(key)) ?: continue
                    if (csv.first == 200 && csv.second.isNotEmpty()) {
                        store.putCsv(key, csv.second); drive.queue("csv:$key"); copied++
                    }
                }
                if (copied > 0) { drive.queue("xlsx"); runOnUiThread { toast("$copied new flight(s) copied to the phone") } }
                uploadInBackground()
            } catch (e: Exception) {
            } finally { syncing = false }
        }.start()
    }

    private fun uploadInBackground(manual: Boolean = false) {
        if (!drive.configured || !drive.signedIn || drive.pending().isEmpty()) {
            if (manual) toast(if (drive.pending().isEmpty()) "Everything is already uploaded" else "Sign in to OneDrive first")
            return
        }
        Thread {
            val n = runCatching { drive.uploadPending(store) }.getOrDefault(0)
            runOnUiThread {
                refreshDrive()
                if (manual || n > 0) toast(if (n > 0) "Uploaded $n file(s) to OneDrive" else "Upload failed: ${drive.lastError}")
            }
        }.start()
    }

    // ── OneDrive sign-in (device code) ──────────────────────────────
    private fun signIn() {
        if (!drive.configured) return
        signInCancelled = false
        Thread {
            val dc = runCatching { drive.startSignIn() }.getOrElse { e ->
                runOnUiThread { toast("Could not reach Microsoft: ${e.message}") }; return@Thread
            }
            runOnUiThread { showCodeDialog(dc) }
            val err = runCatching { drive.finishSignIn(dc) { signInCancelled } }.getOrElse { it.message }
            runOnUiThread {
                codeDialog?.dismiss()
                if (err == null) { toast("Signed in to OneDrive"); drive.queue("xlsx"); uploadInBackground() }
                else if (err != "cancelled") toast("OneDrive sign-in: $err")
                refreshDrive(); webView?.evaluateJavascript("if(tab==='settings')render()", null)
            }
        }.start()
    }
    private var codeDialog: AlertDialog? = null
    private fun showCodeDialog(dc: OneDrive.DeviceCode) {
        val v = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(24), dp(16), dp(24), dp(8)) }
        v.addView(TextView(this).apply { text = "On the Microsoft page, sign in and enter this code:"; textSize = 15f })
        v.addView(TextView(this).apply {
            text = dc.userCode; textSize = 30f; setTypeface(Typeface.MONOSPACE, Typeface.BOLD)
            gravity = Gravity.CENTER; setPadding(0, dp(16), 0, dp(16)); setTextIsSelectable(true)
        })
        v.addView(TextView(this).apply { text = "The code has been copied - paste it in. This window closes by itself once you're signed in."; textSize = 13f; alpha = 0.7f })
        (getSystemService(CLIPBOARD_SERVICE) as ClipboardManager).setPrimaryClip(ClipData.newPlainText("code", dc.userCode))
        codeDialog = AlertDialog.Builder(this).setTitle("Sign in to OneDrive").setView(v)
            .setPositiveButton("Open Microsoft page", null)
            .setNegativeButton("Cancel") { _, _ -> signInCancelled = true }
            .setCancelable(false).create()
        codeDialog?.setOnShowListener {
            codeDialog?.getButton(AlertDialog.BUTTON_POSITIVE)?.setOnClickListener {
                runCatching { startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(dc.verificationUri))) }
            }
        }
        codeDialog?.show()
    }

    // ── Things the pages ask of the app ─────────────────────────────
    inner class AppBridge {
        @JavascriptInterface
        fun oneDriveStatus(): String {
            val last = drive.lastUpload
            return JSONObject().put("configured", drive.configured).put("signedIn", drive.signedIn)
                .put("account", drive.account).put("pending", drive.pending().size).put("error", drive.lastError)
                .put("flights", store.count())
                .put("lastUpload", if (last > 0) DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(last)) else "")
                .toString()
        }
        @JavascriptInterface fun oneDriveSignIn() = runOnUiThread { signIn() }
        @JavascriptInterface fun oneDriveUploadNow() = runOnUiThread { uploadInBackground(true) }
        @JavascriptInterface fun oneDriveSignOut() = runOnUiThread {
            drive.signOut(); refreshDrive(); webView?.evaluateJavascript("if(tab==='settings')render()", null)
        }
        @JavascriptInterface fun home() = runOnUiThread { showHome() }
        @JavascriptInterface fun appVersion(): String = updates.versionName()
        @JavascriptInterface fun checkAppUpdate() = runOnUiThread { updates.checkApp(true) }
        @JavascriptInterface fun firmwareReleases() = updates.firmwareList()
        @JavascriptInterface fun installFirmware(tag: String, board: String) {
            if (!connected) { runOnUiThread { toast("Connect to the logger first.") }; return }
            updates.installFirmware(tag, board)
        }
        /** The pages' CSV exports: a WebView can't download, so hand the file to the share sheet. */
        @JavascriptInterface fun saveFile(name: String, text: String) = runOnUiThread {
            runCatching {
                val dir = File(cacheDir, "exports").apply { mkdirs() }
                val f = File(dir, name.replace(Regex("[^A-Za-z0-9._-]"), "_"))
                f.writeText(text)
                val u = FileProvider.getUriForFile(this@MainActivity, "$packageName.fileprovider", f)
                startActivity(Intent.createChooser(Intent(Intent.ACTION_SEND).apply {
                    type = "text/csv"; putExtra(Intent.EXTRA_STREAM, u)
                    addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                }, "Save or send $name"))
            }.onFailure { toast("Could not export: ${it.message}") }
        }
    }

    private fun toast(m: String) = Toast.makeText(this, m, Toast.LENGTH_LONG).show()

    companion object {
        // fetch() -> AndroidBle.request -> window.__bleResolve, from Malcolm's RXV2App shim.
        private const val SHIM = """<script>
(function(){
  if (window.__bleShim) return; window.__bleShim = true;
  window.__blePending = {}; window.__bleSeq = 0;
  window.__bleResolve = function(id, code, type, b64){
    var p = window.__blePending[id]; if(!p) return; delete window.__blePending[id];
    var bytes; try { bytes = Uint8Array.from(atob(b64||''), function(c){return c.charCodeAt(0);}); }
    catch(e){ bytes = new Uint8Array(); }
    p.resolve(new Response(bytes, {status: code||200, headers: {'Content-Type': type||'text/plain'}}));
  };
  window.fetch = function(input, init){
    init = init || {};
    var url = (typeof input === 'string') ? input : (input && input.url) || '';
    var method = (init.method || 'GET').toUpperCase();
    var body = (init.body != null) ? String(init.body) : '';
    var id = ++window.__bleSeq;
    return new Promise(function(resolve, reject){
      window.__blePending[id] = {resolve:resolve, reject:reject};
      try { AndroidBle.request(id, method, url, '{}', body); } catch(e){ reject(e); }
    });
  };
})();
</script>"""
    }
}
