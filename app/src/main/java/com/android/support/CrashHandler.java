package com.android.support;

import android.content.Context;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.lang.Thread.UncaughtExceptionHandler;
import java.text.SimpleDateFormat;
import java.util.Date;

/**
 * CrashHandler — robust crash logging for Unity/IL2CPP mods.
 *
 * Captures:
 *   1. Java uncaught exceptions
 *   2. Native SIGSEGV/SIGABRT/etc (via native handler)
 *   3. Last 500 lines of logcat at crash time
 *
 * Logs go to:
 *   /storage/emulated/0/Documents/  (user-visible)
 *   /data/data/<pkg>/files/         (always works)
 */
public final class CrashHandler {

    private static final String TAG = "ModXCrash";
    public static final UncaughtExceptionHandler DEFAULT_HANDLER =
            Thread.getDefaultUncaughtExceptionHandler();

    private static Context sAppCtx = null;
    private static volatile boolean sInstalled = false;

    // Native crash handler registration
    private static native void nativeInstallCrashHandler(String logDir);

    // ================================================================
    //  Install — call VERY EARLY (before game activity launches)
    // ================================================================
    public static void init(final Context app, final boolean overlayRequired) {
        if (sInstalled) return;
        sInstalled = true;
        sAppCtx = app.getApplicationContext();

        // --- 1. Java crash handler ---
        Thread.setDefaultUncaughtExceptionHandler(new UncaughtExceptionHandler() {
            @Override
            public void uncaughtException(Thread thread, Throwable throwable) {
                handleJavaCrash(thread, throwable);
            }
        });

        // --- 2. Native crash handler (SIGSEGV etc.) ---
        try {
            String nativeLogDir = resolveLogDir(app);
            if (nativeLogDir != null) {
                nativeInstallCrashHandler(nativeLogDir);
                Log.i(TAG, "Native crash handler installed: " + nativeLogDir);
            }
        } catch (Throwable t) {
            Log.e(TAG, "Native handler install failed: " + t);
        }
    }

    // ================================================================
    //  Java crash handling
    // ================================================================
    private static void handleJavaCrash(Thread thread, Throwable throwable) {
        Log.e(TAG, "=== JAVA CRASH ===");

        try {
            String log = buildJavaCrashReport(thread, throwable);
            writeCrashLog("java_crash", log);
        } catch (Throwable ignored) { }

        // Delegate to default handler so process exits normally
        if (DEFAULT_HANDLER != null) {
            DEFAULT_HANDLER.uncaughtException(thread, throwable);
        } else {
            System.exit(2);
        }
    }

    // ================================================================
    //  Called from native code on SIGSEGV/SIGABRT/etc.
    // ================================================================
    public static void onNativeCrash(String nativeInfo) {
        try {
            String log = buildNativeCrashReport(nativeInfo);
            writeCrashLog("native_crash", log);
        } catch (Throwable ignored) { }
    }

    // ================================================================
    //  Report builders
    // ================================================================
    private static String buildJavaCrashReport(Thread thread, Throwable throwable) {
        StringBuilder sb = new StringBuilder(4096);

        sb.append("========== JAVA CRASH REPORT ==========\n");
        appendDeviceInfo(sb);
        sb.append("Thread        : ").append(thread != null ? thread.getName() : "?").append("\n");
        sb.append("=======================================\n\n");

        if (throwable != null) {
            StringWriter sw = new StringWriter();
            PrintWriter pw = new PrintWriter(sw);
            throwable.printStackTrace(pw);
            pw.flush();
            sb.append(sw.toString());
            pw.close();
        }

        sb.append("\n\n========== LAST LOGCAT (500 lines) ==========\n");
        sb.append(readLogcat(500));
        return sb.toString();
    }

    private static String buildNativeCrashReport(String nativeInfo) {
        StringBuilder sb = new StringBuilder(4096);

        sb.append("========== NATIVE CRASH REPORT ==========\n");
        appendDeviceInfo(sb);
        sb.append("==========================================\n\n");

        if (nativeInfo != null) {
            sb.append(nativeInfo).append("\n\n");
        }

        sb.append("========== LAST LOGCAT (500 lines) ==========\n");
        sb.append(readLogcat(500));
        return sb.toString();
    }

