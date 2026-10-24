package org.gow.shell;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.drawable.GradientDrawable;
import android.os.Debug;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.widget.TextView;
import java.util.Locale;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Bundle;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.Button;
import android.widget.SeekBar;

import org.libsdl.app.SDLActivity;

import java.util.Arrays;

/** SDL host activity launched by the Android launcher, with on-screen touch controls. */
public final class GameActivity extends SDLActivity {
    private SharedPreferences prefs;
    private InputManager inputManager;
    private TouchControls controls;
    private TextView performanceHud;
    private final Handler hudHandler = new Handler(Looper.getMainLooper());
    private static native double nativeGetFps();
    private static native double nativeGetFrameMs();

    private String localized(String value) {
        if (!"en".equals(getSharedPreferences("gow_settings", MODE_PRIVATE)
                .getString("launcher_language", "pt"))) return value;
        switch (value) {
            case "MENU DO JOGO": return "GAME MENU";
            case "Sair do game": return "Exit game";
            case "Editar controles na tela": return "Edit on-screen controls";
            case "Editar HUD de FPS": return "Edit FPS HUD";
            case "Concluir edição": return "Finish editing";
            case "Entre no modo de edição e arraste o botão desejado. Toque em Concluir para salvar.": return "Enter edit mode and drag the button you want. Tap Finish to save.";
            case "Editar": return "Edit";
            case "Cancelar": return "Cancel";
            case "Exibição compacta em uma linha": return "Compact single-line display";
            case "Posicionar à direita": return "Align right";
            case "Opacidade do HUD": return "HUD opacity";
            case "Mostrar HUD de desempenho": return "Show performance HUD";
            case "Tempo de quadro": return "Frame time";
            case "RAM utilizada": return "RAM used";
            case "Quadro": return "Frame time";
            case "Salvar": return "Save";
            case "Fechar": return "Close";
            case "Restaurar": return "Restore";
            case "GAME MENU": return "GAME MENU";
            case "TOQUE E ARRASTE CADA BOTÃO": return "TAP AND DRAG EACH BUTTON";
            case "EDITANDO: ": return "EDITING: ";
            case "CIMA": return "UP";
            case "BAIXO": return "DOWN";
            case "ESQUERDA": return "LEFT";
            case "DIREITA": return "RIGHT";
            case "CRUZ": return "CROSS";
            case "CÍRCULO": return "CIRCLE";
            case "QUADRADO": return "SQUARE";
            case "TRIÂNGULO": return "TRIANGLE";
            case "ANALÓGICO": return "ANALOG STICK";
            case "ANALÓGICO BAIXO": return "ANALOG DOWN";
            case "ANALÓGICO ESQUERDA": return "ANALOG LEFT";
            case "ANALÓGICO DIREITA": return "ANALOG RIGHT";

            default: return value;
        }
    }


    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        enterImmersiveMode();
        prefs = getSharedPreferences("gow_settings", MODE_PRIVATE);

        controls = new TouchControls(this, prefs.getInt("ctl_opacity", 50),
                "xbox".equals(prefs.getString("ctl_style", "ps")), prefs);
        controls.sink = new TouchControls.Sink() {
            @Override public void down(int keyCode) { SDLActivity.onNativeKeyDown(keyCode); }
            @Override public void up(int keyCode) { SDLActivity.onNativeKeyUp(keyCode); }
        };
        addContentView(controls, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        inputManager = (InputManager) getSystemService(Context.INPUT_SERVICE);
        if (inputManager != null) inputManager.registerInputDeviceListener(padListener, null);
        applyControlsVisibility();
        createPerformanceHud();
        createGameDrawer();
    }

    @Override
    protected void onResume() {
        super.onResume();
        hudHandler.removeCallbacks(hudUpdater);
        if (performanceHud != null) hudHandler.post(hudUpdater);
    }

