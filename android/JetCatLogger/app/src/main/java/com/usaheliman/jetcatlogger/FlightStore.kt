// JetCat ECU Logger app :: FlightStore.kt
//
// Every flight is copied to the phone and kept, so the Fleet, Flights and Trends pages
// work with the logger switched off, and nothing is lost if a flight is later deleted on
// the logger. Files live in the app's private storage:
//   flights/<key>.json   the flight's summary, exactly as /api/flights.json lists it
//   flights/<key>.csv    the last run, exactly as /api/flight.csv?key=<key> returns it
//   helis.json           ECU serial -> heli name, as /api/helis.json returns it
// GPL-3.0-or-later.

package com.usaheliman.jetcatlogger

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

class FlightStore(context: Context) {
    private val dir = File(context.filesDir, "flights").apply { mkdirs() }
    private val helisFile = File(context.filesDir, "helis.json")

    /** Flight keys are "<serial>-<runs>", digits and a dash; anything else is refused. */
    private fun safeKey(key: String) = key.isNotEmpty() && key.length <= 32 &&
        key.all { it.isLetterOrDigit() || it == '-' || it == '_' }

    @Synchronized fun hasJson(key: String) = safeKey(key) && File(dir, "$key.json").isFile
    @Synchronized fun hasCsv(key: String) = safeKey(key) && File(dir, "$key.csv").isFile

    @Synchronized fun putJson(key: String, flight: JSONObject) {
        if (!safeKey(key)) return
        writeAtomically(File(dir, "$key.json"), flight.toString(1).toByteArray())
    }
    @Synchronized fun putCsv(key: String, csv: ByteArray) {
        if (!safeKey(key) || csv.isEmpty()) return
        writeAtomically(File(dir, "$key.csv"), csv)
    }
    @Synchronized fun csv(key: String): ByteArray? =
        if (hasCsv(key)) File(dir, "$key.csv").readBytes() else null
    @Synchronized fun json(key: String): JSONObject? =
        if (hasJson(key)) runCatching { JSONObject(File(dir, "$key.json").readText()) }.getOrNull() else null

    /** All stored flights, as the array /api/flights.json would return. */
    @Synchronized fun allFlights(): JSONArray {
        val out = JSONArray()
        dir.listFiles { f -> f.isFile && f.name.endsWith(".json") }
            ?.sortedBy { it.name }
            ?.forEach { f -> runCatching { out.put(JSONObject(f.readText())) } }
        return out
    }
    @Synchronized fun count() = dir.listFiles { f -> f.isFile && f.name.endsWith(".json") }?.size ?: 0

    /** The logger's list merged with the phone's copies: the logger's entry wins for a
     *  flight both have, and flights only the phone still has are kept. */
    @Synchronized fun merged(fromLogger: JSONArray): JSONArray {
        val seen = HashSet<String>()
        val out = JSONArray()
        for (i in 0 until fromLogger.length()) {
            val f = fromLogger.optJSONObject(i) ?: continue
            seen.add(f.optString("key")); out.put(f)
        }
        val mine = allFlights()
        for (i in 0 until mine.length()) {
            val f = mine.optJSONObject(i) ?: continue
            if (f.optString("key") !in seen) out.put(f)
        }
        return out
    }

    @Synchronized fun helis(): JSONObject =
        runCatching { JSONObject(helisFile.readText()) }.getOrElse { JSONObject() }
    @Synchronized fun putHelis(h: JSONObject) {
        // Names typed on the phone page go to the logger; keep any the logger no longer has.
        val merged = helis()
        h.keys().forEach { k -> merged.put(k, h.get(k)) }
        writeAtomically(helisFile, merged.toString(1).toByteArray())
    }

    private fun writeAtomically(f: File, bytes: ByteArray) {
        val tmp = File(f.parentFile, f.name + ".tmp")
        tmp.writeBytes(bytes)
        if (!tmp.renameTo(f)) { f.delete(); tmp.renameTo(f) }
    }
}
