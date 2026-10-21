package org.gow.shell;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.text.InputType;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * God of War PSP Recomp Android launcher (layout horizontal, 6 telas).
 *
 * Só as opções que o runtime já entende gravam argumentos no args.txt:
 * --aspect, --res-scale, --fxaa e --fps. O resto aparece como "Em breve...".
 */
public final class LauncherActivity extends Activity {
    private static final int BG = Color.rgb(17, 17, 19);
    private static final int CARD = Color.rgb(28, 28, 32);
    private static final int BRD = Color.rgb(52, 52, 58);
    private static final int LINE = Color.rgb(38, 38, 43);
    private static final int RED = Color.rgb(200, 32, 48);
    private static final int ROSE = Color.rgb(232, 72, 90);
    private static final int TEXT = Color.rgb(238, 238, 238);
    private static final int MUTED = Color.rgb(154, 154, 162);
    private static final int OFF = Color.rgb(58, 58, 64);
    private static final int GREEN = Color.rgb(155, 203, 145);

    private static final String PREFS = "gow_settings";
    private static final String VERSION = "v0.1.0";
    private static final String REPO_URL = "https://github.com/flavinxs0/gow-psp-recomp-android";
    private static final String[] MODES = { "16:9", "4:3", "FULLSCREEN" };

    private interface Pick { void on(int index); }
    private interface Flip { void on(boolean checked); }

    private TextView status;
    private int pad;
    private String current = "main";

