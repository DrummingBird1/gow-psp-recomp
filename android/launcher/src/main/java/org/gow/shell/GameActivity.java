package org.gow.shell;

import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.TextView;

import org.libsdl.app.SDLActivity;

/** SDL host for the native recompilation runtime. */
public final class GameActivity extends SDLActivity {
    @Override protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (getPreferences(MODE_PRIVATE).getBoolean("performance_hud_enabled", false)) {
            showPerformanceHud();
        }
    }

    private void showPerformanceHud() {
        float density = getResources().getDisplayMetrics().density;
        TextView hud = new TextView(this);
        hud.setText("GOW • DESEMPENHO\nHUD ATIVO\nMétricas nativas: integração pendente");
        hud.setTextColor(Color.rgb(242, 237, 226));
        hud.setTextSize(11);
        hud.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        hud.setPadding((int)(10 * density), (int)(8 * density),
                (int)(10 * density), (int)(8 * density));
        GradientDrawable panel = new GradientDrawable();
        panel.setColor(0xD90E0D0E);
        panel.setCornerRadius(8 * density);
        panel.setStroke(Math.max(1, (int)density), Color.rgb(163, 32, 32));
        hud.setBackground(panel);
        hud.setClickable(false);
        hud.setFocusable(false);

        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.START);
        params.setMargins((int)(10 * density), (int)(28 * density), 0, 0);
        addContentView(hud, params);
        hud.setVisibility(View.VISIBLE);
    }
}
