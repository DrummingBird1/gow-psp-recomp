package org.gow.shell;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import java.io.File;

/** Lightweight native launcher shown before the SDL game activity. */
public final class LauncherActivity extends Activity {
    private static final int BG = Color.rgb(14, 13, 14);
    private static final int PANEL = Color.rgb(29, 26, 26);
    private static final int RED = Color.rgb(163, 32, 32);
    private static final int GOLD = Color.rgb(205, 169, 76);
    private static final int TEXT = Color.rgb(242, 237, 226);
    private static final int MUTED = Color.rgb(177, 169, 158);
    private TextView status;
    private TextView play;
    private Switch performanceToggle;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(BG);
        getWindow().setNavigationBarColor(BG);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LOW_PROFILE);
        buildUi();
        refreshStatus();
    }

    private GradientDrawable shape(int color, int stroke, int radius) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(radius * getResources().getDisplayMetrics().density);
        if (stroke != 0) d.setStroke((int)(1 * getResources().getDisplayMetrics().density), stroke);
        return d;
    }

    private TextView text(String value, int size, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(value);
        t.setTextSize(size);
        t.setTextColor(color);
        t.setGravity(Gravity.CENTER);
        t.setTypeface(bold ? Typeface.create("serif", Typeface.BOLD) : Typeface.create("sans-serif", Typeface.NORMAL));
        return t;
    }

    private void buildUi() {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(BG);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        int pad = (int)(24 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad, pad, pad);
        scroll.addView(root);
        setContentView(scroll);

        TextView crest = text("G O W", 17, GOLD, true);
        crest.setLetterSpacing(0.18f);
        root.addView(crest, matchWrap());
        TextView title = text("GOD OF WAR", 30, TEXT, true);
        title.setPadding(0, pad / 2, 0, 0);
        root.addView(title, matchWrap());
        TextView subtitle = text("PSP RECOMP", 13, GOLD, true);
        subtitle.setLetterSpacing(0.24f);
        subtitle.setPadding(0, 2, 0, pad);
        root.addView(subtitle, matchWrap());

        View divider = new View(this);
        divider.setBackgroundColor(RED);
        LinearLayout.LayoutParams line = new LinearLayout.LayoutParams((int)(88 * getResources().getDisplayMetrics().density), (int)(2 * getResources().getDisplayMetrics().density));
        line.bottomMargin = pad;
        root.addView(divider, line);

        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(pad / 2, pad / 2, pad / 2, pad / 2);
        card.setBackground(shape(PANEL, 0xFF44372A, 12));
        TextView heading = text("A JORNADA COMEÇA", 16, TEXT, true);
        card.addView(heading, matchWrap());
        TextView description = text("Abra sua cópia do jogo e continue de onde a lenda começa.", 14, MUTED, false);
        description.setPadding(pad / 3, pad / 2, pad / 3, pad / 2);
        card.addView(description, matchWrap());
        status = text("", 12, GOLD, false);
        status.setPadding(0, pad / 3, 0, 0);
        card.addView(status, matchWrap());
        root.addView(card, matchWidthWithBottom(pad));

        LinearLayout performance = new LinearLayout(this);
        performance.setOrientation(LinearLayout.VERTICAL);
        performance.setPadding(pad / 2, pad / 2, pad / 2, pad / 2);
        performance.setBackground(shape(PANEL, 0xFF44372A, 12));
        TextView performanceTitle = text("DESEMPENHO", 16, TEXT, true);
        performance.addView(performanceTitle, matchWrap());
        TextView performanceDescription = text(
            "Exibe o painel de diagnóstico durante o jogo. As métricas dependem do suporte do runtime.",
            12, MUTED, false);
        performanceDescription.setPadding(0, pad / 4, 0, pad / 4);
        performance.addView(performanceDescription, matchWrap());
        performanceToggle = new Switch(this);
        performanceToggle.setText("Mostrar HUD durante o jogo");
        performanceToggle.setTextColor(TEXT);
        performanceToggle.setButtonTintList(android.content.res.ColorStateList.valueOf(GOLD));
        performanceToggle.setChecked(getPreferences(MODE_PRIVATE).getBoolean("performance_hud_enabled", false));
        performanceToggle.setOnCheckedChangeListener((button, checked) ->
            getPreferences(MODE_PRIVATE).edit().putBoolean("performance_hud_enabled", checked).apply());
        performance.addView(performanceToggle, matchWrap());
        root.addView(performance, matchWidthWithBottom(pad));

        play = text("JOGAR", 18, Color.WHITE, true);
        play.setLetterSpacing(0.15f);
        play.setBackground(shape(RED, GOLD, 10));
        play.setPadding(pad, pad / 2, pad, pad / 2);
        play.setOnClickListener(v -> launchGame());
        root.addView(play, matchWidthWithBottom(pad));

        TextView note = text("A ISO e os arquivos do jogo não acompanham o aplicativo.", 11, MUTED, false);
        root.addView(note, matchWrap());
        TextView version = text("ANDROID • ARM64", 10, 0xFF6F6860, true);
        version.setPadding(0, pad, 0, 0);
        root.addView(version, matchWrap());
    }

    private LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
    }

    private LinearLayout.LayoutParams matchWidthWithBottom(int bottom) {
        LinearLayout.LayoutParams p = matchWrap();
        p.bottomMargin = bottom;
        return p;
    }

    private File gameDir() {
        File dir = getExternalFilesDir(null);
        return dir == null ? getFilesDir() : dir;
    }

    private void refreshStatus() {
        File image = new File(gameDir(), "image.bin");
        File iso = new File(gameDir(), "game.iso");
        boolean imageOk = image.isFile() && image.length() > 0;
        boolean isoOk = iso.isFile() && iso.length() > 0;
        if (imageOk && isoOk) {
            status.setText("Arquivos principais encontrados.");
            status.setTextColor(0xFF9BCB91);
            play.setAlpha(1f);
        } else {
            status.setText("Pasta do app: Android/data/org.gow.shell/files\n" +
                (imageOk ? "✓ image.bin" : "• falta image.bin") + "   " +
                (isoOk ? "✓ game.iso" : "• falta game.iso"));
            status.setTextColor(GOLD);
            play.setAlpha(1f); // Allow launch so the runtime can write its diagnostic log.
        }
    }

    @Override protected void onResume() {
        super.onResume();
        if (status != null) refreshStatus();
    }

    private void launchGame() {
        startActivity(new Intent(this, GameActivity.class));
    }
}
