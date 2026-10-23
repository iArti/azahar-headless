// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

package org.citra.citra_emu.stream

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.wifi.WifiManager
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import org.citra.citra_emu.R
import org.citra.citra_emu.utils.Log

/**
 * Runs a game for the Magic Stream console app without any activity on the phone screen: the
 * renderer draws into a video encoder, the frames go to the console over UDP and its controls
 * come back. Started by the Magic Stream companion from the shell:
 *   am start-foreground-service -n <package>/org.citra.citra_emu.stream.StreamService \
 *     --es console <ip> --ei tcp <port> --ei udp <port> --es key <16 hex>
 */
class StreamService : Service() {
    private var link: ControlLink? = null
    private var worker: Thread? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        val console = intent?.getStringExtra("console")
        val tcp = intent?.getIntExtra("tcp", 0) ?: 0
        val udp = intent?.getIntExtra("udp", 0) ?: 0
        val key = intent?.getStringExtra("key") ?: ""
        startInForeground(null)
        if (console == null || tcp !in 1..65535 || udp !in 1..65535) {
            Log.error("[Stream] missing console address")
            stopSelf()
            return START_NOT_STICKY
        }
        // One console at a time: a new start replaces the running session.
        link?.close()
        worker?.join(15_000)
        holdLocks()
        val l = ControlLink(console, tcp, udp, key) { title -> startInForeground(title) }
        link = l
        worker = Thread({
            l.run()
            stopSelf()
        }, "StreamControl").apply { start() }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        link?.close()
        worker?.join(15_000)
        wakeLock?.takeIf { it.isHeld }?.release()
        wifiLock?.takeIf { it.isHeld }?.release()
        super.onDestroy()
    }

    private fun holdLocks() {
        if (wakeLock == null) {
            wakeLock = getSystemService(PowerManager::class.java)
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "azahar:stream")
                .apply { acquire() }
        }
        if (wifiLock == null) {
            @Suppress("DEPRECATION")
            val mode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                WifiManager.WIFI_MODE_FULL_LOW_LATENCY
            } else {
                WifiManager.WIFI_MODE_FULL_HIGH_PERF
            }
            wifiLock = getSystemService(WifiManager::class.java)
                .createWifiLock(mode, "azahar:stream").apply { acquire() }
        }
    }

    private fun startInForeground(game: String?) {
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(
            NotificationChannel(CHANNEL, "Magic Stream", NotificationManager.IMPORTANCE_LOW)
        )
        val stop = PendingIntent.getService(
            this, 0, Intent(this, StreamService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE
        )
        val screen = PendingIntent.getActivity(
            this, 1, Intent(this, StreamActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
            PendingIntent.FLAG_IMMUTABLE
        )
        val notification: Notification = NotificationCompat.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_notification_logo)
            .setContentTitle(if (game != null) "Streaming to the console" else "Waiting for the console")
            .setContentText(game ?: "Magic Stream")
            .setOngoing(true)
            .setContentIntent(screen)
            .addAction(0, "Stop", stop)
            .build()
        ServiceCompat.startForeground(
            this, NOTIFICATION_ID, notification,
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE
            } else {
                0
            }
        )
    }

    companion object {
        private const val CHANNEL = "magic_stream"
        private const val NOTIFICATION_ID = 0x4d53
        const val ACTION_STOP = "org.citra.citra_emu.stream.STOP"
    }
}