    // Estado pendente da tela Gráficos (só vale ao tocar em Aplicar).
    private int gRes;
    private int gAspect;
    private int gFps;
    private boolean gFxaa;
    private boolean gVsync;
    private boolean tVsync;
    private boolean tRes;
    private boolean tFxaa;
    private boolean tFps;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(BG);
        getWindow().setNavigationBarColor(BG);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LOW_PROFILE);
        pad = dp(20);
        showMain();
    }

    @Override protected void onResume() {
        super.onResume();
        if ("main".equals(current) && status != null) refreshStatus();
    }

    @Override public void onBackPressed() {
        if (!"main".equals(current)) showMain();
        else super.onBackPressed();
    }

    // ------------------------------------------------------------------ helpers de UI

    private int dp(int value) {
        return (int)(value * getResources().getDisplayMetrics().density + 0.5f);
    }

    private GradientDrawable shape(int color, int stroke, int radius) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(radius));
        if (stroke != 0) d.setStroke(dp(1), stroke);
        return d;
    }

    private TextView tv(String value, int size, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(value);
        t.setTextSize(size);
        t.setTextColor(color);
        t.setTypeface(Typeface.create("sans-serif", bold ? Typeface.BOLD : Typeface.NORMAL));
        return t;
    }

    private LinearLayout.LayoutParams lp(int w, int h) {
        return new LinearLayout.LayoutParams(w, h);
    }

    private LinearLayout.LayoutParams matchWrap() {
        return lp(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
    }

    private LinearLayout.LayoutParams wrapWrap() {
        return lp(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
    }

    private LinearLayout.LayoutParams weight(float w) {
        return new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, w);
    }

    private LinearLayout.LayoutParams withBottom(int bottom) {
        LinearLayout.LayoutParams p = matchWrap();
        p.bottomMargin = bottom;
        return p;
    }

    private LinearLayout column() {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        return c;
    }

    private LinearLayout rowLayout() {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.HORIZONTAL);
        c.setGravity(Gravity.CENTER_VERTICAL);
        return c;
    }

    private LinearLayout card() {
        LinearLayout c = column();
        c.setPadding(dp(16), dp(14), dp(16), dp(14));
        c.setBackground(shape(CARD, BRD, 14));
        return c;
    }

    private ScrollView newScroll() {
        ScrollView s = new ScrollView(this);
        s.setFillViewport(true);
        s.setBackgroundColor(BG);
        return s;
    }

    private AlertDialog.Builder dialog() {
        return new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert);
    }

    private void toast(String message) {
        Toast.makeText(this, message, Toast.LENGTH_LONG).show();
    }

    private TextView button(String label, boolean primary, final Runnable action) {
        TextView b = tv(label, 17, TEXT, true);
        b.setGravity(Gravity.CENTER_VERTICAL);
        b.setPadding(dp(20), dp(15), dp(20), dp(15));
        b.setBackground(primary ? shape(RED, 0, 14) : shape(CARD, BRD, 14));
        b.setOnClickListener(v -> action.run());
        return b;
    }

    private LinearLayout header(String title) {
        LinearLayout h = rowLayout();
        TextView back = tv("\u2190", 20, TEXT, false);
        back.setGravity(Gravity.CENTER);
        back.setBackground(shape(BG, BRD, 10));
        back.setOnClickListener(v -> showMain());
        h.addView(back, lp(dp(44), dp(44)));
        TextView t = tv(title, 22, TEXT, true);
        t.setPadding(dp(16), 0, 0, 0);
        h.addView(t, wrapWrap());
        return h;
    }

    /** Tela de configuração: seta de voltar + título no canto, coluna esquerda e direita. */
    private void showScreen(String name, String title, View left, View right) {
        current = name;
        status = null;
        ScrollView scroll = newScroll();
        LinearLayout col = column();
        col.setPadding(pad, pad, pad, pad);
        col.addView(header(title), withBottom(dp(16)));
        LinearLayout body = new LinearLayout(this);
        body.setOrientation(LinearLayout.HORIZONTAL);
        body.addView(left, weight(1f));
        LinearLayout.LayoutParams rp = weight(1.25f);
        rp.leftMargin = pad * 2;
        body.addView(right, rp);
        col.addView(body, matchWrap());
        scroll.addView(col);
        setContentView(scroll);
    }

    private void disable(View v) {
        v.setEnabled(false);
        v.setAlpha(0.4f);
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) g.getChildAt(i).setEnabled(false);
        }
    }

    /** Linha de opção: rótulo (+ legenda) à esquerda, controle à direita, divisor embaixo. */
    private LinearLayout row(String label, String sub, View control, boolean soon) {
        LinearLayout r = column();
        LinearLayout line = rowLayout();
        line.setPadding(0, dp(12), 0, dp(12));
        LinearLayout names = column();
        names.addView(tv(label, 16, TEXT, false), wrapWrap());
        if (sub != null) names.addView(tv(sub, 11, MUTED, false), wrapWrap());
        line.addView(names, weight(1f));
        if (soon) {
            TextView s = tv("Em breve...", 13, MUTED, false);
            s.setTypeface(Typeface.create("sans-serif", Typeface.ITALIC));
            s.setPadding(0, 0, dp(10), 0);
            line.addView(s, wrapWrap());
        }
        if (control != null) {
            if (soon) disable(control);
            line.addView(control, wrapWrap());
        }
        r.addView(line, matchWrap());
        View div = new View(this);
        div.setBackgroundColor(LINE);
        r.addView(div, lp(LinearLayout.LayoutParams.MATCH_PARENT, dp(1)));
        return r;
    }

    private void paintSeg(TextView[] items, int sel) {
        for (int i = 0; i < items.length; i++) {
            items[i].setBackground(i == sel ? shape(RED, 0, 10) : shape(BG, BRD, 10));
        }
    }

    /** Botões de escolha única (um selecionado, em vermelho). */
    private LinearLayout seg(String[] labels, int sel, final Pick pick) {
        LinearLayout g = rowLayout();
        final TextView[] items = new TextView[labels.length];
        for (int i = 0; i < labels.length; i++) {
            final int idx = i;
            TextView b = tv(labels[i], 14, TEXT, false);
            b.setGravity(Gravity.CENTER);
            b.setMinWidth(dp(52));
            b.setPadding(dp(14), dp(8), dp(14), dp(8));
            b.setOnClickListener(v -> {
                paintSeg(items, idx);
                pick.on(idx);
            });
            items[i] = b;
            LinearLayout.LayoutParams p = wrapWrap();
            if (i > 0) p.leftMargin = dp(6);
            g.addView(b, p);
        }
        paintSeg(items, sel);
        return g;
    }

    /** Botões de ação (contorno), opcionalmente em tom de alerta. */
    private LinearLayout actions(String[] labels, Runnable[] acts, boolean danger) {
        LinearLayout g = rowLayout();
        for (int i = 0; i < labels.length; i++) {
            final Runnable a = acts[i];
            TextView b = tv(labels[i], 14, danger ? ROSE : TEXT, false);
            b.setGravity(Gravity.CENTER);
            b.setMinWidth(dp(52));
            b.setPadding(dp(14), dp(8), dp(14), dp(8));
            b.setBackground(shape(BG, danger ? ROSE : BRD, 10));
            b.setOnClickListener(v -> {
                if (a != null) a.run();
            });
            LinearLayout.LayoutParams p = wrapWrap();
            if (i > 0) p.leftMargin = dp(6);
            g.addView(b, p);
        }
        return g;
    }

    private Switch toggle(boolean on, final Flip flip) {
        Switch s = new Switch(this);
        s.setChecked(on);
        if (Build.VERSION.SDK_INT >= 23) {
            s.setThumbTintList(ColorStateList.valueOf(Color.WHITE));
            s.setTrackTintList(new ColorStateList(
                    new int[][] { { android.R.attr.state_checked }, {} },
                    new int[] { RED, OFF }));
        }
        s.setOnCheckedChangeListener((b, checked) -> flip.on(checked));
        return s;
    }

    private View bar(int pct) {
        LinearLayout b = new LinearLayout(this);
        b.setWeightSum(100f);
        b.setBackground(shape(OFF, 0, 4));
        View fill = new View(this);
        fill.setBackground(shape(RED, 0, 4));
        b.addView(fill, new LinearLayout.LayoutParams(0, dp(8), (float) pct));
        b.setLayoutParams(lp(dp(170), dp(8)));
        return b;
    }

    // ------------------------------------------------------------------ 1. Entrada

    private void showMain() {
        current = "main";
        ScrollView scroll = newScroll();
        LinearLayout body = new LinearLayout(this);
        body.setOrientation(LinearLayout.HORIZONTAL);
        body.setPadding(pad, pad, pad, pad);

        LinearLayout left = column();
        left.setGravity(Gravity.CENTER_VERTICAL);
        TextView crest = tv("G O D   O F   W A R", 12, MUTED, false);
        crest.setLetterSpacing(0.1f);
        left.addView(crest, wrapWrap());
        SpannableStringBuilder name = new SpannableStringBuilder("Chains of Olympus");
        name.setSpan(new ForegroundColorSpan(ROSE), 10, 17, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        TextView title = tv("", 30, TEXT, true);
        title.setText(name);
        left.addView(title, wrapWrap());
        left.addView(tv("Launcher n\u00e3o oficial \u00b7 " + VERSION, 14, MUTED, false), wrapWrap());
        TextView by = tv("By flavinxs0", 13, ROSE, true);
        by.setPadding(0, dp(2), 0, dp(14));
        left.addView(by, wrapWrap());
        LinearLayout statusCard = card();
        status = tv("", 12, MUTED, false);
        statusCard.addView(status, matchWrap());
        left.addView(statusCard, withBottom(dp(14)));
        left.addView(tv("Android " + Build.VERSION.RELEASE + " \u00b7 " + abi(), 11, MUTED, false), wrapWrap());
        body.addView(left, weight(1f));

        LinearLayout right = column();
        right.setGravity(Gravity.CENTER_VERTICAL);
        right.addView(button("Jogar", true, this::launchGame), withBottom(dp(8)));
        right.addView(button("Gr\u00e1ficos", false, this::showGraphics), withBottom(dp(8)));
        right.addView(button("Desempenho / HUD", false, this::showHud), withBottom(dp(8)));
        right.addView(button("Controles", false, this::showControls), withBottom(dp(8)));
        right.addView(button("Extras", false, this::showExtras), withBottom(dp(8)));
        right.addView(button("Sobre", false, this::showAbout), withBottom(0));
        LinearLayout.LayoutParams rp = weight(1.25f);
        rp.leftMargin = pad * 2;
        body.addView(right, rp);

        scroll.addView(body);
        setContentView(scroll);
        refreshStatus();
    }

    private void refreshStatus() {
        File image = new File(gameDir(), "image.bin");
        File iso = new File(gameDir(), "game.iso");
        boolean imageOk = image.isFile() && image.length() > 0;
        boolean isoOk = iso.isFile() && iso.length() > 0;
        status.setText("Pasta: Android/data/org.gow.shell/files\n"
                + (imageOk ? "\u2713 image.bin" : "\u2022 image.bin pendente") + "   "
                + (isoOk ? "\u2713 game.iso" : "\u2022 game.iso pendente"));
        status.setTextColor(imageOk && isoOk ? GREEN : MUTED);
    }

    private void launchGame() {
        writeArgs();
        startActivity(new Intent(this, GameActivity.class));
    }

    // ------------------------------------------------------------------ 2. Gráficos

    private void showGraphics() {
        SharedPreferences p = prefs();
        gRes = currentRes();
        gFxaa = currentFxaa();
        gVsync = currentVsync();
        gFps = currentFps();
        String mode = p.getString("screen_mode", "16:9");
        gAspect = "4:3".equals(mode) ? 1 : ("FULLSCREEN".equals(mode) ? 2 : 0);
        tRes = false;
        tFxaa = false;
        tVsync = false;
        tFps = false;

        LinearLayout left = column();
        left.addView(tv("Cada op\u00e7\u00e3o vira um argumento do runtime. Escalas altas podem pesar na GPU.",
                13, MUTED, false), withBottom(dp(10)));
        left.addView(tv("16:9 = padr\u00e3o do PSP \u00b7 4:3 = cl\u00e1ssico \u00b7 Esticada = tela toda",
                12, MUTED, false), withBottom(dp(10)));
        left.addView(tv("60 FPS \u00e9 o padr\u00e3o e n\u00e3o grava --fps.", 12, MUTED, false),
                withBottom(dp(18)));
        left.addView(button("Aplicar", true, this::applyGraphics), matchWrap());

        LinearLayout right = column();
        right.addView(row("Escala de resolu\u00e7\u00e3o", "--res-scale",
                seg(new String[] { "1x", "2x", "3x" }, gRes - 1, i -> { gRes = i + 1; tRes = true; }), false));
        right.addView(row("Propor\u00e7\u00e3o de tela", "--aspect",
                seg(new String[] { "16:9", "4:3", "Esticada" }, gAspect, i -> gAspect = i), false));
        right.addView(row("FXAA", "--fxaa",
                toggle(gFxaa, on -> { gFxaa = on; tFxaa = true; }), false));
        right.addView(row("VSync", "--vsync",
                toggle(gVsync, on -> { gVsync = on; tVsync = true; }), false));
        right.addView(row("Limite de FPS", "--fps",
                seg(new String[] { "30", "45", "60" }, fpsIndex(gFps), i -> {
                    gFps = i == 0 ? 30 : (i == 1 ? 45 : 60);
                    tFps = true;
                }), false));
        showScreen("graphics", "Gr\u00e1ficos", left, right);
    }

    private int fpsIndex(int fps) {
        return fps == 30 ? 0 : (fps == 45 ? 1 : 2);
    }

    private void applyGraphics() {
        SharedPreferences.Editor e = prefs().edit();
        e.putString("screen_mode", MODES[gAspect]);
        if (tRes) e.putInt("res_scale", gRes);
        if (tFxaa) e.putBoolean("fxaa", gFxaa);
        if (tVsync) e.putBoolean("vsync", gVsync);
        if (tFps) e.putInt("fps_limit", gFps);
        e.apply();
        writeArgs();
        String line = readArgsLine();
        toast("Salvo. Vale ao tocar em JOGAR.\nargs.txt: " + (line.isEmpty() ? "(vazio)" : line));
        showMain();
    }

    // ------------------------------------------------------------------ 3. Desempenho / HUD

    private void showHud() {
        LinearLayout left = column();
        left.addView(tv("Ative o painel durante o jogo. O FPS real depende de instrumenta\u00e7\u00e3o nativa "
                + "na biblioteca do jogo.", 13, MUTED, false), matchWrap());

        LinearLayout right = column();
        right.addView(row("Mostrar HUD em jogo", null,
                toggle(prefs().getBoolean("performance_hud_enabled", false),
                        on -> prefs().edit().putBoolean("performance_hud_enabled", on).apply()), false));
        right.addView(row("FPS", null, toggle(true, on -> { }), true));
        right.addView(row("Tempo de quadro", null, toggle(true, on -> { }), true));
        right.addView(row("Mem\u00f3ria (RAM)", null, toggle(false, on -> { }), true));
        right.addView(row("Posi\u00e7\u00e3o", null, seg(new String[] { "Esq.", "Dir." }, 0, i -> { }), true));
        right.addView(row("Opacidade", "70%", bar(70), true));
        showScreen("hud", "Desempenho / HUD", left, right);
    }

    // ------------------------------------------------------------------ 4. Controles

    private void showControls() {
        final SharedPreferences p = prefs();
        final int[] ops = { 30, 50, 70, 100 };
        int opIndex = 1;
        for (int i = 0; i < ops.length; i++) {
            if (ops[i] == p.getInt("ctl_opacity", 50)) opIndex = i;
        }
        LinearLayout left = column();
        LinearLayout info = card();
        info.addView(tv("Controles na tela", 18, TEXT, true), withBottom(dp(6)));
        info.addView(tv("Dire\u00e7\u00e3o, anal\u00f3gico, \u2715 \u25cb \u25a1 \u25b3, L, R, Start e Select, "
                + "com v\u00e1rios toques ao mesmo tempo. O anal\u00f3gico virtual \u00e9 digital "
                + "(tudo ou nada) por enquanto. Vale na pr\u00f3xima vez que tocar em JOGAR.",
                13, MUTED, false), matchWrap());
        left.addView(info, matchWrap());

        LinearLayout right = column();
        right.addView(row("Controles na tela", "Dire\u00e7\u00e3o, anal\u00f3gico e bot\u00f5es",
                toggle(p.getBoolean("ctl_screen", true),
                        on -> p.edit().putBoolean("ctl_screen", on).apply()), false));
        right.addView(row("Autom\u00e1tico", "Oculta os controles na tela ao conectar um controle",
                toggle(p.getBoolean("ctl_auto", true),
                        on -> p.edit().putBoolean("ctl_auto", on).apply()), false));
        right.addView(row("Opacidade", null,
                seg(new String[] { "30%", "50%", "70%", "100%" }, opIndex,
                        i -> p.edit().putInt("ctl_opacity", ops[i]).apply()), false));
        right.addView(row("Tipo de controle", null,
                seg(new String[] { "Tela", "PlayStation", "Xbox" }, 0, i -> { }), true));
        right.addView(row("Vibra\u00e7\u00e3o do controle", null, toggle(true, on -> { }), true));
        right.addView(row("Zona morta do anal\u00f3gico", "15%", bar(15), true));
        right.addView(row("Mapeamento dos bot\u00f5es", null,
                seg(new String[] { "Padr\u00e3o", "Personalizar" }, 0, i -> { }), true));
        showScreen("controls", "Controles", left, right);
    }

    // ------------------------------------------------------------------ 5. Extras

    private void showExtras() {
        File iso = new File(gameDir(), "game.iso");
        String line = readArgsLine();

        LinearLayout left = column();
        LinearLayout argsCard = card();
        argsCard.addView(tv("Argumentos atuais (args.txt)", 12, MUTED, false), wrapWrap());
        TextView argsText = tv(line.isEmpty() ? "(vazio)" : line, 16, TEXT, false);
        argsText.setTypeface(Typeface.MONOSPACE);
        argsText.setPadding(0, dp(6), 0, dp(4));
        argsCard.addView(argsText, matchWrap());
        left.addView(argsCard, withBottom(dp(12)));

        LinearLayout discCard = card();
        discCard.addView(tv("Disco", 12, MUTED, false), wrapWrap());
        String size = iso.isFile() && iso.length() > 0
                ? String.format(Locale.US, "game.iso \u00b7 %.1f GB", iso.length() / 1073741824.0)
                : "game.iso n\u00e3o encontrado";
        TextView discText = tv(size, 16, TEXT, false);
        discText.setPadding(0, dp(6), 0, dp(4));
        discCard.addView(discText, matchWrap());
        discCard.addView(tv("Android/data/org.gow.shell/files", 11, MUTED, false), wrapWrap());
        left.addView(discCard, withBottom(dp(12)));
        left.addView(tv("Launcher n\u00e3o oficial \u00b7 " + VERSION + " \u00b7 By flavinxs0", 11, MUTED, false),
                wrapWrap());

        LinearLayout right = column();
        right.addView(row("Saves", "Backup e restaura\u00e7\u00e3o de ms0/PSP/SAVEDATA",
                actions(new String[] { "Backup", "Restaurar" },
                        new Runnable[] { this::backupSaves, this::restoreSaves }, false), false));
        right.addView(row("Logs", "Ver, compartilhar ou limpar o log.txt",
                actions(new String[] { "Ver", "Compartilhar", "Limpar" },
                        new Runnable[] { this::showLog, this::shareLog, this::clearLog }, false), false));
        right.addView(row("Argumentos avan\u00e7ados", "Ver e editar o args.txt",
                actions(new String[] { "Editar" }, new Runnable[] { this::editArgs }, false), false));
        right.addView(row("Disco", "Trocar o game.iso",
                actions(new String[] { "Trocar" }, new Runnable[] { null }, false), true));
        right.addView(row("Restaurar padr\u00f5es", "Volta todas as configura\u00e7\u00f5es ao original",
                actions(new String[] { "Restaurar" }, new Runnable[] { this::confirmReset }, true), false));
        right.addView(row("Idioma", null, seg(new String[] { "Portugu\u00eas", "English" }, 0, i -> { }), true));
        showScreen("extras", "Extras", left, right);
    }

    private File savesDir() {
        return new File(gameDir(), "ms0/PSP/SAVEDATA");
    }

    private File backupsDir() {
        return new File(gameDir(), "backup_saves");
    }

    private boolean copyTree(File src, File dst) {
        try {
            if (src.isDirectory()) {
                if (!dst.isDirectory() && !dst.mkdirs()) return false;
                File[] kids = src.listFiles();
                if (kids == null) return true;
                for (File k : kids) {
                    if (!copyTree(k, new File(dst, k.getName()))) return false;
                }
                return true;
            }
            File parent = dst.getParentFile();
            if (parent != null) parent.mkdirs();
            try (FileInputStream in = new FileInputStream(src);
                 FileOutputStream out = new FileOutputStream(dst)) {
                byte[] buf = new byte[65536];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            }
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    private void backupSaves() {
        final File src = savesDir();
        if (!src.isDirectory()) {
            toast("Nenhum save encontrado ainda.");
            return;
        }
        final String stamp = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date());
        final File dst = new File(backupsDir(), stamp);
        new Thread(() -> {
            final boolean ok = copyTree(src, dst);
            runOnUiThread(() -> toast(ok ? "Backup criado: backup_saves/" + stamp
                    : "Falha ao criar o backup."));
        }).start();
    }

    private void restoreSaves() {
        File[] backups = backupsDir().listFiles();
        if (backups == null || backups.length == 0) {
            toast("Nenhum backup encontrado.");
            return;
        }
        Arrays.sort(backups);
        final File latest = backups[backups.length - 1];
        dialog().setTitle("RESTAURAR SAVES")
                .setMessage("Restaurar o backup " + latest.getName()
                        + "?\n\nOs saves atuais ser\u00e3o sobrescritos.")
                .setNegativeButton("CANCELAR", null)
                .setPositiveButton("RESTAURAR", (d, w) -> new Thread(() -> {
                    final boolean ok = copyTree(latest, savesDir());
                    runOnUiThread(() -> toast(ok ? "Saves restaurados." : "Falha ao restaurar."));
                }).start())
                .show();
    }

    private File logFile() {
        return new File(gameDir(), "log.txt");
    }

    private String readLog(int maxChars) {
        File log = logFile();
        if (!log.isFile()) return "";
        StringBuilder out = new StringBuilder();
        try (BufferedReader reader = new BufferedReader(new FileReader(log))) {
            String line;
            while ((line = reader.readLine()) != null) out.append(line).append('\n');
        } catch (Exception e) {
            return "N\u00e3o foi poss\u00edvel ler o log: " + e.getMessage();
        }
        if (out.length() > maxChars) return out.substring(out.length() - maxChars);
        return out.toString();
    }

    private void showLog() {
        String text = readLog(60000);
        if (text.isEmpty()) text = "Nenhum log encontrado ainda. Inicie o jogo para gerar o registro.";
        TextView body = new TextView(this);
        body.setText(text);
        body.setTextColor(TEXT);
        body.setTextSize(11);
        body.setTypeface(Typeface.MONOSPACE);
        body.setPadding(dp(16), dp(8), dp(16), dp(8));
        ScrollView scroller = new ScrollView(this);
        scroller.addView(body);
        dialog().setTitle("LOG").setView(scroller).setPositiveButton("FECHAR", null).show();
    }

    private void shareLog() {
        String text = readLog(200000);
        if (text.isEmpty()) {
            toast("Nenhum log encontrado ainda.");
            return;
        }
        Intent send = new Intent(Intent.ACTION_SEND);
        send.setType("text/plain");
        send.putExtra(Intent.EXTRA_SUBJECT, "log.txt");
        send.putExtra(Intent.EXTRA_TEXT, text);
        startActivity(Intent.createChooser(send, "Compartilhar log"));
    }

    private void clearLog() {
        if (!logFile().isFile()) {
            toast("Nenhum log para limpar.");
            return;
        }
        dialog().setTitle("LIMPAR LOG")
                .setMessage("Apagar o log.txt da \u00faltima sess\u00e3o?")
                .setNegativeButton("CANCELAR", null)
                .setPositiveButton("LIMPAR", (d, w) -> {
                    toast(logFile().delete() ? "Log apagado." : "N\u00e3o foi poss\u00edvel apagar o log.");
                })
                .show();
    }

    private void editArgs() {
        final EditText input = new EditText(this);
        input.setText(readArgsLine());
        input.setTextColor(TEXT);
        input.setTypeface(Typeface.MONOSPACE);
        input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE
                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        input.setMinLines(3);
        input.setGravity(Gravity.TOP | Gravity.START);
        dialog().setTitle("ARGUMENTOS AVAN\u00c7ADOS")
                .setMessage("Edite com cuidado. A propor\u00e7\u00e3o de tela (--aspect) \u00e9 controlada pela tela Gr\u00e1ficos.")
                .setView(input)
                .setNegativeButton("CANCELAR", null)
                .setPositiveButton("SALVAR", (d, w) -> {
                    saveArgsRaw(input.getText().toString());
                    toast("args.txt salvo.");
                    showExtras();
                })
                .show();
    }

    private void confirmReset() {
        dialog().setTitle("RESTAURAR PADR\u00d5ES")
                .setMessage("Voltar proporção, escala, FXAA, limite de FPS e HUD ao original? "
                        + "Outros argumentos do args.txt s\u00e3o mantidos.")
                .setNegativeButton("CANCELAR", null)
                .setPositiveButton("RESTAURAR", (d, w) -> {
                    resetDefaults();
                    toast("Padr\u00f5es restaurados.");
                    showExtras();
                })
                .show();
    }

    // ------------------------------------------------------------------ 6. Sobre

    private void showAbout() {
        LinearLayout left = column();
        LinearLayout brand = card();
        brand.addView(tv("G O D   O F   W A R", 12, MUTED, false), wrapWrap());
        SpannableStringBuilder name = new SpannableStringBuilder("Chains of Olympus");
        name.setSpan(new ForegroundColorSpan(ROSE), 10, 17, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        TextView title = tv("", 26, TEXT, true);
        title.setText(name);
        brand.addView(title, wrapWrap());
        brand.addView(tv("Launcher n\u00e3o oficial \u00b7 " + VERSION, 14, MUTED, false), wrapWrap());
        TextView by = tv("By flavinxs0", 13, ROSE, true);
        by.setPadding(0, dp(2), 0, 0);
        brand.addView(by, wrapWrap());
        left.addView(brand, withBottom(dp(12)));

        LinearLayout notice = card();
        notice.addView(tv("Aviso", 12, MUTED, false), withBottom(dp(6)));
        notice.addView(tv("Projeto n\u00e3o oficial, sem afilia\u00e7\u00e3o com a Sony.\n"
                + "\u00c9 necess\u00e1rio ter sua pr\u00f3pria c\u00f3pia do jogo.", 13, TEXT, false), matchWrap());
        left.addView(notice, matchWrap());

        LinearLayout right = column();
        right.addView(row("Vers\u00e3o", null, tv(VERSION, 15, MUTED, false), false));
        right.addView(row("Aparelho", null, tv(Build.MANUFACTURER + " " + Build.MODEL, 15, MUTED, false), false));
        right.addView(row("Android", null, tv(Build.VERSION.RELEASE, 15, MUTED, false), false));
        right.addView(row("Arquitetura", null, tv(abi(), 15, MUTED, false), false));
        right.addView(row("C\u00f3digo-fonte", "Projeto no GitHub",
                actions(new String[] { "GitHub" }, new Runnable[] { this::openRepo }, false), false));
        right.addView(row("Licen\u00e7as", "Bibliotecas de c\u00f3digo aberto",
                actions(new String[] { "Ver" }, new Runnable[] { this::showLicenses }, false), false));
        showScreen("about", "Sobre", left, right);
    }

    private String abi() {
        String[] abis = Build.SUPPORTED_ABIS;
        return abis != null && abis.length > 0 ? abis[0] : "ARM64";
    }

    private void openRepo() {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(REPO_URL)));
        } catch (ActivityNotFoundException e) {
            toast("Nenhum navegador encontrado.");
        }
    }

    private void showLicenses() {
        dialog().setTitle("LICEN\u00c7AS")
                .setMessage("Este app usa a biblioteca SDL (licen\u00e7a zlib).\n\n"
                        + "A lista completa de componentes est\u00e1 no reposit\u00f3rio do projeto.")
                .setPositiveButton("OK", null)
                .show();
    }

    // ------------------------------------------------------------------ args.txt

    private SharedPreferences prefs() {
        return getSharedPreferences(PREFS, MODE_PRIVATE);
    }

    private File gameDir() {
        File dir = getExternalFilesDir(null);
        return dir == null ? getFilesDir() : dir;
    }

    private List<String> tokenize(String text) {
        List<String> out = new ArrayList<>();
        for (String t : text.trim().split("\\s+")) {
            if (!t.isEmpty()) out.add(t);
        }
        return out;
    }

    private List<String> readArgTokens() {
        File f = new File(gameDir(), "args.txt");
        StringBuilder all = new StringBuilder();
        if (f.isFile()) {
            try (BufferedReader reader = new BufferedReader(new FileReader(f))) {
                String line;
                while ((line = reader.readLine()) != null) all.append(line).append(' ');
            } catch (Exception ignored) {
            }
        }
        return tokenize(all.toString());
    }

    private String readArgsLine() {
        return join(readArgTokens());
    }

    private String join(List<String> tokens) {
        StringBuilder sb = new StringBuilder();
        for (String t : tokens) {
            if (sb.length() > 0) sb.append(' ');
            sb.append(t);
        }
        return sb.toString();
    }

    private String tokenValue(List<String> tokens, String key) {
        int i = tokens.indexOf(key);
        return i >= 0 && i + 1 < tokens.size() ? tokens.get(i + 1) : null;
    }

    private int parseInt(String value, int fallback) {
        try {
            return value == null ? fallback : Integer.parseInt(value);
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    private int currentRes() {
        SharedPreferences p = prefs();
        if (p.contains("res_scale")) return p.getInt("res_scale", 1);
        return parseInt(tokenValue(readArgTokens(), "--res-scale"), 1);
    }

    private boolean currentFxaa() {
        SharedPreferences p = prefs();
        if (p.contains("fxaa")) return p.getBoolean("fxaa", false);
        return readArgTokens().contains("--fxaa");
    }

    private boolean currentVsync() {
        SharedPreferences p = prefs();
        if (p.contains("vsync")) return p.getBoolean("vsync", false);
        return readArgTokens().contains("--vsync");
    }

    private int currentFps() {
        SharedPreferences p = prefs();
        if (p.contains("fps_limit")) return p.getInt("fps_limit", 60);
        return parseInt(tokenValue(readArgTokens(), "--fps"), 60);
    }

    /** Proporção real da tela na horizontal, reduzida (ex.: 2400x1080 -> 20:9). */
    @SuppressWarnings("deprecation")
    private String stretchRatio() {
        android.util.DisplayMetrics m = new android.util.DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(m);
        int w = Math.max(m.widthPixels, m.heightPixels);
        int h = Math.min(m.widthPixels, m.heightPixels);
        if (w <= 0 || h <= 0) return "16:9";
        int a = w, b = h;
        while (b != 0) { int t = a % b; a = b; b = t; }
        return (w / a) + ":" + (h / a);
    }

    /** Remove --aspect sempre; os demais argumentos gerenciados só quando pedido. */
    private List<String> filterTokens(List<String> tokens, boolean res, boolean fxaa, boolean fps, boolean vsync) {
        List<String> out = new ArrayList<>();
        for (int i = 0; i < tokens.size(); i++) {
            String t = tokens.get(i);
            if (t.equals("--aspect")) { i++; continue; }
            if (res && t.equals("--res-scale")) { i++; continue; }
            if (fps && t.equals("--fps")) { i++; continue; }
            if (fxaa && t.equals("--fxaa")) continue;
            if (vsync && t.equals("--vsync")) continue;
            out.add(t);
        }
        return out;
    }

    /**
     * Monta a linha do args.txt: mantém o que já estava lá e troca só o que foi escolhido
     * nas telas. 16:9, 1x sem toque e 60 FPS não escrevem nada novo.
     */
    private String buildArgsLine() {
        SharedPreferences p = prefs();
        String mode = p.getString("screen_mode", "16:9");
        boolean res = p.contains("res_scale");
        boolean fxaa = p.contains("fxaa");
        boolean fps = p.contains("fps_limit");
        boolean vsync = p.contains("vsync");
        List<String> out = filterTokens(readArgTokens(), res, fxaa, fps, vsync);
        if (res) {
            out.add("--res-scale");
            out.add(String.valueOf(p.getInt("res_scale", 1)));
        }
        if (fxaa && p.getBoolean("fxaa", false)) out.add("--fxaa");
        if (vsync && p.getBoolean("vsync", false)) out.add("--vsync");
        if (fps) {
            int limit = p.getInt("fps_limit", 60);
            if (limit != 60) {
                out.add("--fps");
                out.add(String.valueOf(limit));
            }
        }
        if ("4:3".equals(mode)) {
            out.add("--aspect");
            out.add("4:3");
        } else if ("FULLSCREEN".equals(mode)) {
            out.add("--aspect");
            out.add(stretchRatio());
        }
        return join(out);
    }

    private void writeLine(String line) {
        // A linha tem de ser montada ANTES de abrir o arquivo: abrir trunca o args.txt.
        File f = new File(gameDir(), "args.txt");
        try (FileOutputStream out = new FileOutputStream(f)) {
            out.write((line + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (Exception ignored) {
        }
    }

    private void writeArgs() {
        writeLine(buildArgsLine());
    }

    /** Salva o texto editado à mão e alinha as preferências com ele. */
    private void saveArgsRaw(String text) {
        List<String> tokens = tokenize(text);
        String aspect = tokenValue(tokens, "--aspect");
        String mode = aspect == null ? "16:9" : ("4:3".equals(aspect) ? "4:3" : "FULLSCREEN");
        prefs().edit().remove("res_scale").remove("fxaa").remove("fps_limit").remove("vsync")
                .putString("screen_mode", mode).apply();
        writeLine(join(tokens));
    }

    private void resetDefaults() {
        prefs().edit().remove("res_scale").remove("fxaa").remove("fps_limit").remove("vsync")
                .remove("performance_hud_enabled").remove("ctl_screen").remove("ctl_auto").remove("ctl_opacity").putString("screen_mode", "16:9").apply();
        writeLine(join(filterTokens(readArgTokens(), true, true, true, true)));
    }
}
