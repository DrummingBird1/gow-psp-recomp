package org.gow.shell;

import android.app.Activity;
import android.app.AlertDialog;
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
import java.io.BufferedReader;
import java.io.FileReader;

/**
 * God of War PSP Recomp Android launcher.
 * Inspired by the navigation and diagnostics layout of the MW2 Android fork,
 * but only exposes settings that are actually implemented by this runtime.
 */
public final class LauncherActivity extends Activity {
    private static final int BG = Color.rgb(12, 11, 12);
    private static final int PANEL = Color.rgb(28, 25, 25);
    private static final int RED = Color.rgb(163, 32, 32);
    private static final int GOLD = Color.rgb(205, 169, 76);
    private static final int TEXT = Color.rgb(242, 237, 226);
    private static final int MUTED = Color.rgb(177, 169, 158);
    private TextView status;
    private TextView play;
    private Switch performanceToggle;
    private LinearLayout root;
    private int pad;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(BG);
        getWindow().setNavigationBarColor(BG);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LOW_PROFILE);
        pad = (int)(20 * getResources().getDisplayMetrics().density);
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

    private LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
    }

    private LinearLayout.LayoutParams withBottom(int bottom) {
        LinearLayout.LayoutParams p = matchWrap();
        p.bottomMargin = bottom;
        return p;
    }

    private void buildUi() {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(BG);
        root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setPadding(pad, pad, pad, pad);
        scroll.addView(root);
        setContentView(scroll);

        TextView crest = text("G O W  •  ANDROID", 12, GOLD, true);
        crest.setLetterSpacing(0.16f);
        root.addView(crest, matchWrap());

        TextView title = text("GOD OF WAR", 30, TEXT, true);
        title.setPadding(0, pad / 2, 0, 0);
        root.addView(title, matchWrap());

        TextView subtitle = text("CHAINS OF OLYMPUS  /  PSP RECOMP", 10, GOLD, true);
        subtitle.setLetterSpacing(0.08f);
        subtitle.setPadding(0, 3, 0, pad);
        root.addView(subtitle, matchWrap());

        View divider = new View(this);
        divider.setBackgroundColor(RED);
        LinearLayout.LayoutParams line = new LinearLayout.LayoutParams((int)(92 * getResources().getDisplayMetrics().density), dp(2));
        line.bottomMargin = pad;
        root.addView(divider, line);

        LinearLayout card = panel();
        addHeading(card, "A JORNADA COMEÇA");
        TextView description = text("Sua jornada começa aqui. Inicie o jogo ou ajuste as opções disponíveis.", 13, MUTED, false);
        description.setPadding(pad / 3, pad / 3, pad / 3, pad / 2);
        card.addView(description, matchWrap());
        status = text("", 11, GOLD, false);
        status.setPadding(0, pad / 4, 0, 0);
        card.addView(status, matchWrap());
        root.addView(card, withBottom(pad));

        play = text("▶   JOGAR", 18, Color.WHITE, true);
        play.setLetterSpacing(0.12f);
        play.setBackground(shape(RED, GOLD, 10));
        play.setPadding(pad, pad / 2, pad, pad / 2);
        play.setOnClickListener(v -> launchGame());
        root.addView(play, withBottom(pad));

        addMenuButton("DESEMPENHO  /  HUD", "FPS, tempo por quadro e informações do renderizador.");
        addMenuButton("ARQUIVOS DO JOGO", "Verifique a localização dos arquivos necessários.");
        addMenuButton("DIAGNÓSTICO E LOG", "Consulte o registro do runtime para investigar erros.");
        addMenuButton("SOBRE", "God of War PSP Recomp Android\nVersão experimental para Android ARM64.");

        TextView note = text("A ISO e os arquivos do jogo não acompanham o aplicativo.", 10, MUTED, false);
        note.setPadding(0, pad, 0, 0);
        root.addView(note, matchWrap());
        TextView version = text("ANDROID  •  ARM64", 10, 0xFF6F6860, true);
        version.setPadding(0, pad / 3, 0, 0);
        root.addView(version, matchWrap());
    }

    private LinearLayout panel() {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(pad / 2, pad / 2, pad / 2, pad / 2);
        card.setBackground(shape(PANEL, 0xFF44372A, 12));
        return card;
    }

    private void addHeading(LinearLayout parent, String label) {
        TextView heading = text(label, 15, TEXT, true);
        heading.setLetterSpacing(0.04f);
        parent.addView(heading, matchWrap());
    }

    private void addMenuButton(String label, String description) {
        TextView button = text(label + "   ›", 13, TEXT, true);
        button.setGravity(Gravity.CENTER_VERTICAL);
        button.setPadding(pad / 2, pad / 2, pad / 2, pad / 2);
        button.setBackground(shape(PANEL, 0xFF44372A, 8));
        button.setOnClickListener(v -> {
            if (label.startsWith("DESEMPENHO")) showPerformance();
            else if (label.startsWith("ARQUIVOS")) showFiles();
            else if (label.startsWith("DIAGNÓSTICO")) showLog();
            else new AlertDialog.Builder(this).setTitle("SOBRE").setMessage(description)
                    .setPositiveButton("OK", null).show();
        });
        root.addView(button, withBottom(dp(8)));
    }

    private void showPerformance() {
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        int inset = dp(8);
        content.setPadding(inset, inset, inset, inset);
        TextView note = new TextView(this);
        note.setText("Ative o painel durante o jogo. O FPS real depende de instrumentação nativa na biblioteca do jogo.");
        note.setTextColor(TEXT);
        note.setTextSize(14);
        content.addView(note, matchWrap());
        performanceToggle = new Switch(this);
        performanceToggle.setText("Mostrar HUD durante o jogo");
        performanceToggle.setTextColor(TEXT);
        performanceToggle.setChecked(getSharedPreferences("gow_settings", MODE_PRIVATE)
                .getBoolean("performance_hud_enabled", false));
        performanceToggle.setOnCheckedChangeListener((button, checked) ->
                getSharedPreferences("gow_settings", MODE_PRIVATE).edit()
                        .putBoolean("performance_hud_enabled", checked).apply());
        content.addView(performanceToggle, matchWrap());
        new AlertDialog.Builder(this).setTitle("DESEMPENHO")
                .setView(content).setPositiveButton("FECHAR", null).show();
    }

    private void showFiles() {
        File dir = gameDir();
        File image = new File(dir, "image.bin");
        File iso = new File(dir, "game.iso");
        String message = "Pasta do aplicativo:\n" + dir.getAbsolutePath() + "\n\n"
                + (image.isFile() && image.length() > 0 ? "✓ image.bin encontrado" : "• image.bin não encontrado")
                + "\n" + (iso.isFile() && iso.length() > 0 ? "✓ game.iso encontrado" : "• game.iso não encontrado")
                + "\n\nOs nomes esperados dependem da versão do runtime incluída no APK.";
        new AlertDialog.Builder(this).setTitle("ARQUIVOS DO JOGO")
                .setMessage(message).setPositiveButton("OK", null).show();
    }

    private void showLog() {
        File dir = getExternalFilesDir(null);
        if (dir == null) dir = getFilesDir();
        File log = new File(dir, "log.txt");
        StringBuilder out = new StringBuilder();
        if (log.isFile()) {
            try (BufferedReader reader = new BufferedReader(new FileReader(log))) {
                String line;
                int count = 0;
                while ((line = reader.readLine()) != null) {
                    if (count++ > 250) out.append("… log truncado na tela …\n");
                    else out.append(line).append('\n');
                }
            } catch (Exception e) {
                out.append("Não foi possível ler o log: ").append(e.getMessage());
            }
        } else {
            out.append("Nenhum log encontrado ainda. Inicie o jogo para gerar o registro.");
        }
        TextView body = new TextView(this);
        body.setText(out.toString());
        body.setTextColor(TEXT);
        body.setTextSize(11);
        body.setTypeface(Typeface.MONOSPACE);
        body.setPadding(dp(16), dp(8), dp(16), dp(8));
        ScrollView scroller = new ScrollView(this);
        scroller.addView(body);
        new AlertDialog.Builder(this).setTitle("DIAGNÓSTICO E LOG")
                .setView(scroller).setPositiveButton("FECHAR", null).show();
    }

    private int dp(int value) {
        return (int)(value * getResources().getDisplayMetrics().density + 0.5f);
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
        status.setText("Pasta: Android/data/org.gow.shell/files\n"
                + (imageOk ? "✓ image.bin" : "• image.bin pendente") + "   "
                + (isoOk ? "✓ game.iso" : "• game.iso pendente"));
        status.setTextColor(imageOk && isoOk ? 0xFF9BCB91 : GOLD);
        play.setAlpha(1f);
    }

    @Override protected void onResume() {
        super.onResume();
        if (status != null) refreshStatus();
    }

    private void launchGame() {
        startActivity(new Intent(this, GameActivity.class));
    }
}
