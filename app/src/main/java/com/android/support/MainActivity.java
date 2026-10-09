package com.android.support;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

public class MainActivity extends Activity {

    public String GameActivity = "com.sybogames.chili.multidex.ChiliMultidexSupportActivity";
    public boolean hasLaunched = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // ============================================================
        // ★ STEP 1: Install crash handler BEFORE anything else
        // ============================================================
        try {
            CrashHandler.init(getApplicationContext(), false);
        } catch (Throwable ignored) { }

        // Silent runtime gate
        if (!SecurityNative.preload(this, BuildConfig.DEBUG)) {
            android.os.Process.killProcess(android.os.Process.myPid());
            return;
        }

        // ============================================================
        // ★ STEP 2: Launch game activity
        // ============================================================
        if (!hasLaunched) {
            hasLaunched = true;
            try {
                startActivity(new Intent(MainActivity.this,
                        Class.forName(GameActivity)));
            } catch (ClassNotFoundException e) {
                // Game not found — silently continue
            }
        }

        // ============================================================
        // ★ STEP 3: Start the mod menu
        // ============================================================
        Main.Start(this);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        try {
            Main.onDestroy();
        } catch (Throwable ignored) {
        }
    }
}