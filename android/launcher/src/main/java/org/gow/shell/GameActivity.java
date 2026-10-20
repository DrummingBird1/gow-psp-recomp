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

/** SDL host for the native recompilation runtime, with a lightweight diagnostics overlay. */
public final class GameActivity extends SDLActivity {
    private static final long HUD_REFRESH_MS = 1000;
    private static final Pattern PERF = Pattern.compile(
        "\\[gow-perf\\]\\s+fps=([0-9.]+)\\s+frame=([0-9]+)\\s+frame_ms=([0-9.]+)\\s+video_driver=([^\\s]+)\\s+gpu_backend=([^\\s]+)"
    );
    private final Handler hudHandler = new Handler(Looper.getMainLooper());
    private TextView hud;
    private boolean hudVisible = true;

    @Override protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        boolean enabled = getSharedPreferences("gow_settings", MODE_PRIVATE)
                .getBoolean("performance_hud_enabled", false);
        if (enabled) {
            createHud();
            hudHandler.post(hudRefresh);
        }
    }

    private void createHud() {
        hud = new TextView(this);
        hud.setText("GOW PERF  •  lendo métricas...");
        hud.setTextColor(Color.rgb(245, 239, 224));
        hud.setTextSize(11);
        hud.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        hud.setPadding(dp(10), dp(7), dp(10), dp(7));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xD91A1718);
        bg.setCornerRadius(dp(7));
        bg.setStroke(dp(1), Color.rgb(163, 32, 32));
        hud.setBackground(bg);
        hud.setClickable(true);
        hud.setOnClickListener(v -> {
            hudVisible = !hudVisible;
            hud.setVisibility(hudVisible ? View.VISIBLE : View.GONE);
        });
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.TOP | Gravity.START
        );
        lp.setMargins(dp(8), dp(28), dp(8), dp(8));
        addContentView(hud, lp);
    }

    private int dp(int value) {
        return (int)(value * getResources().getDisplayMetrics().density + 0.5f);
    }

    private final Runnable hudRefresh = new Runnable() {
        @Override public void run() {
            if (hud != null) {
                String line = readLatestPerfLine();
                if (line == null) {
                    hud.setText("GOW PERF\nSem métricas nativas nesta build");
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

    /** Read only the tail of the app log to keep polling cheap during gameplay. */
    private String readLatestPerfLine() {
        File dir = getExternalFilesDir(null);
        if (dir == null) dir = getFilesDir();
        File log = new File(dir, "log.txt");
        if (!log.isFile() || log.length() == 0) return null;
        try (RandomAccessFile raf = new RandomAccessFile(log, "r")) {
            long start = Math.max(0, raf.length() - 16384);
            raf.seek(start);
            if (start > 0) raf.readLine(); // discard a possibly partial first line
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
        super.onDestroy();
    }
}
