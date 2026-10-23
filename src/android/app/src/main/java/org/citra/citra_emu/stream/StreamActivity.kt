// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

package org.citra.citra_emu.stream

import android.app.Activity
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import java.io.File
import java.util.Locale

/**
 * The phone's screen while a game streams to the console: statistics only. Keeping it open matters: Android applies
 * the Wi-Fi low-latency lock and foreground priority only while the app is in front with the screen on, so the screen
 * is kept on here.
 */
class StreamActivity : Activity() {
    private lateinit var text: TextView
    private val handler = Handler(Looper.getMainLooper())
    private val tick = object : Runnable {
        override fun run() {
            text.text = describe()
            handler.postDelayed(this, 1_000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val pad = (24 * resources.displayMetrics.density).toInt()
        text = TextView(this).apply {
            setTextColor(Color.rgb(0xd6, 0xe0, 0xe5))
            typeface = Typeface.MONOSPACE
            textSize = 16f
        }
        val stop = Button(this).apply {
            setText("Stop streaming")
            setOnClickListener {
                startService(Intent(this@StreamActivity, StreamService::class.java).setAction(StreamService.ACTION_STOP))
                finish()
            }
        }
        setContentView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.TOP
            setBackgroundColor(Color.rgb(0x10, 0x18, 0x20))
            setPadding(pad, pad * 2, pad, pad)
            addView(TextView(this@StreamActivity).apply {
                setText("Azahar Stream")
                setTextColor(Color.rgb(0x81, 0xef, 0xcc))
                textSize = 24f
                typeface = Typeface.DEFAULT_BOLD
            })
            addView(text)
            addView(stop)
        })
    }

    override fun onResume() {
        super.onResume()
        handler.post(tick)
    }

    override fun onPause() {
        handler.removeCallbacks(tick)
        super.onPause()
    }

    private fun describe(): String {
        val s = StreamStatus
        val lines = mutableListOf<String>()
        lines += ""
        lines += s.state
        if (!s.active) return lines.joinToString("\n")
        lines += "Console   ${s.console}"
        s.game?.let { lines += "Game      $it" }
        s.perf?.let { p ->
            val stream = p.optJSONObject("stream")
            lines += ""
            lines += "FPS       %.1f  (game %.1f)".format(Locale.US, p.optDouble("system_fps"), p.optDouble("game_fps"))
            lines += "Speed     %d%%".format(Locale.US, Math.round(p.optDouble("speed") * 100))
            lines += "Frame     %.1f ms  GPU %.1f  swap %.1f".format(
                Locale.US, p.optDouble("frame_ms"), p.optDouble("gpu_ms"), p.optDouble("swap_ms")
            )
            lines += "Video     %.1f Mbit/s".format(Locale.US, p.optDouble("mbps"))
            if (stream != null) {
                lines += "Resent    ${stream.optLong("nacks")}   key frames ${stream.optLong("key_requests")}"
                lines += "Input     ${stream.optLong("inputs")} packets"
            }
        }
        lines += ""
        lines += "Phone     ${shellTemperature()}"
        lines += "CPU caps  ${cpuCaps()}"
        lines += ""
        lines += "Keep this screen open while playing:"
        lines += "it keeps Wi-Fi in low-latency mode."
        return lines.joinToString("\n")
    }

    /** Back of the case, which is what the phone's thermal limits follow (where readable). */
    private fun shellTemperature(): String {
        val zones = File("/sys/class/thermal").listFiles() ?: return "-"
        for (z in zones) {
            val type = runCatching { File(z, "type").readText().trim() }.getOrNull() ?: continue
            if (type == "shell_back") {
                val t = runCatching { File(z, "temp").readText().trim().toInt() }.getOrNull() ?: return "-"
                return "%.1f °C back".format(Locale.US, t / 1000.0)
            }
        }
        return "-"
    }

    /** Frequency ceilings of the CPU clusters: they drop when the phone throttles. */
    private fun cpuCaps(): String {
        val policies = File("/sys/devices/system/cpu/cpufreq").listFiles { f -> f.name.startsWith("policy") }
            ?: return "-"
        return policies.sortedBy { it.name }.joinToString(" / ") { p ->
            val max = runCatching { File(p, "scaling_max_freq").readText().trim().toLong() }.getOrNull()
            if (max == null) "-" else "%.1f".format(Locale.US, max / 1e6)
        } + " GHz"
    }
}
