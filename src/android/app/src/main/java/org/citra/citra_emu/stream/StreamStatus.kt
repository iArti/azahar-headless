// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

package org.citra.citra_emu.stream

import org.json.JSONObject

/** What the stream screen shows; written by ControlLink, read by StreamActivity. */
object StreamStatus {
    @Volatile var active = false
    @Volatile var console = ""
    @Volatile var device = ""
    @Volatile var game: String? = null
    @Volatile var state = "Waiting for the console"
    /** Latest one-second sample: emulator counters and stream totals (see ControlLink.sample). */
    @Volatile var perf: JSONObject? = null
    @Volatile var mbps = 0.0

    fun reset() {
        active = false
        game = null
        perf = null
        mbps = 0.0
        state = "Stopped"
    }
}
