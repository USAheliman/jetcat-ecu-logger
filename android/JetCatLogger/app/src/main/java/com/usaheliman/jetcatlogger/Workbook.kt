// JetCat ECU Logger app :: Workbook.kt
//
// Builds "JetCat Flights.xlsx" from the flights kept on the phone:
//   Summary      every flight, oldest first per heli
//   <heli name>  one sheet per heli, same columns
//   Notes        what each column means
// Written as plain Office Open XML (a zip of XML parts, ECMA-376), so the app needs no
// spreadsheet library. Columns match the web page's "Export all flights (CSV)".
// GPL-3.0-or-later.

package com.usaheliman.jetcatlogger

import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

object Workbook {

    // Engine-off reasons, as the ECU reports them (JetCat serial protocol; same table as the web page).
    private val OFFS = mapOf(0 to "None", 1 to "Shut down via RC", 2 to "Overtemperature", 3 to "Ignition timeout",
        4 to "Acceleration timeout", 5 to "Acceleration too slow", 6 to "Over RPM", 7 to "Low RPM off",
        8 to "Low battery", 9 to "Auto off", 10 to "Low temperature off", 11 to "High temperature off",
        12 to "Glow plug defective", 13 to "Watchdog timer", 14 to "Failsafe off", 15 to "Manual off (GSU)",
        16 to "Power fail", 17 to "Temp sensor fail", 18 to "Fuel fail", 19 to "Prop fail",
        20 to "2nd engine fail", 21 to "2nd engine differential too high", 22 to "2nd engine no communication")

    private class Col(val title: String, val note: String, val value: (JSONObject, String) -> Any?)

    private fun lr(f: JSONObject): JSONObject = f.optJSONObject("last_run") ?: JSONObject()
    private fun num(o: JSONObject, k: String): Any? = if (o.has(k) && !o.isNull(k)) o.opt(k) else null
    private fun runSecs(f: JSONObject): Any? {
        val l = lr(f)
        val ig = l.optInt("ignition_t", -1); val sd = l.optInt("shutdown_t", -1)
        return if (ig >= 0 && sd >= 0) sd - ig else num(f, "last_run_time_s")
    }
    private fun whenText(f: JSONObject): String {
        val l = lr(f)
        val ign = l.optString("ignition_at", "")
        if (ign.isNotEmpty() && ign != "null") return ign
        val dl = f.optString("downloaded_at", "")
        return if (dl.isNotEmpty()) "before $dl" else ""
    }
    private fun session(f: JSONObject) = f.optBoolean("session_values_valid", false)

    private val COLS = listOf(
        Col("Flight", "Flight key: ECU serial and the ECU's count of successful runs.") { f, _ -> f.optString("key") },
        Col("Heli", "Heli name set on the Settings page.") { _, heli -> heli },
        Col("ECU serial", "Serial number of the ECU (fixed to its turbine).") { f, _ -> f.optString("serial") },
        Col("Run no.", "ECU's count of successful runs after this flight.") { f, _ -> num(f, "runs_ok") },
        Col("Failed starts", "ECU's total count of failed starts.") { f, _ -> num(f, "starts_failed") },
        Col("When", "Time of ignition (UTC) when known; otherwise 'before' the download time.") { f, _ -> whenText(f) },
        Col("Time source", "How the time was worked out.") { f, _ -> f.optString("time_source") },
        Col("Run time (s)", "Ignition to shutdown, from the ECU's log.") { f, _ -> runSecs(f) },
        Col("Start (s)", "Ignition to running.") { f, _ -> num(lr(f), "start_s") },
        Col("Peak start EGT (°C)", "Highest exhaust temperature during the start.") { f, _ -> num(lr(f), "peak_start_egt_c") },
        Col("Max EGT in flight (°C)", "Highest exhaust temperature after the start.") { f, _ -> num(lr(f), "max_egt_c") },
        Col("Mean EGT in flight (°C)", "Average exhaust temperature in flight.") { f, _ -> num(lr(f), "mean_flight_egt_c") },
        Col("Mean RPM in flight", "Average turbine speed in flight.") { f, _ -> num(lr(f), "mean_flight_rpm") },
        Col("Min RPM in flight", "Lowest turbine speed in flight.") { f, _ -> num(lr(f), "min_flight_rpm") },
        Col("Mean pump (V) in flight", "Average fuel pump voltage in flight.") { f, _ -> num(lr(f), "mean_flight_pump_v") },
        Col("Max pump (V)", "Highest fuel pump voltage.") { f, _ -> num(lr(f), "max_pump_v") },
        Col("Min battery at start (V)", "Lowest ECU battery voltage during the start.") { f, _ -> num(lr(f), "min_start_batt_v") },
        Col("Engine off reason", "Why the engine stopped (only when downloaded in the same ECU session).") { f, _ ->
            if (session(f)) OFFS[f.optInt("off_condition", -1)] ?: f.opt("off_condition") else null },
        Col("Failsafe count", "Failsafe events (same session only).") { f, _ -> if (session(f)) num(f, "failsafe_count") else null },
        Col("Failsafe time (s)", "Time in failsafe (same session only).") { f, _ -> if (session(f)) num(f, "failsafe_time_s") else null },
        Col("Total run time (h)", "ECU's total running time.") { f, _ ->
            f.optDouble("total_run_time_s", Double.NaN).takeIf { !it.isNaN() }?.let { Math.round(it / 36.0) / 100.0 } },
        Col("Ambient (°C)", "Temperature at the ECU when downloaded.") { f, _ -> num(f, "ambient_c") },
        Col("Log", "Whether the ECU's log memory was full (oldest data overwritten).") { f, _ -> f.optString("log_status") },
    )

