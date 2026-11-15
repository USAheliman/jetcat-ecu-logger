// JetCat ECU Logger app :: OneDrive.kt
//
// Copies the flights to the pilot's own OneDrive:
//   JetCat Logger/JetCat Flights.xlsx
//   JetCat Logger/Flights/<heli>/<flight>.csv
//
// Sign-in: Microsoft identity platform, OAuth 2.0 device authorization grant, tenant
// "consumers" (personal Microsoft accounts), scopes Files.ReadWrite offline_access.
// Source: learn.microsoft.com/entra/identity-platform/v2-oauth2-device-code. The pilot
// opens microsoft.com/devicelogin, types the short code once, and the app keeps the
// refresh token. Needs a (free) app registration: "Personal Microsoft accounts only",
// "Allow public client flows" = Yes, delegated Graph permission Files.ReadWrite.
// Its Application (client) ID is the build setting ONEDRIVE_CLIENT_ID.
//
// Upload: Microsoft Graph "PUT /me/drive/root:/{path}:/content" (simple upload, files
// up to 250 MB - ours are tens of kB). Anything not yet uploaded stays queued and is
// retried whenever the app runs with a network connection.
// GPL-3.0-or-later.

package com.usaheliman.jetcatlogger

import android.content.Context
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

class OneDrive(private val context: Context, private val clientId: String) {
    private val prefs = context.getSharedPreferences("onedrive", Context.MODE_PRIVATE)
    private val authority = "https://login.microsoftonline.com/consumers/oauth2/v2.0"
    private val scope = "Files.ReadWrite offline_access"

    val configured get() = clientId.isNotBlank()
    val signedIn get() = prefs.getString("refresh", null) != null
    val account get() = prefs.getString("account", "") ?: ""

    // ── Queue ───────────────────────────────────────────────────────
    // Entries: "csv:<key>" for a flight's log, "xlsx" for the workbook.
    @Synchronized fun queue(item: String) {
        val q = pending().toMutableSet(); q.add(item)
        prefs.edit().putStringSet("pending", q).apply()
    }
    @Synchronized fun pending(): Set<String> = prefs.getStringSet("pending", emptySet())?.toSet() ?: emptySet()
    @Synchronized private fun done(item: String) {
        val q = pending().toMutableSet(); q.remove(item)
        prefs.edit().putStringSet("pending", q).putLong("lastUpload", System.currentTimeMillis()).apply()
    }
    val lastUpload get() = prefs.getLong("lastUpload", 0L)
    var lastError: String
        get() = prefs.getString("lastError", "") ?: ""
        private set(v) { prefs.edit().putString("lastError", v).apply() }

    // ── Sign-in (device code) ───────────────────────────────────────
    class DeviceCode(val userCode: String, val verificationUri: String, val deviceCode: String,
                     val intervalS: Int, val expiresS: Int, val message: String)

    /** Step 1: ask Microsoft for a code to show the pilot. Blocking - call off the UI thread. */
    fun startSignIn(): DeviceCode {
        val j = postForm("$authority/devicecode", mapOf("client_id" to clientId, "scope" to scope)).second
        return DeviceCode(j.getString("user_code"), j.optString("verification_uri", "https://microsoft.com/devicelogin"),
            j.getString("device_code"), j.optInt("interval", 5), j.optInt("expires_in", 900), j.optString("message"))
    }

    /** Step 2: poll until the pilot has signed in. Returns null on success, else why it stopped. */
    fun finishSignIn(dc: DeviceCode, cancelled: () -> Boolean): String? {
        var interval = dc.intervalS.coerceAtLeast(5)
        val until = System.currentTimeMillis() + dc.expiresS * 1000L
        while (System.currentTimeMillis() < until) {
            Thread.sleep(interval * 1000L)
            if (cancelled()) return "cancelled"
            val (code, j) = postForm("$authority/token", mapOf(
                "grant_type" to "urn:ietf:params:oauth:grant-type:device_code",
                "client_id" to clientId, "device_code" to dc.deviceCode))
            if (code == 200) { keepTokens(j); return null }
            when (j.optString("error")) {
                "authorization_pending" -> {}
                "slow_down" -> interval += 5            // RFC 8628 section 3.5
                "authorization_declined" -> return "sign-in was declined"
                "expired_token" -> return "the code expired - try again"
                else -> return j.optString("error_description", j.optString("error", "HTTP $code")).lineSequence().first()
            }
        }
        return "the code expired - try again"
    }

    fun signOut() { prefs.edit().remove("refresh").remove("access").remove("accessUntil").remove("account").apply() }

