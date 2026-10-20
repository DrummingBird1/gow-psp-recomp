package org.gow.shell;

import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.TextView;

import org.libsdl.app.SDLActivity;

import java.io.RandomAccessFile;
import java.io.File;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** SDL host with an in-game FPS switch and lightweight diagnostics overlay. */
public final class GameActivity extends SDLActivity {
    private static final long HUD_REFRESH_MS = 1000;
    private static final Pattern PERF = Pattern.compile(
        "\\[gow-perf\\]\\s+fps=([0-9.]+)\\s+frame=([0-9]+)\\s+frame_ms=([0-9.]+)\\s+video_driver=([^\\s]+)\\s+gpu_backend=([^\\s]+)"
    );
    private final Handler hudHandler = new Handler(Looper.getMainLooper());
    private TextView hud;
    private TextView toggle;
    private FrameLayout overlay;
    private boolean hudEnabled;

    @Override protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        hudEnabled = getSharedPreferences("gow_settings", MODE_PRIVATE)
                .getBoolean("performance_hud_enabled", false);
        createOverlay();
        updateHudVisibility();
        if (hudEnabled) hudHandler.post(hudRefresh);
    }

    private void createOverlay() {
        overlay = new FrameLayout(this);
        overlay.setClipChildren(false);
        overlay.setClipToPadding(false);
        FrameLayout.LayoutParams full = new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        addContentView(overlay, full);
        overlay.bringToFront();

        toggle = new TextView(this);
        toggle.setText("FPS");
        toggle.setTextColor(Color.rgb(245, 239, 224));
        toggle.setTextSize(11);
        toggle.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        toggle.setGravity(Gravity.CENTER);
        toggle.setPadding(dp(12), dp(8), dp(12), dp(8));
        toggle.setBackground(chipBackground());
        toggle.setClickable(true);
        toggle.setContentDescription("Ativar ou desativar HUD de FPS");
        toggle.setOnClickListener(v -> {
            hudEnabled = !hudEnabled;
            getSharedPreferences("gow_settings", MODE_PRIVATE).edit()
                    .putBoolean("performance_hud_enabled", hudEnabled).apply();
            updateHudVisibility();
            hudHandler.removeCallbacks(hudRefresh);
            if (hudEnabled) hudHandler.post(hudRefresh);
        });
        FrameLayout.LayoutParams toggleLp = new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.TOP | Gravity.END);
        toggleLp.setMargins(dp(8), dp(28), dp(8), dp(8));
        overlay.addView(toggle, toggleLp);

        hud = new TextView(this);
        hud.setText("GOW PERF\nLendo métricas...");
        hud.setTextColor(Color.rgb(245, 239, 224));
        hud.setTextSize(11);
        hud.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        hud.setPadding(dp(10), dp(7), dp(10), dp(7));
        hud.setBackground(chipBackground());
        hud.setClickable(false);
        FrameLayout.LayoutParams hudLp = new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.TOP | Gravity.START);
        hudLp.setMargins(dp(8), dp(28), dp(8), dp(8));
        overlay.addView(hud, hudLp);
    }

    private GradientDrawable chipBackground() {
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xE61A1718);
        bg.setCornerRadius(dp(7));
        bg.setStroke(dp(1), Color.rgb(163, 32, 32));
        return bg;
    }

    private void updateHudVisibility() {
        if (hud != null) hud.setVisibility(hudEnabled ? View.VISIBLE : View.GONE);
        if (toggle != null) {
            toggle.setText(hudEnabled ? "FPS  ON" : "FPS  OFF");
            toggle.setAlpha(hudEnabled ? 1f : 0.78f);
        }
    }

    private int dp(int value) {
        return (int)(value * getResources().getDisplayMetrics().density + 0.5f);
    }

    private final Runnable hudRefresh = new Runnable() {
        @Override public void run() {
            if (hud != null && hudEnabled) {
                String line = readLatestPerfLine();
                if (line == null) {
                    hud.setText("GOW PERF\nFPS nativo indisponível nesta build");
                } else {
                    Matcher m = PERF.matcher(line);
                    if (m.find()) {
                        hud.setText(String.format(java.util.Locale.US,
                            "GOW PERF  %.1f FPS\n%.2f ms/frame  •  frame %s\nVídeo: %s  •  GPU: %s",
                            Double.parseDouble(m.group(1)), Double.parseDouble(m.group(3)),
                            m.group(2), m.group(4), m.group(5)));
                    } else {
                        hud.setText("GOW PERF\nAguardando amostra nativa...");
                    }
                }
                hudHandler.postDelayed(this, HUD_REFRESH_MS);
            }
        }
    };

    /** Read only the tail of the same app-private external log used by rt_android.c. */
    private String readLatestPerfLine() {
        File dir = getExternalFilesDir(null);
        if (dir == null) dir = getFilesDir();
        File log = new File(dir, "log.txt");
        if (!log.isFile() || log.length() == 0) return null;
        try (RandomAccessFile raf = new RandomAccessFile(log, "r")) {
            long start = Math.max(0, raf.length() - 16384);
            raf.seek(start);
            if (start > 0) raf.readLine();
            String line;
            String latest = null;
            while ((line = raf.readLine()) != null) {
                if (line.contains("[gow-perf]")) latest = line;
            }
            return latest;
        } catch (Exception ignored) {
            return null;
        }
    }

    @Override protected void onDestroy() {
        hudHandler.removeCallbacks(hudRefresh);
        hud = null;
        toggle = null;
        overlay = null;
        super.onDestroy();
    }
}