    @Override
    protected void onPause() {
        if (controls != null) controls.releaseAll();
        hudHandler.removeCallbacks(hudUpdater);
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        hudHandler.removeCallbacks(hudUpdater);
        if (inputManager != null) inputManager.unregisterInputDeviceListener(padListener);
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) enterImmersiveMode();
    }


    private View gameDrawerHandle;
    private LinearLayout gameDrawer;
    private Button finishEditButton;

    private void createGameDrawer() {
        FrameLayout.LayoutParams handleLp = new FrameLayout.LayoutParams(dp(28), dp(92), Gravity.RIGHT | Gravity.CENTER_VERTICAL);
        gameDrawerHandle = new TextView(this);
        ((TextView) gameDrawerHandle).setText("‹");
        ((TextView) gameDrawerHandle).setTextColor(Color.WHITE);
        ((TextView) gameDrawerHandle).setTextSize(25);
        ((TextView) gameDrawerHandle).setGravity(Gravity.CENTER);
        gameDrawerHandle.setBackground(drawerBackground());
        gameDrawerHandle.setElevation(dp(40));
        gameDrawerHandle.setOnClickListener(v -> toggleGameDrawer());
        addContentView(gameDrawerHandle, handleLp);

        gameDrawer = new LinearLayout(this);
        gameDrawer.setOrientation(LinearLayout.VERTICAL);
        gameDrawer.setPadding(dp(14), dp(18), dp(14), dp(14));
        gameDrawer.setBackground(drawerBackground());
        gameDrawer.setElevation(dp(41));
        TextView title = new TextView(this);
        title.setText(localized("MENU DO JOGO"));
        title.setTextColor(Color.rgb(235, 70, 85));
        title.setTextSize(15);
        title.setTypeface(null, android.graphics.Typeface.BOLD);
        gameDrawer.addView(title, new LinearLayout.LayoutParams(-1, dp(42)));
        addDrawerButton(localized("Sair do game"), () -> { if (controls != null) controls.releaseAll(); finish(); });
        addDrawerButton(localized("Editar controles na tela"), this::showInGameControlEditor);
        addDrawerButton(localized("Editar HUD de FPS"), this::showInGameHudEditor);
        FrameLayout.LayoutParams drawerLp = new FrameLayout.LayoutParams(dp(270), -1, Gravity.RIGHT | Gravity.CENTER_VERTICAL);
        drawerLp.setMargins(0, dp(12), dp(28), dp(12));
        gameDrawer.setVisibility(View.GONE);
        addContentView(gameDrawer, drawerLp);

        finishEditButton = new Button(this);
        finishEditButton.setText(localized("Concluir edição"));
        finishEditButton.setAllCaps(false);
        finishEditButton.setTextColor(Color.WHITE);
        finishEditButton.setBackground(drawerBackground());
        finishEditButton.setElevation(dp(45));
        finishEditButton.setVisibility(View.GONE);
        finishEditButton.setOnClickListener(v -> {
            if (controls != null) controls.setEditMode(false);
            finishEditButton.setVisibility(View.GONE);
        });
        FrameLayout.LayoutParams doneLp = new FrameLayout.LayoutParams(-2, dp(48), Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        doneLp.topMargin = dp(8);
        addContentView(finishEditButton, doneLp);
    }

    private void styleGameDialog(android.app.AlertDialog dialog) {
        if (dialog.getWindow() != null) dialog.getWindow().setBackgroundDrawable(drawerBackground());
        int accent = Color.rgb(235, 70, 85);
        int titleId = getResources().getIdentifier("alertTitle", "id", "android");
        TextView title = titleId == 0 ? null : dialog.findViewById(titleId);
        if (title != null) title.setTextColor(Color.WHITE);
        Button positive = dialog.getButton(android.app.AlertDialog.BUTTON_POSITIVE);
        Button negative = dialog.getButton(android.app.AlertDialog.BUTTON_NEGATIVE);
        Button neutral = dialog.getButton(android.app.AlertDialog.BUTTON_NEUTRAL);
        if (positive != null) positive.setTextColor(accent);
        if (negative != null) negative.setTextColor(Color.LTGRAY);
        if (neutral != null) neutral.setTextColor(accent);
        android.widget.ListView choices = dialog.getListView();
        if (choices != null) {
            for (int i = 0; i < choices.getChildCount(); i++) {
                View item = choices.getChildAt(i);
                if (item instanceof TextView) ((TextView) item).setTextColor(Color.WHITE);
            }
        }
    }

    private GradientDrawable drawerBackground() {
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(Color.argb(245, 18, 18, 23));
        bg.setCornerRadius(dp(12));
        bg.setStroke(dp(1), Color.rgb(150, 35, 48));
        return bg;
    }

    private void addDrawerButton(String label, Runnable action) {
        Button b = new Button(this);
        b.setText(label);
        b.setTextColor(Color.WHITE);
        b.setAllCaps(false);
        b.setGravity(Gravity.CENTER_VERTICAL | Gravity.LEFT);
        b.setPadding(dp(14), 0, dp(10), 0);
        GradientDrawable buttonBg = new GradientDrawable();
        buttonBg.setColor(Color.rgb(29, 22, 26));
        buttonBg.setCornerRadius(dp(8));
        buttonBg.setStroke(dp(1), Color.rgb(112, 35, 47));
        b.setBackground(buttonBg);
        b.setOnClickListener(v -> { toggleGameDrawer(); action.run(); });
        LinearLayout.LayoutParams buttonLp = new LinearLayout.LayoutParams(-1, dp(52));
        buttonLp.bottomMargin = dp(8);
        gameDrawer.addView(b, buttonLp);
    }

    private void toggleGameDrawer() {
        if (gameDrawer == null) return;
        boolean open = gameDrawer.getVisibility() != View.VISIBLE;
        gameDrawer.setVisibility(open ? View.VISIBLE : View.GONE);
        ((TextView) gameDrawerHandle).setText(open ? "›" : "‹");
        if (controls != null && open) controls.releaseAll();
    }

    private void showInGameControlEditor() {
        final String[] profiles = { "PlayStation", "Xbox" };
        int current = "xbox".equals(prefs.getString("ctl_style", "ps")) ? 1 : 0;
        android.app.AlertDialog profileDialog = new android.app.AlertDialog.Builder(this)
                .setTitle(localized("Editar controles na tela"))
                .setSingleChoiceItems(profiles, current, (dialog, which) -> {
                    prefs.edit().putString("ctl_style", which == 1 ? "xbox" : "ps").apply();
                    if (controls != null) controls.setControllerStyle(which == 1);
                    dialog.dismiss();
                    android.app.AlertDialog editDialog = new android.app.AlertDialog.Builder(this)
                             .setTitle(profiles[which])
                            .setMessage(localized("Entre no modo de edição e arraste o botão desejado. Toque em Concluir para salvar."))
                            .setPositiveButton(localized("Editar"), (d, w) -> {
                                if (controls != null) controls.setEditMode(true);
                                if (finishEditButton != null) finishEditButton.setVisibility(View.VISIBLE);
                            })
                            .setNegativeButton(localized("Cancelar"), null).create();
                    editDialog.show();
                    styleGameDialog(editDialog);
                }).setNegativeButton(localized("Cancelar"), null).create();
        profileDialog.show();
        styleGameDialog(profileDialog);
    }

    private void showInGameHudEditor() {
        LinearLayout box = new LinearLayout(this);
        box.setPadding(dp(20), dp(8), dp(20), dp(8));
        box.setBackgroundColor(Color.rgb(18, 18, 23));
        box.setOrientation(LinearLayout.VERTICAL);
        android.widget.Switch enabled = hudSwitch("Mostrar HUD de desempenho", "performance_hud_enabled", true);
        android.widget.Switch fps = hudSwitch("FPS", "performance_hud_fps", true);
        android.widget.Switch frame = hudSwitch("Tempo de quadro", "performance_hud_frame_time", true);
        android.widget.Switch ram = hudSwitch("RAM utilizada", "performance_hud_ram", true);
        android.widget.Switch compact = new android.widget.Switch(this);
        compact.setText(localized("Exibição compacta em uma linha"));
        compact.setTextColor(Color.WHITE);
        compact.setChecked(prefs.getInt("performance_hud_style", 0) == 1);
        android.widget.Switch right = new android.widget.Switch(this);
        right.setText(localized("Posicionar à direita"));
        right.setTextColor(Color.WHITE);
        right.setChecked(prefs.getInt("performance_hud_position", 0) == 1);
        box.addView(enabled); box.addView(fps); box.addView(frame); box.addView(ram);
        box.addView(compact); box.addView(right);
        TextView opacityLabel = new TextView(this);
        opacityLabel.setText(localized("Opacidade do HUD"));
        opacityLabel.setTextColor(Color.WHITE);
        box.addView(opacityLabel);
        SeekBar opacity = new SeekBar(this);
        opacity.setMax(100);
        opacity.setProgress(prefs.getInt("performance_hud_opacity", 70));
        if (Build.VERSION.SDK_INT >= 21) {
            opacity.setProgressTintList(android.content.res.ColorStateList.valueOf(Color.rgb(235,70,85)));
            opacity.setThumbTintList(android.content.res.ColorStateList.valueOf(Color.rgb(235,70,85)));
        }
        box.addView(opacity);
        android.app.AlertDialog hudDialog = new android.app.AlertDialog.Builder(this).setTitle(localized("Editar HUD de FPS")).setView(box)
                .setPositiveButton(localized("Salvar"), (d, w) -> {
                    prefs.edit().putBoolean("performance_hud_enabled", enabled.isChecked())
                            .putBoolean("performance_hud_fps", fps.isChecked())
                            .putBoolean("performance_hud_frame_time", frame.isChecked())
                            .putBoolean("performance_hud_ram", ram.isChecked())
                            .putInt("performance_hud_style", compact.isChecked() ? 1 : 0)
                            .putInt("performance_hud_position", right.isChecked() ? 1 : 0)
                            .putInt("performance_hud_opacity", opacity.getProgress()).apply();
                    refreshPerformanceHud();
                }).setNegativeButton(localized("Cancelar"), null).create();
        hudDialog.show();
        styleGameDialog(hudDialog);
    }

    private android.widget.Switch hudSwitch(String label, String key, boolean defaultValue) {
        android.widget.Switch toggle = new android.widget.Switch(this);
        toggle.setText(localized(label));
        toggle.setTextColor(Color.WHITE);
        toggle.setChecked(prefs.getBoolean(key, defaultValue));
        return toggle;
    }

    private int dp(float value) { return Math.round(value * getResources().getDisplayMetrics().density); }

    private void createPerformanceHud() {
        if (prefs == null) return;
        performanceHud = new TextView(this);
        performanceHud.setTextColor(Color.WHITE);
        performanceHud.setTextSize(12);
        performanceHud.setTypeface(android.graphics.Typeface.MONOSPACE);
        performanceHud.setIncludeFontPadding(false);
        performanceHud.setPadding(dp(10), dp(7), dp(10), dp(7));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(Color.argb(190, 10, 10, 12));
        bg.setCornerRadius(dp(8));
        bg.setStroke(dp(1), Color.argb(70, 255, 255, 255));
        performanceHud.setBackground(bg);
        performanceHud.setElevation(dp(24));
        performanceHud.setClickable(false);
        performanceHud.setFocusable(false);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.LEFT);
        lp.setMargins(dp(12), dp(12), dp(12), dp(12));
        addContentView(performanceHud, lp);
        refreshPerformanceHud();
        hudHandler.post(hudUpdater);
    }

    private final Runnable hudUpdater = new Runnable() {
        @Override public void run() {
            refreshPerformanceHud();
            hudHandler.postDelayed(this, 1000);
        }
    };

    private void refreshPerformanceHud() {
        if (performanceHud == null || prefs == null) return;
        boolean enabled = prefs.getBoolean("performance_hud_enabled", false);
        boolean showFps = prefs.getBoolean("performance_hud_fps", true);
        boolean showFrame = prefs.getBoolean("performance_hud_frame_time", true);
        boolean showRam = prefs.getBoolean("performance_hud_ram", true);
        performanceHud.setVisibility(enabled && (showFps || showFrame || showRam) ? View.VISIBLE : View.GONE);
        if (!enabled) return;

        double fps = 0.0, frameMs = 0.0;
        if (showFps) {
            try { fps = nativeGetFps(); } catch (UnsatisfiedLinkError ignored) { }
        }
        if (showFrame) {
            try { frameMs = nativeGetFrameMs(); } catch (UnsatisfiedLinkError ignored) { }
        }
        double ramGb = 0.0;
        if (showRam) {
            Debug.MemoryInfo memory = new Debug.MemoryInfo();
            Debug.getMemoryInfo(memory);
            ramGb = memory.getTotalPss() / (1024.0 * 1024.0);
        }
        String fpsValue = fps > 0 ? String.format(Locale.US, "%.0f", fps) : "--";
        String frameValue = frameMs > 0 ? String.format("en".equals(prefs.getString("launcher_language", "pt")) ? Locale.US : Locale.forLanguageTag("pt-BR"), "%.1f ms", frameMs) : "-- ms";
        String ramValue = String.format("en".equals(prefs.getString("launcher_language", "pt")) ? Locale.US : Locale.forLanguageTag("pt-BR"), "%.1f GB", ramGb);
        boolean compact = prefs.getInt("performance_hud_style", 0) == 1;
        StringBuilder value = new StringBuilder();
        if (compact) {
            if (showFps) value.append("FPS ").append(fpsValue);
            if (showFrame) { if (value.length() > 0) value.append("  ·  "); value.append(localized("Quadro")).append(" ").append(frameValue); }
            if (showRam) { if (value.length() > 0) value.append("  ·  "); value.append("RAM ").append(ramValue); }
        } else {
            if (showFps) value.append("FPS  ").append(fpsValue);
            if (showFrame) { if (value.length() > 0) value.append("\n"); value.append(localized("Tempo de quadro")).append("  ").append(frameValue); }
            if (showRam) { if (value.length() > 0) value.append("\n"); value.append(localized("RAM utilizada")).append("  ").append(ramValue); }
        }
        performanceHud.setText(value.toString());
        performanceHud.setAlpha(Math.max(0.2f, Math.min(1f,
                prefs.getInt("performance_hud_opacity", 70) / 100f)));
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) performanceHud.getLayoutParams();
        boolean right = prefs.getInt("performance_hud_position", 0) == 1;
        lp.gravity = Gravity.TOP | (right ? Gravity.RIGHT : Gravity.LEFT);
        performanceHud.setLayoutParams(lp);
        android.text.SpannableString styled = new android.text.SpannableString(value.toString());
        int fpsStart = value.indexOf("FPS");
        if (fpsStart >= 0) {
            int end = compact ? value.indexOf("  ·  ") : value.indexOf("\n");
            if (end < 0) end = value.length();
            styled.setSpan(new android.text.style.ForegroundColorSpan(Color.rgb(117, 229, 154)),
                    fpsStart, Math.max(fpsStart, end), android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        }
        performanceHud.setText(styled);
    }

    private final InputManager.InputDeviceListener padListener = new InputManager.InputDeviceListener() {
        @Override public void onInputDeviceAdded(int id) { applyControlsVisibility(); }
        @Override public void onInputDeviceRemoved(int id) { applyControlsVisibility(); }
        @Override public void onInputDeviceChanged(int id) { applyControlsVisibility(); }
    };

    private boolean gamepadConnected() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            int s = d.getSources();
            boolean pad = (s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                    || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
            if (!pad) {
                try {
                    boolean[] k = d.hasKeys(KeyEvent.KEYCODE_BUTTON_A);
                    pad = k.length > 0 && k[0];
                } catch (RuntimeException ignored) {
                }
            }
            if (pad) return true;
        }
        return false;
    }

    private void applyControlsVisibility() {
        if (controls == null || prefs == null) return;
        boolean on = prefs.getBoolean("ctl_screen", true);
        boolean auto = prefs.getBoolean("ctl_auto", true);
        boolean show = on && !(auto && gamepadConnected());
        if (!show) controls.releaseAll();
        controls.setVisibility(show ? View.VISIBLE : View.GONE);
    }

    private void enterImmersiveMode() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            getWindow().setDecorFitsSystemWindows(false);
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                controller.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    /**
     * Botoes virtuais. Cada botao vira a tecla que o runtime ja le (rt_host.c, g_bind):
     * setas = direcional, Z/X/A/S = cruz/bolinha/quadrado/triangulo, Q/W = L/R,
     * Espaco = Start, V = Select, I/K/J/L = analogico (digital, tudo ou nada).
     */
    static final class TouchControls extends View {
        interface Sink {
            void down(int keyCode);
            void up(int keyCode);
        }

        static final int UP = 0, DOWN = 1, LEFT = 2, RIGHT = 3;
        static final int CROSS = 4, CIRCLE = 5, SQUARE = 6, TRIANGLE = 7;
        static final int BL = 8, BR = 9, START = 10, SELECT = 11;
        static final int SUP = 12, SDOWN = 13, SLEFT = 14, SRIGHT = 15;
        static final int N = 16;
        static final int[] CODES = {
            KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN,
            KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT,
            KeyEvent.KEYCODE_Z, KeyEvent.KEYCODE_X, KeyEvent.KEYCODE_A, KeyEvent.KEYCODE_S,
            KeyEvent.KEYCODE_Q, KeyEvent.KEYCODE_W, KeyEvent.KEYCODE_SPACE, KeyEvent.KEYCODE_V,
            KeyEvent.KEYCODE_I, KeyEvent.KEYCODE_K, KeyEvent.KEYCODE_J, KeyEvent.KEYCODE_L
        };
        private static final int[] FACE_CODES = {
            KeyEvent.KEYCODE_Z, KeyEvent.KEYCODE_X, KeyEvent.KEYCODE_A, KeyEvent.KEYCODE_S
        };
        private final int[] codes = CODES.clone();

        Sink sink;
        private final boolean[] held = new boolean[N];
        private final boolean[] want = new boolean[N];
        private final float d;
        private final int alpha;
        private final int deadZonePercent;
        private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF rect = new RectF();
        private final Path path = new Path();

        private float stickR, stickCx, stickCy, padArm, padCx, padCy, faceOff, faceR, faceCx, faceCy;
        private final RectF rL = new RectF(), rR = new RectF(), rStart = new RectF(), rSelect = new RectF();
        private float knobX, knobY;
        private float layoutOffsetX, layoutOffsetY, lastDragX, lastDragY;
        private final float[] posX = new float[N], posY = new float[N];
        private int selectedControl = -1;
        private boolean editLayout;
        private boolean stickHeld;
        // Each finger captures the control it first touches until that finger is lifted.
        private int stickPointerId = -1;
        private final int[] capturedPointerIds = new int[10];
        private final int[] capturedButtonMasks = new int[10];

        private boolean xbox;

        TouchControls(Context ctx, int opacityPercent, boolean xbox, SharedPreferences prefs) {
            super(ctx);
            Arrays.fill(capturedPointerIds, -1);
            this.xbox = xbox;
            this.editLayout = prefs.getBoolean("ctl_edit_layout", false);
            this.layoutOffsetX = prefs.getFloat("ctl_offset_x", 0f);
            this.layoutOffsetY = prefs.getFloat("ctl_offset_y", 0f);
            String layoutProfile = xbox ? "xbox" : "ps";
            for (int i = 0; i < N; i++) {
                posX[i] = prefs.getFloat("ctl_pos_" + layoutProfile + "_" + i + "_x", 0f);
                posY[i] = prefs.getFloat("ctl_pos_" + layoutProfile + "_" + i + "_y", 0f);
            }
            int deadZone = Math.max(0, Math.min(50, prefs.getInt("ctl_deadzone", 5)));
            this.deadZonePercent = deadZone;
            String profile = xbox ? "xbox" : "ps";
            codes[CROSS] = FACE_CODES[Math.max(0, Math.min(3, prefs.getInt("ctl_map_" + profile + "_cross", 0)))];
            codes[CIRCLE] = FACE_CODES[Math.max(0, Math.min(3, prefs.getInt("ctl_map_" + profile + "_circle", 1)))];
            codes[SQUARE] = FACE_CODES[Math.max(0, Math.min(3, prefs.getInt("ctl_map_" + profile + "_square", 2)))];
            codes[TRIANGLE] = FACE_CODES[Math.max(0, Math.min(3, prefs.getInt("ctl_map_" + profile + "_triangle", 3)))];
            d = ctx.getResources().getDisplayMetrics().density;
            alpha = Math.max(20, Math.min(255, Math.round(255f * opacityPercent / 100f)));
            stroke.setStyle(Paint.Style.STROKE);
            stroke.setStrokeCap(Paint.Cap.ROUND);
            fill.setStyle(Paint.Style.FILL);
            text.setTextAlign(Paint.Align.CENTER);
            text.setFakeBoldText(true);
            setClickable(true);
        }

        @Override
        protected void onSizeChanged(int w, int h, int ow, int oh) {
            stickR = 62 * d;
            stickCx = 28 * d + stickR;
            stickCy = h - 24 * d - stickR;
            padArm = 50 * d;
            padCx = stickCx;
            padCy = stickCy - stickR - 20 * d - padArm;
            faceOff = 46 * d;
            faceR = 31 * d;
            faceCx = w - 28 * d - faceOff - faceR;
            faceCy = h - 24 * d - faceOff - faceR;
            float sw = 74 * d, sh = 40 * d;
            rL.set(14 * d, 10 * d, 14 * d + sw, 10 * d + sh);
            rR.set(w - 14 * d - sw, 10 * d, w - 14 * d, 10 * d + sh);
            float pw = 70 * d, ph = 30 * d;
            float py = h - 22 * d - ph;
            rSelect.set(w / 2f - 8 * d - pw, py, w / 2f - 8 * d, py + ph);
            rStart.set(w / 2f + 8 * d, py, w / 2f + 8 * d + pw, py + ph);
        }

        private static boolean near(float x, float y, float cx, float cy, float r) {
            float dx = x - cx, dy = y - cy;
            return dx * dx + dy * dy <= r * r;
        }

        private boolean hit(RectF r, float x, float y) {
            float s = 6 * d;
            return x >= r.left - s && x <= r.right + s && y >= r.top - s && y <= r.bottom + s;
        }

        private void collect(float x, float y, boolean includeStick) {
            // Each virtual control has its own position offset; touch hitboxes follow the visuals.
            float px = x - posX[UP], py = y - posY[UP];
            if (near(px, py, padCx, padCy - padArm * 0.55f, padArm * 0.48f)
                    && Math.abs(px - padCx) < padArm * 0.55f) want[UP] = true;
            px = x - posX[DOWN]; py = y - posY[DOWN];
            if (near(px, py, padCx, padCy + padArm * 0.55f, padArm * 0.48f)
                    && Math.abs(px - padCx) < padArm * 0.55f) want[DOWN] = true;
            px = x - posX[LEFT]; py = y - posY[LEFT];
            if (near(px, py, padCx - padArm * 0.55f, padCy, padArm * 0.48f)
                    && Math.abs(py - padCy) < padArm * 0.55f) want[LEFT] = true;
            px = x - posX[RIGHT]; py = y - posY[RIGHT];
            if (near(px, py, padCx + padArm * 0.55f, padCy, padArm * 0.48f)
                    && Math.abs(py - padCy) < padArm * 0.55f) want[RIGHT] = true;

            if (includeStick && isStickHit(x, y)) updateStick(x, y);
            float fr = faceR * 1.1f;
            if (near(x - posX[TRIANGLE], y - posY[TRIANGLE], faceCx, faceCy - faceOff, fr)) want[TRIANGLE] = true;
            if (near(x - posX[CROSS], y - posY[CROSS], faceCx, faceCy + faceOff, fr)) want[CROSS] = true;
            if (near(x - posX[SQUARE], y - posY[SQUARE], faceCx - faceOff, faceCy, fr)) want[SQUARE] = true;
            if (near(x - posX[CIRCLE], y - posY[CIRCLE], faceCx + faceOff, faceCy, fr)) want[CIRCLE] = true;
            if (hit(rL, x - posX[BL], y - posY[BL])) want[BL] = true;
            if (hit(rR, x - posX[BR], y - posY[BR])) want[BR] = true;
            if (hit(rStart, x - posX[START], y - posY[START])) want[START] = true;
            if (hit(rSelect, x - posX[SELECT], y - posY[SELECT])) want[SELECT] = true;
        }

        private boolean isStickHit(float x, float y) {
            float px = x - posX[SUP], py = y - posY[SUP];
            return near(px, py, stickCx, stickCy, stickR * 1.5f);
        }

        private void updateStick(float x, float y) {
            float px = x - posX[SUP], py = y - posY[SUP];
            float dx = px - stickCx, dy = py - stickCy;
            float t = stickR * (deadZonePercent / 100f);
            if (dy < -t) want[SUP] = true;
            if (dy > t) want[SDOWN] = true;
            if (dx < -t) want[SLEFT] = true;
            if (dx > t) want[SRIGHT] = true;
            float len = (float) Math.sqrt(dx * dx + dy * dy);
            float lim = stickR * 0.7f;
            if (len > lim) { dx *= lim / len; dy *= lim / len; }
            knobX = dx;
            knobY = dy;
            stickHeld = true;
        }

        private String controlName(int id) {
            switch (id) {
                case UP: return ((GameActivity) getContext()).localized("CIMA"); case DOWN: return ((GameActivity) getContext()).localized("BAIXO"); case LEFT: return ((GameActivity) getContext()).localized("ESQUERDA"); case RIGHT: return ((GameActivity) getContext()).localized("DIREITA");
                case CROSS: return xbox ? "A" : ((GameActivity) getContext()).localized("CRUZ"); case CIRCLE: return xbox ? "B" : ((GameActivity) getContext()).localized("CÍRCULO");
                case SQUARE: return xbox ? "X" : ((GameActivity) getContext()).localized("QUADRADO"); case TRIANGLE: return xbox ? "Y" : ((GameActivity) getContext()).localized("TRIÂNGULO");
                case BL: return xbox ? "LB" : "L"; case BR: return xbox ? "RB" : "R";
                case START: return xbox ? "MENU" : "START"; case SELECT: return xbox ? "VIEW" : "SELECT";
                case SUP: return ((GameActivity) getContext()).localized("ANALÓGICO"); case SDOWN: return ((GameActivity) getContext()).localized("ANALÓGICO BAIXO");
                case SLEFT: return ((GameActivity) getContext()).localized("ANALÓGICO ESQUERDA"); default: return ((GameActivity) getContext()).localized("ANALÓGICO DIREITA");
            }
        }

        private float[] controlCenter(int id) {
            switch (id) {
                case UP: return new float[]{padCx, padCy-padArm*0.55f};
                case DOWN: return new float[]{padCx, padCy+padArm*0.55f};
                case LEFT: return new float[]{padCx-padArm*0.55f, padCy};
                case RIGHT: return new float[]{padCx+padArm*0.55f, padCy};
                case TRIANGLE: return new float[]{faceCx, faceCy-faceOff};
                case CROSS: return new float[]{faceCx, faceCy+faceOff};
                case SQUARE: return new float[]{faceCx-faceOff, faceCy};
                case CIRCLE: return new float[]{faceCx+faceOff, faceCy};
                case BL: return new float[]{rL.centerX(),rL.centerY()};
                case BR: return new float[]{rR.centerX(),rR.centerY()};
                case START: return new float[]{rStart.centerX(),rStart.centerY()};
                case SELECT: return new float[]{rSelect.centerX(),rSelect.centerY()};
                default: return new float[]{stickCx,stickCy};
            }
        }

        private int pickControl(float x, float y) {
            int[] ids = {UP,DOWN,LEFT,RIGHT,CROSS,CIRCLE,SQUARE,TRIANGLE,BL,BR,START,SELECT,SUP};
            float best = Float.MAX_VALUE; int selected = SUP;
            for (int id : ids) {
                float[] center = controlCenter(id);
                float dx = x - center[0] - posX[id], dy = y - center[1] - posY[id];
                float dist = dx*dx + dy*dy;
                if (dist < best) { best = dist; selected = id; }
            }
            return selected;
        }

        private int pointerSlot(int pointerId) {
            for (int i = 0; i < capturedPointerIds.length; i++) {
                if (capturedPointerIds[i] == pointerId) return i;
            }
            return -1;
        }

        private void captureButtons(int pointerId, float x, float y) {
            int slot = pointerSlot(pointerId);
            if (slot < 0) {
                for (int i = 0; i < capturedPointerIds.length; i++) {
                    if (capturedPointerIds[i] < 0) { slot = i; break; }
                }
            }
            if (slot < 0) return;
            Arrays.fill(want, false);
            collect(x, y, false);
            int mask = 0;
            for (int i = 0; i < N && i < 31; i++) if (want[i]) mask |= (1 << i);
            Arrays.fill(want, false);
            capturedPointerIds[slot] = pointerId;
            capturedButtonMasks[slot] = mask;
        }

        private void clearCapturedPointer(int pointerId) {
            int slot = pointerSlot(pointerId);
            if (slot >= 0) {
                capturedPointerIds[slot] = -1;
                capturedButtonMasks[slot] = 0;
            }
        }

        @Override
        public boolean onTouchEvent(MotionEvent ev) {
            int action = ev.getActionMasked();
            int actionIndex = ev.getActionIndex();
            if (action == MotionEvent.ACTION_CANCEL) {
                releaseAll();
                return true;
            }
            if (editLayout) {
                if (action == MotionEvent.ACTION_DOWN) {
                    selectedControl = pickControl(ev.getX() - layoutOffsetX, ev.getY() - layoutOffsetY);
                    lastDragX = ev.getX(); lastDragY = ev.getY();
                    releaseAll(); invalidate(); return true;
                }
                if (action == MotionEvent.ACTION_MOVE && selectedControl >= 0) {
                    posX[selectedControl] += ev.getX() - lastDragX;
                    posY[selectedControl] += ev.getY() - lastDragY;
                    lastDragX = ev.getX(); lastDragY = ev.getY();
                    String profile = xbox ? "xbox" : "ps";
                    getContext().getSharedPreferences("gow_settings", Context.MODE_PRIVATE).edit()
                            .putFloat("ctl_pos_" + profile + "_" + selectedControl + "_x", posX[selectedControl])
                            .putFloat("ctl_pos_" + profile + "_" + selectedControl + "_y", posY[selectedControl]).apply();
                    invalidate(); return true;
                }
                if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
                    selectedControl = -1; releaseAll(); invalidate(); return true;
                }
                return true;
            }

            if (action == MotionEvent.ACTION_UP) {
                // Always release every virtual key when the final finger is lifted.
                releaseAll();
                return true;
            }

            if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
                float x = ev.getX(actionIndex) - layoutOffsetX;
                float y = ev.getY(actionIndex) - layoutOffsetY;
                int pointerId = ev.getPointerId(actionIndex);
                if (stickPointerId < 0 && isStickHit(x, y)) {
                    stickPointerId = pointerId;
                } else {
                    captureButtons(pointerId, x, y);
                }
            }

            if (action == MotionEvent.ACTION_POINTER_UP) {
                int pointerId = ev.getPointerId(actionIndex);
                clearCapturedPointer(pointerId);
                if (pointerId == stickPointerId) stickPointerId = -1;
            }

            Arrays.fill(want, false);
            stickHeld = false;
            int skip = action == MotionEvent.ACTION_POINTER_UP ? actionIndex : -1;
            for (int i = 0; i < ev.getPointerCount(); i++) {
                if (i == skip) continue;
                int pointerId = ev.getPointerId(i);
                float x = ev.getX(i) - layoutOffsetX;
                float y = ev.getY(i) - layoutOffsetY;
                if (pointerId == stickPointerId) {
                    // The analog remains tied to its original finger across the screen.
                    updateStick(x, y);
                } else {
                    // Buttons remain tied to the original finger/control, not hover-dragged to another.
                    int slot = pointerSlot(pointerId);
                    if (slot >= 0) {
                        int mask = capturedButtonMasks[slot];
                        for (int b = 0; b < N && b < 31; b++) {
                            if ((mask & (1 << b)) != 0) want[b] = true;
                        }
                    }
                }
            }
            if (!stickHeld) { knobX = 0; knobY = 0; }
            apply();
            return true;
        }

        private void apply() {
            if (sink != null) {
                for (int i = 0; i < N; i++) {
                    if (want[i] && !held[i]) {
                        held[i] = true;
                        if (getContext().getSharedPreferences("gow_settings", Context.MODE_PRIVATE).getBoolean("ctl_vibration", true)) {
                            performHapticFeedback(android.view.HapticFeedbackConstants.KEYBOARD_TAP);
                        }
                        sink.down(codes[i]);
                    } else if (!want[i] && held[i]) {
                        held[i] = false;
                        sink.up(codes[i]);
                    }
                }
            }
            invalidate();
        }

        void setEditMode(boolean enabled) {
            this.editLayout = enabled;
            getContext().getSharedPreferences("gow_settings", Context.MODE_PRIVATE).edit().putBoolean("ctl_edit_layout", enabled).apply();
            selectedControl = -1; releaseAll(); invalidate();
        }

        void setControllerStyle(boolean useXbox) {
            this.xbox = useXbox;
            String profile = useXbox ? "xbox" : "ps";
            SharedPreferences p = getContext().getSharedPreferences("gow_settings", Context.MODE_PRIVATE);
            codes[CROSS] = FACE_CODES[Math.max(0, Math.min(3, p.getInt("ctl_map_" + profile + "_cross", 0)))];
            codes[CIRCLE] = FACE_CODES[Math.max(0, Math.min(3, p.getInt("ctl_map_" + profile + "_circle", 1)))];
            codes[SQUARE] = FACE_CODES[Math.max(0, Math.min(3, p.getInt("ctl_map_" + profile + "_square", 2)))];
            codes[TRIANGLE] = FACE_CODES[Math.max(0, Math.min(3, p.getInt("ctl_map_" + profile + "_triangle", 3)))];
            for (int i = 0; i < N; i++) {
                posX[i] = p.getFloat("ctl_pos_" + profile + "_" + i + "_x", 0f);
                posY[i] = p.getFloat("ctl_pos_" + profile + "_" + i + "_y", 0f);
            }
            invalidate();
        }

        void releaseAll() {
            Arrays.fill(want, false);
            Arrays.fill(capturedPointerIds, -1);
            Arrays.fill(capturedButtonMasks, 0);
            stickPointerId = -1;
            stickHeld = false;
            knobX = 0;
            knobY = 0;
            apply();
        }

        private int a(float k) {
            return Math.round(alpha * k);
        }

        @Override
        protected void onDraw(Canvas c) {
            c.save();
            c.translate(layoutOffsetX, layoutOffsetY);
            stroke.setStrokeWidth(2.5f * d);
            c.save(); c.translate(posX[SUP],posY[SUP]);
            stroke.setColor(Color.argb(a(0.8f),255,255,255));
            c.drawCircle(stickCx,stickCy,stickR,stroke);
            fill.setColor(Color.argb(a(stickHeld?0.55f:0.25f),255,255,255));
            c.drawCircle(stickCx+knobX,stickCy+knobY,stickR*0.45f,fill); c.restore();
            float aw=16*d,in=12*d;
            c.save(); c.translate(posX[UP],posY[UP]); arm(c,held[UP],padCx-aw,padCy-padArm,padCx+aw,padCy-in); c.restore();
            c.save(); c.translate(posX[DOWN],posY[DOWN]); arm(c,held[DOWN],padCx-aw,padCy+in,padCx+aw,padCy+padArm); c.restore();
            c.save(); c.translate(posX[LEFT],posY[LEFT]); arm(c,held[LEFT],padCx-padArm,padCy-aw,padCx-in,padCy+aw); c.restore();
            c.save(); c.translate(posX[RIGHT],posY[RIGHT]); arm(c,held[RIGHT],padCx+in,padCy-aw,padCx+padArm,padCy+aw); c.restore();
            c.save(); c.translate(posX[TRIANGLE],posY[TRIANGLE]); face(c,TRIANGLE,faceCx,faceCy-faceOff); c.restore();
            c.save(); c.translate(posX[CROSS],posY[CROSS]); face(c,CROSS,faceCx,faceCy+faceOff); c.restore();
            c.save(); c.translate(posX[SQUARE],posY[SQUARE]); face(c,SQUARE,faceCx-faceOff,faceCy); c.restore();
            c.save(); c.translate(posX[CIRCLE],posY[CIRCLE]); face(c,CIRCLE,faceCx+faceOff,faceCy); c.restore();
            c.save(); c.translate(posX[BL],posY[BL]); pill(c,rL,xbox?"LB":"L",held[BL],12*d,16*d); c.restore();
            c.save(); c.translate(posX[BR],posY[BR]); pill(c,rR,xbox?"RB":"R",held[BR],12*d,16*d); c.restore();
            c.save(); c.translate(posX[SELECT],posY[SELECT]); pill(c,rSelect,xbox?"VIEW":"SELECT",held[SELECT],15*d,11*d); c.restore();
            c.save(); c.translate(posX[START],posY[START]); pill(c,rStart,xbox?"MENU":"START",held[START],15*d,11*d); c.restore();
            if(editLayout) {
                text.setColor(Color.argb(230,255,255,255)); text.setTextSize(12*d);
                c.drawText(((GameActivity) getContext()).localized("TOQUE E ARRASTE CADA BOTÃO"),getWidth()/2f,28*d,text);
                if(selectedControl>=0) { text.setColor(Color.rgb(235,70,85)); c.drawText(((GameActivity) getContext()).localized("EDITANDO: ")+controlName(selectedControl),getWidth()/2f,46*d,text); }
            }
            c.restore();
        }

        private void arm(Canvas c, boolean on, float l, float t, float r, float b) {
            rect.set(l, t, r, b);
            fill.setColor(Color.argb(a(on ? 0.6f : 0.22f), 255, 255, 255));
            c.drawRoundRect(rect, 6 * d, 6 * d, fill);
            stroke.setColor(Color.argb(a(0.7f), 255, 255, 255));
            c.drawRoundRect(rect, 6 * d, 6 * d, stroke);
        }

        private void face(Canvas c, int which, float cx, float cy) {
            boolean on = held[which];
            fill.setColor(Color.argb(a(on ? 0.6f : 0.22f), 255, 255, 255));
            c.drawCircle(cx, cy, faceR, fill);
            stroke.setColor(Color.argb(a(0.7f), 255, 255, 255));
            c.drawCircle(cx, cy, faceR, stroke);
            float s = faceR * 0.4f;
            if (xbox) {
                int r = 255, gg = 255, b = 255;
                String letter;
                if (which == CROSS) { letter = "A"; r = 110; gg = 210; b = 130; }
                else if (which == CIRCLE) { letter = "B"; r = 255; gg = 110; b = 110; }
                else if (which == SQUARE) { letter = "X"; r = 90; gg = 150; b = 255; }
                else { letter = "Y"; r = 240; gg = 200; b = 60; }
                text.setColor(Color.argb(a(1f), r, gg, b));
                text.setTextSize(faceR * 0.9f);
                float ty = cy - (text.descent() + text.ascent()) / 2f;
                c.drawText(letter, cx, ty, text);
            } else if (which == CROSS) {
                stroke.setColor(Color.argb(a(1f), 120, 170, 255));
                c.drawLine(cx - s, cy - s, cx + s, cy + s, stroke);
                c.drawLine(cx - s, cy + s, cx + s, cy - s, stroke);
            } else if (which == CIRCLE) {
                stroke.setColor(Color.argb(a(1f), 255, 110, 110));
                c.drawCircle(cx, cy, s, stroke);
            } else if (which == SQUARE) {
                stroke.setColor(Color.argb(a(1f), 240, 150, 220));
                c.drawRect(cx - s, cy - s, cx + s, cy + s, stroke);
            } else {
                stroke.setColor(Color.argb(a(1f), 110, 210, 160));
                path.reset();
                path.moveTo(cx, cy - s * 1.1f);
                path.lineTo(cx + s * 1.05f, cy + s * 0.8f);
                path.lineTo(cx - s * 1.05f, cy + s * 0.8f);
                path.close();
                c.drawPath(path, stroke);
            }
        }

        private void pill(Canvas c, RectF r, String label, boolean on, float radius, float textSize) {
            fill.setColor(Color.argb(a(on ? 0.6f : 0.22f), 255, 255, 255));
            c.drawRoundRect(r, radius, radius, fill);
            stroke.setColor(Color.argb(a(0.7f), 255, 255, 255));
            c.drawRoundRect(r, radius, radius, stroke);
            text.setColor(Color.argb(a(1f), 255, 255, 255));
            text.setTextSize(textSize);
            float y = r.centerY() - (text.descent() + text.ascent()) / 2f;
            c.drawText(label, r.centerX(), y, text);
        }
    }
}