    private fun keepTokens(j: JSONObject) {
        val e = prefs.edit()
        e.putString("access", j.getString("access_token"))
        e.putLong("accessUntil", System.currentTimeMillis() + (j.optLong("expires_in", 3600) - 120) * 1000)
        if (j.has("refresh_token")) e.putString("refresh", j.getString("refresh_token"))
        e.apply()
        if (account.isEmpty()) runCatching {
            val me = JSONObject(String(graph("GET", "https://graph.microsoft.com/v1.0/me/drive?\$select=owner", null).second))
            val name = me.optJSONObject("owner")?.optJSONObject("user")?.optString("displayName") ?: ""
            prefs.edit().putString("account", name).apply()
        }
    }

    private fun accessToken(): String {
        val until = prefs.getLong("accessUntil", 0)
        prefs.getString("access", null)?.let { if (System.currentTimeMillis() < until) return it }
        val refresh = prefs.getString("refresh", null) ?: throw Exception("not signed in to OneDrive")
        val (code, j) = postForm("$authority/token", mapOf(
            "grant_type" to "refresh_token", "client_id" to clientId, "refresh_token" to refresh, "scope" to scope))
        if (code != 200) {
            if (j.optString("error") == "invalid_grant") signOut()
            throw Exception("OneDrive sign-in expired - sign in again")
        }
        keepTokens(j)
        return prefs.getString("access", "")!!
    }

    // ── Upload ──────────────────────────────────────────────────────
    /** Uploads everything queued. Blocking - call off the UI thread. Returns files uploaded. */
    @Synchronized fun uploadPending(store: FlightStore): Int {
        if (!configured || !signedIn) return 0
        var n = 0
        val helis = store.helis()
        for (item in pending().sortedBy { if (it == "xlsx") 1 else 0 }) {   // CSVs first, workbook last
            try {
                if (item == "xlsx") {
                    put("JetCat Logger/JetCat Flights.xlsx", Workbook.build(store.allFlights(), helis),
                        "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet")
                } else if (item.startsWith("csv:")) {
                    val key = item.removePrefix("csv:")
                    val csv = store.csv(key)
                    val serial = store.json(key)?.optString("serial") ?: key.substringBefore('-')
                    val heli = folder(helis.optString(serial, "").ifEmpty { "ECU $serial" })
                    if (csv != null) put("JetCat Logger/Flights/$heli/$key.csv", csv, "text/csv")
                }
                done(item); n++
                lastError = ""
            } catch (e: Exception) {
                lastError = e.message ?: e.toString()
                break                                   // offline or signed out: try again later
            }
        }
        return n
    }

    private fun folder(s: String) = s.replace(Regex("[\"*:<>?/\\\\|#%]"), " ").trim().ifEmpty { "Model" }

    private fun put(path: String, bytes: ByteArray, type: String) {
        val enc = path.split('/').joinToString("/") { URLEncoder.encode(it, "UTF-8").replace("+", "%20") }
        val (code, body) = graph("PUT", "https://graph.microsoft.com/v1.0/me/drive/root:/$enc:/content", bytes, type)
        if (code !in 200..299) throw Exception("OneDrive upload failed (HTTP $code): " + String(body).take(160))
    }

    private fun graph(method: String, url: String, body: ByteArray?, type: String = "application/octet-stream"): Pair<Int, ByteArray> {
        val c = URL(url).openConnection() as HttpURLConnection
        c.requestMethod = method
        c.connectTimeout = 15000; c.readTimeout = 30000
        c.setRequestProperty("Authorization", "Bearer " + accessToken())
        if (body != null) {
            c.doOutput = true
            c.setRequestProperty("Content-Type", type)
            c.setFixedLengthStreamingMode(body.size)
            c.outputStream.use { it.write(body) }
        }
        val code = c.responseCode
        val bytes = (if (code < 400) c.inputStream else c.errorStream)?.use { it.readBytes() } ?: ByteArray(0)
        c.disconnect()
        return code to bytes
    }

    private fun postForm(url: String, form: Map<String, String>): Pair<Int, JSONObject> {
        val data = form.entries.joinToString("&") { URLEncoder.encode(it.key, "UTF-8") + "=" + URLEncoder.encode(it.value, "UTF-8") }
            .toByteArray()
        val c = URL(url).openConnection() as HttpURLConnection
        c.requestMethod = "POST"; c.doOutput = true
        c.connectTimeout = 15000; c.readTimeout = 30000
        c.setRequestProperty("Content-Type", "application/x-www-form-urlencoded")
        c.outputStream.use { it.write(data) }
        val code = c.responseCode
        val text = (if (code < 400) c.inputStream else c.errorStream)?.use { String(it.readBytes()) } ?: "{}"
        c.disconnect()
        return code to runCatching { JSONObject(text) }.getOrElse { JSONObject().put("error", "HTTP $code") }
    }
}