    /** heliName(serial) -> the name to show; flights grouped per heli, oldest first. */
    fun build(flights: JSONArray, helis: JSONObject): ByteArray {
        fun heli(serial: String) = helis.optString(serial, "").ifEmpty { "ECU $serial" }
        val list = (0 until flights.length()).mapNotNull { flights.optJSONObject(it) }
            .sortedWith(compareBy({ heli(it.optString("serial")) }, { it.optInt("runs_ok") }))
        val sheets = ArrayList<Pair<String, List<List<Any?>>>>()
        fun rows(fs: List<JSONObject>) = listOf(COLS.map { it.title }) +
            fs.map { f -> COLS.map { c -> c.value(f, heli(f.optString("serial"))) } }
        sheets.add("Summary" to rows(list))
        val used = HashSet<String>().apply { add("summary"); add("notes") }
        for ((h, fs) in list.groupBy { heli(it.optString("serial")) }) {
            var name = h.replace(Regex("[\\[\\]:*?/\\\\]"), " ").trim().take(31).ifEmpty { "Heli" }
            var n = 2
            while (name.lowercase() in used) { name = name.take(28) + " " + n++ }
            used.add(name.lowercase())
            sheets.add(name to rows(fs))
        }
        sheets.add("Notes" to listOf(listOf<Any?>("Column", "Meaning")) + COLS.map { listOf(it.title, it.note) } +
            listOf(listOf<Any?>(), listOf<Any?>("Written by the JetCat Logger app from the flights kept on the phone.", null),
                   listOf<Any?>("Each flight's second-by-second log is in Flights/<heli>/<flight>.csv beside this file.", null)))
        return xlsx(sheets)
    }

    // ── Minimal .xlsx writer ────────────────────────────────────────
    private fun esc(s: String) = buildString {
        for (c in s) when (c) {
            '&' -> append("&amp;"); '<' -> append("&lt;"); '>' -> append("&gt;"); '"' -> append("&quot;")
            else -> if (c.code < 0x20 && c != '\t' && c != '\n') append(' ') else append(c)
        }
    }
    private fun colName(i: Int): String { var n = i + 1; val sb = StringBuilder()
        while (n > 0) { val r = (n - 1) % 26; sb.insert(0, ('A' + r)); n = (n - 1) / 26 }; return sb.toString() }

    private fun sheetXml(rows: List<List<Any?>>): String {
        val sb = StringBuilder("""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>""")
        sb.append("""<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">""")
        sb.append("""<sheetViews><sheetView workbookViewId="0"><pane ySplit="1" topLeftCell="A2" activePane="bottomLeft" state="frozen"/></sheetView></sheetViews>""")
        val widths = IntArray(rows.maxOfOrNull { it.size } ?: 0)
        rows.forEach { r -> r.forEachIndexed { i, v -> widths[i] = maxOf(widths[i], (v?.toString()?.length ?: 0)) } }
        if (widths.isNotEmpty()) {
            sb.append("<cols>")
            widths.forEachIndexed { i, w -> sb.append("""<col min="${i + 1}" max="${i + 1}" width="${(w + 2).coerceIn(8, 60)}" customWidth="1"/>""") }
            sb.append("</cols>")
        }
        sb.append("<sheetData>")
        rows.forEachIndexed { ri, r ->
            sb.append("""<row r="${ri + 1}">""")
            r.forEachIndexed { ci, v ->
                val ref = colName(ci) + (ri + 1)
                val style = if (ri == 0) " s=\"1\"" else ""
                when (v) {
                    null -> {}
                    is Number -> if (v.toDouble().isFinite()) sb.append("""<c r="$ref"$style><v>$v</v></c>""")
                    else -> sb.append("""<c r="$ref" t="inlineStr"$style><is><t xml:space="preserve">${esc(v.toString())}</t></is></c>""")
                }
            }
            sb.append("</row>")
        }
        sb.append("</sheetData></worksheet>")
        return sb.toString()
    }

    private fun xlsx(sheets: List<Pair<String, List<List<Any?>>>>): ByteArray {
        val bos = ByteArrayOutputStream()
        ZipOutputStream(bos).use { z ->
            fun put(name: String, text: String) { z.putNextEntry(ZipEntry(name)); z.write(text.toByteArray(Charsets.UTF_8)); z.closeEntry() }
            val n = sheets.size
            put("[Content_Types].xml", """<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>""" +
                (1..n).joinToString("") { """<Override PartName="/xl/worksheets/sheet$it.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>""" } + "</Types>")
            put("_rels/.rels", """<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>""")
            put("xl/workbook.xml", """<?xml version="1.0" encoding="UTF-8" standalone="yes"?><workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets>""" +
                sheets.mapIndexed { i, s -> """<sheet name="${esc(s.first)}" sheetId="${i + 1}" r:id="rId${i + 1}"/>""" }.joinToString("") + "</sheets></workbook>")
            put("xl/_rels/workbook.xml.rels", """<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">""" +
                (1..n).joinToString("") { """<Relationship Id="rId$it" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet$it.xml"/>""" } +
                """<Relationship Id="rId${n + 1}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/></Relationships>""")
            put("xl/styles.xml", """<?xml version="1.0" encoding="UTF-8" standalone="yes"?><styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><fonts count="2"><font><sz val="11"/><name val="Calibri"/></font><font><b/><sz val="11"/><name val="Calibri"/></font></fonts><fills count="2"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill></fills><borders count="1"><border/></borders><cellStyleXfs count="1"><xf/></cellStyleXfs><cellXfs count="2"><xf/><xf fontId="1" applyFont="1"/></cellXfs></styleSheet>""")
            sheets.forEachIndexed { i, s -> put("xl/worksheets/sheet${i + 1}.xml", sheetXml(s.second)) }
        }
        return bos.toByteArray()
    }
}