    private static void appendDeviceInfo(StringBuilder sb) {
        sb.append("Time              : ")
          .append(new SimpleDateFormat("yyyy-MM-dd HH:mm:ss").format(new Date())).append("\n");
        sb.append("Manufacturer      : ").append(Build.MANUFACTURER).append("\n");
        sb.append("Model             : ").append(Build.MODEL).append("\n");
        sb.append("Android Release   : ").append(Build.VERSION.RELEASE).append("\n");
        sb.append("Android SDK       : ").append(Build.VERSION.SDK_INT).append("\n");
        sb.append("ABI               : ").append(Build.SUPPORTED_ABIS[0]).append("\n");
        if (sAppCtx != null) {
            sb.append("Package           : ").append(sAppCtx.getPackageName()).append("\n");
        }
        sb.append("Process           : ").append(getProcessName()).append("\n");
    }

    private static String getProcessName() {
        try {
            if (Build.VERSION.SDK_INT >= 28) {
                return android.app.Application.getProcessName();
            }
        } catch (Throwable ignored) { }
        return "?";
    }

    // ================================================================
    //  Logcat reader — captures last N lines
    // ================================================================
    private static String readLogcat(int maxLines) {
        StringBuilder sb = new StringBuilder(8192);
        Process proc = null;
        BufferedReader br = null;
        try {
            proc = Runtime.getRuntime().exec(new String[]{
                    "logcat", "-d", "-t", String.valueOf(maxLines), "-v", "threadtime"
            });
            br = new BufferedReader(new InputStreamReader(proc.getInputStream()));
            String line;
            while ((line = br.readLine()) != null) {
                sb.append(line).append('\n');
            }
        } catch (Throwable t) {
            sb.append("(logcat read failed: ").append(t).append(")\n");
        } finally {
            try { if (br != null) br.close(); } catch (Throwable ignored) { }
            try { if (proc != null) proc.destroy(); } catch (Throwable ignored) { }
        }
        return sb.toString();
    }

    // ================================================================
    //  File write — attempts multiple locations
    // ================================================================
    private static void writeCrashLog(String prefix, String content) {
        String time = new SimpleDateFormat("yyyyMMdd_HHmmss").format(new Date());
        String filename = prefix + "_" + time + ".txt";

        // Location 1: /storage/emulated/0/Documents  (user-visible)
        // Location 2: /data/data/<pkg>/files          (always works)
        // Location 3: /storage/emulated/0/Android/data/<pkg>/files

        String[] dirs = new String[]{
                "/storage/emulated/0/Documents",
                "/storage/emulated/0/Download",
                sAppCtx != null ? sAppCtx.getFilesDir().getAbsolutePath() : null,
                sAppCtx != null && sAppCtx.getExternalFilesDir(null) != null
                        ? sAppCtx.getExternalFilesDir(null).getAbsolutePath() : null
        };

        for (String dir : dirs) {
            if (dir == null) continue;
            try {
                File f = new File(dir, filename);
                File parent = f.getParentFile();
                if (parent != null && !parent.exists()) parent.mkdirs();
                FileOutputStream fos = new FileOutputStream(f);
                fos.write(content.getBytes("UTF-8"));
                fos.flush();
                fos.close();
                Log.e(TAG, "Crash log written: " + f.getAbsolutePath());
                // success — done
                return;
            } catch (Throwable t) {
                Log.w(TAG, "Failed to write to " + dir + ": " + t);
            }
        }
        Log.e(TAG, "Failed to write crash log to any location!");
    }

    // ================================================================
    //  Helper for native side
    // ================================================================
    private static String resolveLogDir(Context ctx) {
        try {
            File f = new File("/storage/emulated/0/Documents");
            if (f.exists() || f.mkdirs()) return f.getAbsolutePath();
        } catch (Throwable ignored) { }

        try {
            File f = ctx.getExternalFilesDir(null);
            if (f != null) return f.getAbsolutePath();
        } catch (Throwable ignored) { }

        try {
            return ctx.getFilesDir().getAbsolutePath();
        } catch (Throwable ignored) { }

        return null;
    }

    private CrashHandler() { }
}