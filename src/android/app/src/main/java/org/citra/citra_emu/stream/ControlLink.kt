// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

package org.citra.citra_emu.stream

import android.os.Build
import java.io.BufferedInputStream
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.net.SocketTimeoutException
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.model.Game
import org.citra.citra_emu.utils.DirectoryInitialization
import org.citra.citra_emu.utils.GameHelper
import org.citra.citra_emu.utils.Log
import org.citra.citra_emu.utils.TurboHelper
import org.json.JSONArray
import org.json.JSONObject

/**
 * Control connection to the console's azahar-stream app (the console listens, we connect).
 * One JSON object per line; tiles follow their header line as raw bytes.
 * See magic-stream docs/TODO.md §5.2.
 */
class ControlLink(
    private val console: String,
    private val tcpPort: Int,
    private val udpPort: Int,
    private val key: String,
    private val onGame: (String?) -> Unit
) {
    private var socket: Socket? = null
    private lateinit var out: OutputStream
    private var games = listOf<Game>()
    private val tiles = HashMap<String, ByteArray>()
    private var emulation: Thread? = null
    @Volatile private var closed = false

    fun close() {
        closed = true
        runCatching { socket?.close() }
    }

    fun run() {
        try {
            val s = connect() ?: return Log.error("[Stream] console $console:$tcpPort unreachable")
            socket = s
            out = s.getOutputStream()
            val input = BufferedInputStream(s.getInputStream())
            val localUdp = NativeLibrary.streamOpen(console, udpPort)
            if (localUdp < 0) {
                return send(JSONObject().put("event", "error").put("message", "udp"))
            }
            send(
                JSONObject().put(
                    "hello",
                    JSONObject().put("version", 1).put("key", key).put("udp", localUdp)
                        .put("device", Build.MODEL)
                )
            )
            sendGames()
            // The console sends statistics every 10 s; silence means it is gone.
            s.soTimeout = 15_000
            while (!closed) {
                val line = readLine(input) ?: break
                handle(JSONObject(line))
            }
        } catch (_: SocketTimeoutException) {
            Log.warning("[Stream] console silent for 15 s")
        } catch (e: Exception) {
            if (!closed) Log.warning("[Stream] control connection: $e")
        } finally {
            stopGame()
            NativeLibrary.streamClose()
            runCatching { socket?.close() }
        }
    }

    private fun connect(): Socket? {
        repeat(20) {
            if (closed) return null
            try {
                return Socket().apply {
                    tcpNoDelay = true
                    connect(InetSocketAddress(console, tcpPort), 2_000)
                }
            } catch (_: IOException) {
                Thread.sleep(250)
            }
        }
        return null
    }

    private fun sendGames() {
        if (!DirectoryInitialization.areCitraDirectoriesReady()) {
            DirectoryInitialization.start()
        }
        games = runCatching { GameHelper.getGames() }.getOrElse {
            Log.error("[Stream] game list: $it")
            emptyList()
        }.filter { it.valid && !it.isSystemTitle }.sortedBy { it.title.lowercase() }
        val list = JSONArray()
        for (game in games) {
            val id = StreamTiles.id(game)
            val tile = StreamTiles.draw(game)
            tiles[id] = tile
            list.put(
                JSONObject().put("id", id).put("rev", StreamTiles.rev(tile)).put("installed", true)
                    .put("title", game.title)
            )
        }
        send(JSONObject().put("games", list))
    }

    private fun handle(msg: JSONObject) {
        when (msg.optString("op")) {
            "tiles" -> sendTiles(msg.optJSONArray("ids") ?: JSONArray())
            "start" -> startGame(msg.optString("id"), msg.optInt("bitrate", 12_000_000))
            "stop" -> {
                stopGame()
                send(JSONObject().put("event", "stopped"))
            }
            "save" -> slot(msg) { NativeLibrary.saveState(it); "saved" }
            "load" -> slot(msg) { NativeLibrary.loadState(it); "loaded" }
            else -> if (msg.has("stats")) answerStats()
        }
    }

    /** The console's statistics tick: answer with the emulator's own counters and the stream's. */
    private fun answerStats() {
        if (emulation == null) return
        val p = NativeLibrary.getPerfStats()
        if (p.size < 9) return
        // Rounded as numbers: String.format would use the phone's locale (a decimal comma).
        fun r(v: Double, digits: Int) = Math.round(v * digits) / digits.toDouble()
        val perf = JSONObject()
            .put("system_fps", r(p[0], 10))
            .put("game_fps", r(p[1], 10))
            .put("speed", r(p[2], 100))
            .put("frame_ms", r(p[3] * 1000, 100))
            .put("gpu_ms", r(p[6] * 1000, 100))
            .put("swap_ms", r(p[7] * 1000, 100))
            .put("stream", JSONObject(NativeLibrary.streamStats()))
        Log.info("[Stream] perf $perf")
        send(JSONObject().put("event", "perf").put("perf", perf))
    }

    private fun slot(msg: JSONObject, action: (Int) -> String) {
        val n = msg.optInt("slot", 1)
        if (n !in 1..3 || emulation == null) {
            return send(JSONObject().put("event", "error").put("message", "no game"))
        }
        send(JSONObject().put("event", action(n)).put("slot", n))
    }

    private fun sendTiles(ids: JSONArray) {
        synchronized(this) {
            for (i in 0 until ids.length()) {
                val id = ids.getString(i)
                val tile = tiles[id]
                val head = JSONObject().put("id", id).put("bytes", tile?.size ?: 0)
                if (tile != null) head.put("rev", StreamTiles.rev(tile))
                out.write((head.toString() + "\n").toByteArray())
                if (tile != null) out.write(tile)
            }
            out.write("{\"end\":true}\n".toByteArray())
            out.flush()
        }
    }

    private fun startGame(id: String, bitrate: Int) {
        val game = games.firstOrNull { StreamTiles.id(it) == id }
            ?: return send(JSONObject().put("event", "error").put("message", "unknown game"))
        if (emulation != null || NativeLibrary.isRunning()) {
            return send(JSONObject().put("event", "error").put("message", "busy"))
        }
        if (!NativeLibrary.streamStartVideo(WIDTH, HEIGHT, bitrate)) {
            return send(JSONObject().put("event", "error").put("message", "encoder"))
        }
        TurboHelper.setTurboEnabled(false, false)
        emulation = Thread({
            Log.info("[Stream] starting ${game.title}")
            NativeLibrary.run(game.path)
            Log.info("[Stream] emulation ended")
        }, "NativeEmulation").apply { start() }
        onGame(game.title)
        val layout = JSONObject()
            .put("display", JSONArray(listOf(WIDTH, HEIGHT)))
            .put("top", JSONArray(listOf(0, 0, 800, 480)))
            .put("bottom_mode", "screen")
            .put("bottom", JSONArray(listOf(80, 480, 640, 480)))
        send(JSONObject().put("event", "started").put("layout", layout))
    }

    private fun stopGame() {
        val thread = emulation ?: return
        NativeLibrary.stopEmulation()
        thread.join(10_000)
        NativeLibrary.streamStopVideo()
        emulation = null
        onGame(null)
    }

    private fun send(o: JSONObject) {
        synchronized(this) {
            runCatching {
                out.write((o.toString() + "\n").toByteArray())
                out.flush()
            }
        }
    }

    companion object {
        const val WIDTH = 800
        const val HEIGHT = 960

        /** Reads one '\n'-terminated line without buffering past it. */
        fun readLine(input: InputStream, max: Int = 8192): String? {
            val buf = ByteArrayOutputStream()
            while (true) {
                val b = input.read()
                if (b < 0) return if (buf.size() > 0) buf.toString("UTF-8") else null
                if (b == '\n'.code) return buf.toString("UTF-8").trimEnd('\r')
                if (buf.size() >= max) throw IOException("line too long")
                buf.write(b)
            }
        }
    }
}
