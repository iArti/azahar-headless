// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

package org.citra.citra_emu.stream

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import android.text.TextPaint
import android.text.TextUtils
import java.nio.IntBuffer
import java.security.MessageDigest
import org.citra.citra_emu.model.Game

/**
 * Menu tiles for the console, which has neither an image decoder nor fonts: a 96x96 RGBA icon
 * (premultiplied, R,G,B,A byte order) followed by a 200x28 A8 label mask. Same format as the
 * Magic Stream companion (docs/TODO.md §1.3 in magic-stream).
 */
object StreamTiles {
    const val ICON = 96
    const val LABEL_W = 200
    const val LABEL_H = 28
    const val BYTES = ICON * ICON * 4 + LABEL_W * LABEL_H

    fun id(game: Game): String = hex16(game.path.toByteArray())

    fun rev(tile: ByteArray): String = hex16(tile)

    fun draw(game: Game): ByteArray {
        val out = ByteArray(BYTES)
        icon(game.icon)?.let { bitmap ->
            val pixels = IntArray(ICON * ICON)
            bitmap.getPixels(pixels, 0, ICON, 0, 0, ICON, ICON)
            for (i in pixels.indices) {
                // SMDH icons are opaque, so premultiplying changes nothing; it is done anyway.
                val p = pixels[i]
                val a = Color.alpha(p)
                out[i * 4] = (Color.red(p) * a / 255).toByte()
                out[i * 4 + 1] = (Color.green(p) * a / 255).toByte()
                out[i * 4 + 2] = (Color.blue(p) * a / 255).toByte()
                out[i * 4 + 3] = a.toByte()
            }
        }
        label(game.title).copyInto(out, ICON * ICON * 4)
        return out
    }

    /** SMDH large icon: 48x48 RGB565, two pixels per int, scaled up 2x by nearest neighbour. */
    private fun icon(vector: IntArray?): Bitmap? {
        if (vector == null || vector.size < 48 * 48 / 2) {
            return null
        }
        val small = Bitmap.createBitmap(48, 48, Bitmap.Config.RGB_565)
        small.copyPixelsFromBuffer(IntBuffer.wrap(vector))
        return Bitmap.createScaledBitmap(small, ICON, ICON, false)
    }

    private fun label(title: String): ByteArray {
        val bitmap = Bitmap.createBitmap(LABEL_W, LABEL_H, Bitmap.Config.ARGB_8888)
        val paint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            textSize = 20f
            typeface = Typeface.DEFAULT_BOLD
            textAlign = Paint.Align.CENTER
        }
        val text = TextUtils.ellipsize(title, paint, 196f, TextUtils.TruncateAt.END).toString()
        Canvas(bitmap).drawText(text, LABEL_W / 2f, 21f, paint)
        val pixels = IntArray(LABEL_W * LABEL_H)
        bitmap.getPixels(pixels, 0, LABEL_W, 0, 0, LABEL_W, LABEL_H)
        return ByteArray(pixels.size) { Color.alpha(pixels[it]).toByte() }
    }

    private fun hex16(bytes: ByteArray): String =
        MessageDigest.getInstance("SHA-256").digest(bytes).take(8)
            .joinToString("") { "%02x".format(it) }
}
