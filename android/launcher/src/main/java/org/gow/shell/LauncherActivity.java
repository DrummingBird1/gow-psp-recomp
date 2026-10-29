package org.gow.shell;

import android.app.Activity;
import android.app.DownloadManager;
import android.os.Environment;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.database.Cursor;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.provider.OpenableColumns;
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
import androidx.core.content.FileProvider;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import org.json.JSONObject;

/**
 * God of War PSP Recomp Android launcher (layout horizontal, 6 telas).
 *
 * As opções de controles na tela são salvas em gow_settings e aplicadas pela GameActivity.
 * As opções gráficas suportadas pelo runtime gravam argumentos no args.txt.
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
    private static final int PICK_GAME_ISO = 401;
    private static final int PICK_IMAGE_BIN = 402;
    private static final String VERSION = "v1.0.1";
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
        try {
            getWindow().setStatusBarColor(BG);
            getWindow().setNavigationBarColor(BG);
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LOW_PROFILE);
            pad = dp(20);
            installBundledImages();
            if ("FULLSCREEN".equals(prefs().getString("screen_mode", "16:9"))
                    && !readArgTokens().contains("--stretch")) writeArgs();
            showMain();
        } catch (Throwable error) {
            android.util.Log.e("GOW_LAUNCHER_STARTUP", "LauncherActivity failed during startup", error);
            TextView diagnostic = new TextView(this);
            diagnostic.setTextColor(Color.WHITE);
            diagnostic.setBackgroundColor(Color.rgb(24, 18, 20));
            diagnostic.setTextSize(14);
            diagnostic.setPadding(32, 32, 32, 32);
            diagnostic.setText("Falha ao abrir o launcher do Ghost of Sparta.\n\n"
                    + error.getClass().getName() + ": " + String.valueOf(error.getMessage())
                    + "\n\nConsulte o logcat pelo marcador GOW_LAUNCHER_STARTUP.");
            diagnostic.setTextIsSelectable(true);
            super.setContentView(diagnostic);
        }
    }

    @Override protected void onResume() {
        super.onResume();
        if ("main".equals(current) && status != null) refreshStatus();
        String pendingPath = prefs().getString("pending_update_apk", "");
        if (!pendingPath.isEmpty() && (Build.VERSION.SDK_INT < 26 || getPackageManager().canRequestPackageInstalls())) {
            prefs().edit().remove("pending_update_apk").apply();
            File pendingApk = new File(pendingPath);
            if (pendingApk.isFile()) launchApkInstaller(pendingApk);
        }
    }

    @Override public void onBackPressed() {
        if (!"main".equals(current)) showMain();
        else super.onBackPressed();
    }

    // ------------------------------------------------------------------ tema (so visual)

    /** Coloca o fundo tematico atras de qualquer tela do launcher. */
    @Override public void setContentView(View view) {
        android.widget.FrameLayout f = new android.widget.FrameLayout(this);
        f.addView(new ThemeBackground(this), new android.widget.FrameLayout.LayoutParams(
                android.view.ViewGroup.LayoutParams.MATCH_PARENT,
                android.view.ViewGroup.LayoutParams.MATCH_PARENT));
        f.addView(view, new android.widget.FrameLayout.LayoutParams(
                android.view.ViewGroup.LayoutParams.MATCH_PARENT,
                android.view.ViewGroup.LayoutParams.MATCH_PARENT));
        super.setContentView(f);
    }

    /** Fundo original: brasa vermelha embaixo, fagulhas, corrente na borda direita e faixa grega no rodape. */
    private final class ThemeBackground extends View {
        private final android.graphics.Paint pt = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
        private final android.graphics.Path path = new android.graphics.Path();
        private final android.graphics.RectF rc = new android.graphics.RectF();
        private android.graphics.RadialGradient glow;
        private float[] spark = new float[0];

        ThemeBackground(android.content.Context c) {
            super(c);
        }

        @Override protected void onSizeChanged(int w, int h, int ow, int oh) {
            float d = getResources().getDisplayMetrics().density;
            glow = new android.graphics.RadialGradient(w * 0.5f, h * 1.05f, Math.max(w, h) * 0.75f,
                    new int[] { Color.argb(150, 120, 20, 32), Color.argb(60, 90, 14, 24), Color.TRANSPARENT },
                    new float[] { 0f, 0.5f, 1f }, android.graphics.Shader.TileMode.CLAMP);
            java.util.Random rnd = new java.util.Random(7);
            int n = 40;
            spark = new float[n * 4];
            for (int i = 0; i < n; i++) {
                spark[i * 4] = rnd.nextFloat() * w;
                spark[i * 4 + 1] = h * (0.08f + 0.9f * (float) Math.sqrt(rnd.nextFloat()));
                spark[i * 4 + 2] = d * (0.8f + rnd.nextFloat() * 1.8f);
                spark[i * 4 + 3] = 50 + rnd.nextFloat() * 110;
            }
        }

        @Override protected void onDraw(android.graphics.Canvas c) {
            float d = getResources().getDisplayMetrics().density;
            float w = getWidth(), h = getHeight();
            c.drawColor(BG);
            if (glow != null) {
                pt.setStyle(android.graphics.Paint.Style.FILL);
                pt.setShader(glow);
                c.drawRect(0, 0, w, h, pt);
                pt.setShader(null);
            }
            pt.setStyle(android.graphics.Paint.Style.FILL);
            for (int i = 0; i + 3 < spark.length; i += 4) {
                pt.setColor(Color.argb((int) spark[i + 3], 200, 40, 55));
                c.drawCircle(spark[i], spark[i + 1], spark[i + 2], pt);
            }
            // corrente na borda direita
            pt.setStyle(android.graphics.Paint.Style.STROKE);
            pt.setStrokeWidth(3 * d);
            pt.setColor(Color.argb(70, 200, 32, 48));
            float cx = w - 6 * d;
            float y = -4 * d;
            boolean front = true;
            while (y < h) {
                if (front) {
                    rc.set(cx - 9 * d, y, cx + 9 * d, y + 30 * d);
                    c.drawOval(rc, pt);
                    y += 22 * d;
                } else {
                    rc.set(cx - 2 * d, y, cx + 2 * d, y + 22 * d);
                    c.drawRoundRect(rc, 2 * d, 2 * d, pt);
                    y += 14 * d;
                }
                front = !front;
            }
            // faixa grega (meandro) no rodape
            pt.setStrokeWidth(1.5f * d);
            pt.setColor(Color.argb(80, 200, 32, 48));
            float bh = 11 * d, y0 = h - 15 * d, step = 22 * d;
            for (float x = 4 * d; x < w; x += step) {
                path.reset();
                path.moveTo(x, y0 + bh);
                path.lineTo(x, y0);
                path.lineTo(x + 14 * d, y0);
                path.lineTo(x + 14 * d, y0 + bh * 0.65f);
                path.lineTo(x + 6 * d, y0 + bh * 0.65f);
                path.lineTo(x + 6 * d, y0 + bh * 0.35f);
                c.drawPath(path, pt);
            }
        }
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

    private String localized(String value) {
        if (!"en".equals(prefs().getString("launcher_language", "pt"))) return value;
        switch (value) {
            case "Início": return "Home";
            case "Gráficos": return "Graphics";
            case "HUD": return "HUD";
            case "Desempenho": return "Performance";
            case "Controles": return "Controls";
            case "Extras": return "Extras";
            case "Sobre": return "About";
            case "Voltar": return "Back";
            case "Voltar sem alterar": return "Back without changes";
            case "JOGAR": return "PLAY";
            case "Jogar": return "Play";
            case "Aplicar": return "Apply";
            case "Cancelar": return "Cancel";
            case "Restaurar": return "Restore";
            case "Restaurar padrões": return "Restore defaults";
            case "Idioma": return "Language";
            case "Idioma da interface do launcher": return "Launcher interface language";
            case "Vibração do controle": return "Controller vibration";
            case "Resposta tátil ao tocar nos controles virtuais": return "Haptic feedback when using virtual controls";
            case "Verificar atualizações": return "Check for updates";
            case "Consultar a versão mais recente do APK no GitHub": return "Check GitHub for the latest APK";
            case "Verificar": return "Check";
            case "Opacidade dos controles": return "Control opacity";
            case "Controles na tela": return "On-screen controls";
            case "Direção, analógico e botões": return "D-pad, analog stick and buttons";
            case "Estilo dos botões": return "Button style";
            case "Automático": return "Automatic";
            case "Oculta os controles na tela ao conectar um controle": return "Hide on-screen controls when a controller connects";
            case "Controle detectado": return "Detected controller";
            case "Nenhum controle físico": return "No physical controller";
            case "Não conectado": return "Not connected";
            case "Zona morta do analógico": return "Analog stick deadzone";
            case "Menor valor responde com menos movimento": return "Lower values respond to smaller movements";
            case "Editor de controles individuais": return "Individual control editor";
            case "Personalize a posição dos botões durante a partida": return "Customize button positions during gameplay";
            case "Como usar": return "How to use";
            case "Posições originais restauradas.": return "Default positions restored.";
            case "Saves": return "Saves";
            case "Backup e restauração de ms0/PSP/SAVEDATA": return "Back up and restore ms0/PSP/SAVEDATA";
            case "Logs": return "Logs";
            case "Ver, compartilhar ou limpar o log.txt": return "View, share or clear log.txt";
            case "Argumentos avançados": return "Advanced arguments";
            case "Ver e editar o args.txt": return "View and edit args.txt";
            case "Arquivos do jogo": return "Game files";
            case "Importar ou substituir game.iso e image.bin": return "Import or replace game.iso and image.bin";
            case "Importar somente a ISO; image.bin vem no APK": return "Import only the ISO; image.bin is included in the APK";
            case "image.bin — incluído no APK e instalado automaticamente": return "image.bin — included in APK and installed automatically";
            case "Para jogar, importe somente a ISO do jogo escolhido. O image.bin correto já vem no APK.": return "To play, import only the ISO for the selected game. The matching image.bin is included in the APK.";
            case "Gerenciar": return "Manage";
            case "Volta todas as configurações ao original": return "Reset all settings to defaults";
            case "Português": return "Portuguese";
            case "English": return "English";
            case "Memória (RAM)": return "Memory (RAM)";
            case "Tempo de quadro": return "Frame time";
            case "Mostrar HUD em jogo": return "Show in-game HUD";
            case "Mostrar HUD de desempenho": return "Show performance HUD";
            case "FPS": return "FPS";
            case "Em linhas": return "Multiple lines";
            case "Uma linha": return "Single line";
            case "Esq.": return "Left";
            case "Dir.": return "Right";
            case "Opacidade do HUD": return "HUD opacity";
            case "Concluir edição": return "Finish editing";
            case "Editar controles na tela": return "Edit on-screen controls";
            case "Editar HUD de FPS": return "Edit FPS HUD";
            case "Sair do game": return "Exit game";
            case "Toque em Concluir edição para voltar a jogar": return "Tap Finish editing to return to the game";
            case "C O M O  U S A R": return "H O W  T O  U S E";
            case "Editor de ": return "Control ";
            case "controles": return "editor";
            case "Dica: desconecte o controle físico ou desligue o Automático, senão os botões na tela ficam ocultos.": return "Tip: disconnect the physical controller or turn off Automatic, otherwise on-screen buttons stay hidden.";
            case "Em breve...": return "Coming soon...";
            case "G O D   O F   W A R": return "G O D   O F   W A R";
            case "Launcher oficial · ": return "Official launcher · ";
            case "By: flavinxs0": return "By: flavinxs0";
            case "Cada opção vira um argumento do runtime. Escalas altas podem pesar na GPU.": return "Each option becomes a runtime argument. High scales may burden the GPU.";
            case "16:9 = padrão do PSP · 4:3 = clássico · Esticada = tela toda": return "16:9 = PSP default · 4:3 = classic · Stretched = full screen";
            case "60 FPS é o padrão e não grava --fps.": return "60 FPS is the default and does not write --fps.";
            case "Painel compacto durante o jogo. FPS e tempo de quadro usam medições nativas; RAM mostra a memória PSS do processo Android.": return "Compact in-game panel. FPS and frame time use native measurements; RAM shows Android process PSS memory.";
            case "MAPEAMENTO PERSONALIZADO": return "CUSTOM MAPPING";
            case "Perfil ": return "Profile ";
            case "Ação atribuída": return "Assigned action";
            case "Argumentos atuais (args.txt)": return "Current arguments (args.txt)";
            case "Disco": return "Disc";
            case "Android/data/org.gow.shell/files": return "Android/data/org.gow.shell/files";
            case "Launcher não oficial · ": return "Unofficial launcher · ";
            case "RESTAURAR PADRÕES": return "RESTORE DEFAULTS";
            case "Isso redefine proporção, escala, FXAA, limite de FPS e HUD. Outros argumentos do args.txt serão mantidos.": return "This resets aspect ratio, scale, FXAA, FPS limit and HUD. Other args.txt arguments will be kept.";
            case "Confirmar restauração": return "Confirm reset";
            case "As configurações gráficas voltarão aos valores originais do launcher.": return "Graphics settings will return to the launcher's defaults.";
            case "Aviso": return "Warning";
            case "Projeto não oficial, sem afiliação com a Sony.\n": return "Unofficial project, not affiliated with Sony.\n";
            case "Pasta: Android/data/org.gow.shell/files": return "Folder: Android/data/org.gow.shell/files";

            case "Verificador de atualizações": return "Update checker";
            case "Não foi possível consultar o GitHub.\n\nDetalhe: ": return "Could not check GitHub.\n\nDetails: ";
            case "Permitir instalação": return "Allow installation";
            case "Falha na atualização": return "Update failed";
            case "RESTAURAR SAVES": return "RESTORE SAVES";
            case "LOG": return "LOG";
            case "FECHAR": return "CLOSE";
            case "LIMPAR LOG": return "CLEAR LOG";
            case "ARGUMENTOS AVANÇADOS": return "ADVANCED ARGUMENTS";
            case "LICENÇAS": return "LICENSES";

            case "Portuguese": return "Português";
            case "Toque em Editar controles na tela": return "Tap Edit on-screen controls";
            case "Escolha PlayStation ou Xbox e arraste cada botão": return "Choose PlayStation or Xbox and drag each button";
            case "Desempenho / HUD": return "Performance / HUD";
            case "Instalar arquivos do jogo": return "Install game files";
            case "O jogo pode não iniciar sem os arquivos necessários.": return "The game may not start without the required files.";
            case "Instalar arquivos": return "Install files";
            case "Selecione seus próprios arquivos. O app fará uma cópia na pasta privada dele.": return "Select your own files. The app will copy them to its private folder.";
            case "Fechar": return "Close";
            case "Não foi possível abrir o seletor de arquivos.": return "Could not open the file picker.";
            case "Não foi possível criar a pasta do app.": return "Could not create the app folder.";
            case "Não foi possível limpar a cópia temporária.": return "Could not clear the temporary copy.";
            case "O seletor não forneceu acesso ao arquivo.": return "The file picker did not grant access to the file.";
            case "O arquivo copiado está vazio ou incompleto.": return "The copied file is empty or incomplete.";
            case "Não foi possível preparar o backup.": return "Could not prepare the backup.";
            case "Não foi possível preservar o arquivo anterior.": return "Could not preserve the previous file.";
            case "Não foi possível concluir a importação.": return "Could not complete the import.";
            case "Verificando atualizações...": return "Checking for updates...";
            case "A API não retornou uma versão publicada.": return "The API did not return a published release.";
            case "Você está na versão mais recente publicada.": return "You are on the latest published version.";
            case "Baixar atualização": return "Download update";
            case "Confira sua conexão ou abra a página de releases.": return "Check your connection or open the releases page.";
            case "Baixando atualização...": return "Downloading update...";
            case "O APK baixado está incompleto.": return "The downloaded APK is incomplete.";
            case "O arquivo baixado não é um APK válido.": return "The downloaded file is not a valid APK.";
            case "Abrir permissões": return "Open permissions";
            case "Ative a permissão de instalação nas configurações do Android.": return "Enable install permission in Android settings.";
            case "Não foi possível abrir o instalador do Android.": return "Could not open the Android installer.";
            case "Proporção de tela": return "Screen aspect ratio";
            case "Posição": return "Position";
            case "Restaurar posição": return "Restore position";
            case "Voltar ao layout original": return "Restore original layout";
            case "Zona morta do analógico, de zero a cinquenta por cento": return "Analog stick deadzone, from zero to fifty percent";
            case "Opacidade dos controles, de 20 a 100 por cento": return "Control opacity, from 20 to 100 percent";
            case "Círculo": return "Circle";
            case "Triângulo": return "Triangle";
            case "Escolha qual ação cada botão representa. Altere o perfil na tela Controles para configurar o outro layout.": return "Choose the action for each button. Change the profile on the Controls screen to configure the other layout.";
            case "Restaurar padrão": return "Restore default";
            case "Mapeamento padrão restaurado. Vale ao abrir o jogo novamente.": return "Default mapping restored. It takes effect the next time you open the game.";
            case "Mapeamento dos botões": return "Button mapping";
            case "Automático ligado: os controles na tela somem ao conectar o controle.": return "Automatic enabled: on-screen controls disappear when a controller connects.";
            case "Automático desligado: os controles na tela ficam sempre visíveis.": return "Automatic disabled: on-screen controls remain visible.";
            case "Nenhum save encontrado ainda.": return "No save data found yet.";
            case "Falha ao criar o backup.": return "Failed to create backup.";
            case "Nenhum backup encontrado.": return "No backup found.";
            case "Falha ao restaurar.": return "Restore failed.";
            case "Nenhum log encontrado ainda. Inicie o jogo para gerar o registro.": return "No log found yet. Start the game to generate one.";
            case "Nenhum log para limpar.": return "No log to clear.";
            case "Apagar o log.txt da última sessão?": return "Delete the log.txt from the last session?";
            case "Não foi possível apagar o log.": return "Could not delete the log.";
            case "Edite com cuidado. A proporção de tela (--aspect) é controlada pela tela Gráficos.": return "Edit carefully. Screen aspect ratio (--aspect) is controlled on the Graphics screen.";
            case "Padrões restaurados.": return "Defaults restored.";
            case "Aparelho": return "Device";
            case "Arquitetura": return "Architecture";
            case "Código-fonte": return "Source code";
            case "Projeto no GitHub": return "Project on GitHub";
            case "Bibliotecas de código aberto": return "Open-source libraries";
            case "A lista completa de componentes está no repositório do projeto.": return "The complete component list is in the project repository.";

            case "Escala de resolução": return "Resolution scale";
            case "Esticada": return "Stretched";
            case "Limite de FPS": return "FPS limit";
            case "Estilo": return "Style";
            case "Opacidade": return "Opacity";
            case "Salvo. Vale ao tocar em JOGAR.\nargs.txt: ": return "Saved. Takes effect when you tap PLAY.\nargs.txt: ";
            case "pronto": return "ready";
            case "pendente": return "missing";
            case "Tentar jogar": return "Try to play";
            case "Selecionar game.iso": return "Select game.iso";
            case "Selecionar image.bin": return "Select image.bin";
            case "OK": return "OK";
            case "Abrir release": return "Open release";
            case "Abrir releases": return "Open releases";
            case "Ver release": return "View release";
            case "Não foi possível baixar o APK: ": return "Could not download APK: ";
            case "O APK foi baixado. Permita que este aplicativo instale APKs nas configurações do Android; depois volte e toque em Verificar atualizações para concluir.": return "The APK has been downloaded. Allow this app to install APKs in Android settings, then return and tap Check for updates to finish.";
            case "Nenhum navegador encontrado.": return "No browser found.";
            case "Salvo. Vale ao tocar em JOGAR.": return "Saved. Takes effect when you tap PLAY.";
            case "Cruz": return "Cross";
            case "Quadrado": return "Square";
            case "Backup": return "Back up";
            case "Ver": return "View";
            case "Compartilhar": return "Share";
            case "Limpar": return "Clear";
            case "Editar": return "Edit";
            case "Idioma Português selecionado.": return "Portuguese selected.";
            case "Saves restaurados.": return "Save data restored.";
            case "Log apagado.": return "Log deleted.";
            case "args.txt salvo.": return "args.txt saved.";
            case "Este app usa a biblioteca SDL (licença zlib).\n\n": return "This app uses the SDL library (zlib license).\n\n";
            case "Pasta: Android/data/org.gow.shell/files\n": return "Folder: Android/data/org.gow.shell/files\n";
            case "game.iso: ": return "game.iso: ";
            case " pronto": return " ready";
            case " pendente": return " missing";
            case "image.bin — ": return "image.bin — ";
            case "Restaurar o backup ": return "Restore backup ";
            case "Falha ao importar ": return "Failed to import ";
            case " instalado (": return " installed (";
            case " MB).": return " MB).";
            case "Selecione um arquivo ": return "Select a ";
            case " para ": return " file for ";
            case "Copiando ": return "Copying ";
            case " para o armazenamento do app...": return " to app storage...";
            case "Falha ao baixar o APK: ": return "Failed to download APK: ";
            case "Restaurar o backup": return "Restore backup";
            case "Versão": return "Version";
            case "O app fará uma cópia na pasta privada dele.": return "The app will copy it to its private folder.";


            case "Exibição compacta em uma linha": return "Compact single-line display";
            case "Posicionar à direita": return "Align right";
            case "Entre no modo de edição e arraste o botão desejado. Toque em Concluir para salvar.": return "Enter edit mode and drag the button you want. Tap Finish to save.";
            case "Salvar": return "Save";
            case "Backup criado: backup_saves/": return "Backup created: backup_saves/";
            case "Nenhum log encontrado ainda.": return "No log found yet.";


            case "CANCELAR": return "CANCEL";
            case "RESTAURAR": return "RESTORE";
            case "LIMPAR": return "CLEAR";
            case "SALVAR": return "SAVE";
            case "Os saves atuais serão sobrescritos.": return "Current save data will be overwritten.";
            case "Compartilhar log": return "Share log";


            case "Pendente": return "Missing";
            case "Pronto (%.1f GB)": return "Ready (%.1f GB)";
            case "Pronto (%.1f MB)": return "Ready (%.1f MB)";
            case "image.bin: ": return "image.bin: ";
            case "Selecione ": return "Select ";
            case "(vazio)": return "(empty)";

            case "Abra uma partida": return "Start a game";
            case "Puxe a aba lateral na borda direita": return "Pull the side tab on the right edge";
            case "Entendi": return "Got it";
            case "Vale na próxima vez que tocar em JOGAR": return "Takes effect the next time you tap PLAY";
            case "Não foi possível criar o backup.": return "Could not create the backup.";
            case "Quadro": return "Frame time";
            case "RAM utilizada": return "RAM used";
            case "● Conectado": return "● Connected";
            case "Editor de controles na tela": return "On-screen controls editor";
            case "Licenças": return "Licenses";
            case "Não foi possível ler o log: ": return "Could not read the log: ";
            case ". Escolha qual ação cada botão representa. Altere o perfil na tela Controles para configurar o outro layout.": return ". Choose what each button does. Change the profile on the Controls screen to configure the other layout.";
            case "Versão instalada: ": return "Installed version: ";
            case "Última release: ": return "Latest release: ";

            case "Nova release: ": return "New release: ";
            case "Baixe o APK para atualizar pelo instalador do Android.": return "Download the APK to update using the Android installer.";
            case "Este app usa a biblioteca SDL (licença zlib).\n\nA lista completa de componentes está no repositório do projeto.": return "This app uses the SDL library (zlib license).\n\nThe full component list is available in the project repository.";
            case "É necessário ter sua própria cópia do jogo.": return "You must provide your own copy of the game.";
            case "Projeto não oficial, sem afiliação com a Sony.\nÉ necessário ter sua própria cópia do jogo.": return "Unofficial project, not affiliated with Sony.\nYou must provide your own copy of the game.";
            case "Folder: Android/data/org.gow.shell/files": return "Folder: Android/data/org.gow.shell/files";
            case "Português selecionado.": return "Portuguese selected.";
            case "Este app usa a biblioteca SDL (licença zlib).": return "This app uses the SDL library (zlib license).";

            default: return value;
        }
    }

    private TextView tv(String value, int size, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(localized(value));
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
        s.setBackgroundColor(Color.TRANSPARENT);
        return s;
    }

    private AlertDialog.Builder dialog() {
        return new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert);
    }

    private void showControlEditorHelp() {
        LinearLayout root = column();
        root.setPadding(dp(22), dp(20), dp(22), dp(18));
        root.setBackground(shape(CARD, BRD, 26));

        TextView eyebrow = tv("C O M O  U S A R", 12, MUTED, true);
        eyebrow.setPadding(0, 0, 0, dp(8));
        root.addView(eyebrow, matchWrap());

        LinearLayout titleRow = rowLayout();
        TextView title = tv("Editor de ", 25, TEXT, true);
        TextView accent = tv("controles", 25, ROSE, true);
        titleRow.addView(title);
        titleRow.addView(accent);
        root.addView(titleRow, withBottom(dp(16)));

        String[] steps = {
            "Abra uma partida",
            "Puxe a aba lateral na borda direita",
            "Toque em Editar controles na tela",
            "Escolha PlayStation ou Xbox e arraste cada botão",
            "Toque em Concluir edição para voltar a jogar"
        };
        for (int i = 0; i < steps.length; i++) {
            LinearLayout item = rowLayout();
            item.setPadding(dp(12), dp(11), dp(14), dp(11));
            item.setBackground(shape(Color.rgb(39, 39, 44), BRD, 32));
            TextView number = tv(String.valueOf(i + 1), 16, TEXT, true);
            number.setGravity(Gravity.CENTER);
            number.setBackground(shape(RED, 0, 40));
            LinearLayout.LayoutParams np = lp(dp(34), dp(34));
            np.rightMargin = dp(12);
            item.addView(number, np);
            TextView text = tv(steps[i], 15, TEXT, false);
            text.setGravity(Gravity.CENTER_VERTICAL);
            item.addView(text, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
            LinearLayout.LayoutParams ip = matchWrap();
            ip.bottomMargin = dp(8);
            root.addView(item, ip);
        }

        TextView tip = tv("Dica: desconecte o controle físico ou desligue o Automático, senão os botões na tela ficam ocultos.", 14, GREEN, false);
        tip.setPadding(dp(14), dp(14), dp(14), dp(14));
        tip.setBackground(shape(Color.rgb(25, 25, 29), BRD, 22));
        LinearLayout.LayoutParams tipLp = withBottom(dp(14));
        tipLp.topMargin = dp(4);
        root.addView(tip, tipLp);

        TextView understood = button("Entendi", true, () -> { });
        understood.setGravity(Gravity.CENTER);
        root.addView(understood, matchWrap());

        ScrollView scroll = newScroll();
        scroll.setFillViewport(true);
        scroll.addView(root);
        AlertDialog dlg = dialog().create();
        understood.setOnClickListener(v -> dlg.dismiss());
        dlg.setView(scroll, dp(8), dp(8), dp(8), dp(8));
        dlg.show();
        dlg.getWindow().setBackgroundDrawableResource(android.R.color.transparent);
    }

    private void toast(String message) {
        Toast.makeText(this, localized(message), Toast.LENGTH_LONG).show();
    }

    private TextView button(String label, boolean primary, final Runnable action) {
        TextView b = tv(label, 17, TEXT, true);
        b.setGravity(Gravity.CENTER_VERTICAL);
        b.setPadding(dp(20), dp(15), dp(20), dp(15));
        b.setBackground(primary ? shape(RED, 0, 40) : shape(CARD, BRD, 40));
        b.setOnClickListener(v -> action.run());
        return b;
    }

    private LinearLayout header(String title) {
        return header(title, this::showMain);
    }

    private LinearLayout header(String title, Runnable backAction) {
        LinearLayout h = rowLayout();
        TextView back = tv("\u2190", 20, TEXT, false);
        back.setGravity(Gravity.CENTER);
        back.setBackground(shape(BG, BRD, 10));
        back.setOnClickListener(v -> backAction.run());
        h.addView(back, lp(dp(44), dp(44)));
        TextView t = tv(title, 22, TEXT, true);
        t.setPadding(dp(16), 0, 0, 0);
        h.addView(t, wrapWrap());
        return h;
    }

    /** Tela de configuração: seta de voltar + título no canto, coluna esquerda e direita. */
    private void showScreen(String name, String title, View left, View right) {
        showScreen(name, title, left, right, this::showMain);
    }

    private void showScreen(String name, String title, View left, View right, Runnable backAction) {
        current = name;
        status = null;
        ScrollView scroll = newScroll();
        LinearLayout col = column();
        col.setPadding(pad, pad, pad, pad);
        col.addView(header(title, backAction), withBottom(dp(16)));
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
            if (control instanceof android.widget.SeekBar) {
                line.addView(control, new LinearLayout.LayoutParams(dp(190), dp(36)));
            } else {
                line.addView(control, "bar".equals(control.getTag())
                        ? new LinearLayout.LayoutParams(dp(170), dp(8)) : wrapWrap());
            }
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

    private View toggle(boolean on, final Flip flip) {
        final boolean[] state = { on };
        final android.graphics.Paint pt = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
        final View v = new View(this) {
            @Override protected void onMeasure(int wSpec, int hSpec) {
                setMeasuredDimension(dp(52), dp(30));
            }

            @Override protected void onDraw(android.graphics.Canvas c) {
                float w = getWidth(), h = getHeight();
                pt.setStyle(android.graphics.Paint.Style.FILL);
                pt.setColor(state[0] ? RED : OFF);
                c.drawRoundRect(new android.graphics.RectF(0, 0, w, h), h / 2f, h / 2f, pt);
                pt.setColor(Color.WHITE);
                float r = h / 2f - dp(3);
                float cx = state[0] ? w - h / 2f : h / 2f;
                c.drawCircle(cx, h / 2f, r, pt);
            }
        };
        v.setClickable(true);
        v.setOnClickListener(x -> {
            state[0] = !state[0];
            v.invalidate();
            flip.on(state[0]);
        });
        return v;
    }

    private View bar(int pct) {
        LinearLayout b = new LinearLayout(this);
        b.setWeightSum(100f);
        b.setBackground(shape(OFF, 0, 4));
        View fill = new View(this);
        fill.setBackground(shape(RED, 0, 4));
        b.addView(fill, new LinearLayout.LayoutParams(0, dp(8), (float) pct));
        b.setLayoutParams(lp(dp(170), dp(8)));
        b.setTag("bar");
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
        TextView title = tv("", 27, TEXT, true);
        title.setText(name);
        left.addView(title, wrapWrap());
        TextView plus = tv("+", 25, ROSE, true);
        plus.setGravity(Gravity.CENTER);
        plus.setPadding(0, dp(1), 0, dp(1));
        left.addView(plus, wrapWrap());
        TextView secondTitle = tv("Ghost of Sparta", 30, ROSE, true);
        secondTitle.setPadding(0, dp(2), 0, dp(5));
        left.addView(secondTitle, wrapWrap());
        left.addView(tv("Launcher oficial \u00b7 " + VERSION, 14, MUTED, false), wrapWrap());
        TextView by = tv("By: flavinxs0", 15, ROSE, true);
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
        right.addView(button("Instalar arquivos do jogo", false, this::showGameFiles), withBottom(dp(8)));
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

    /** Instala o image.bin correspondente à compilação de cada jogo. */
    private void installBundledImages() {
        String[] ids = { "UCUS98653", "UCUS98737" };
        for (String id : ids) {
            String asset = "gow-games/" + id + "/image.bin";
            File dir = new File(new File(getExternalFilesDir(null), "gow-games"), id);
            File target = new File(dir, "image.bin");
            File temp = new File(dir, "image.bin.bundled.tmp");
            try (InputStream probe = getAssets().open(asset)) {
                long expectedSize = probe.available();
                if (target.isFile() && target.length() == expectedSize) continue;
                if (!dir.isDirectory() && !dir.mkdirs()) throw new java.io.IOException("Cannot create " + dir);
                try (InputStream in = getAssets().open(asset); FileOutputStream out = new FileOutputStream(temp)) {
                    byte[] buffer = new byte[64 * 1024];
                    int count;
                    while ((count = in.read(buffer)) != -1) out.write(buffer, 0, count);
                    out.getFD().sync();
                }
                if (temp.length() != expectedSize) throw new java.io.IOException("Bundled image.bin size mismatch for " + id);
                File backup = new File(dir, "image.bin.previous");
                if (target.exists() && !target.renameTo(backup)) throw new java.io.IOException("Cannot back up old image.bin for " + id);
                if (!temp.renameTo(target)) {
                    if (backup.exists()) backup.renameTo(target);
                    throw new java.io.IOException("Cannot install bundled image.bin for " + id);
                }
                if (backup.exists()) backup.delete();
                android.util.Log.i("GOW_ASSETS", "Installed matching image.bin for " + id + " (" + expectedSize + " bytes)");
            } catch (Exception error) {
                if (temp.exists()) temp.delete();
                android.util.Log.e("GOW_ASSETS", "Could not install bundled image.bin for " + id, error);
            }
        }
    }

    private void refreshStatus() {
        File image = new File(gameDir(), "image.bin");
        File iso = new File(gameDir(), "game.iso");
        boolean imageOk = image.isFile() && image.length() > 0;
        boolean isoOk = iso.isFile() && iso.length() > 0;
        status.setText(localized("Pasta: Android/data/org.gow.shell/files") + "\n"
                + (imageOk ? "\u2713 image.bin" : "\u2022 image.bin " + localized("Pendente")) + "   "
                + (isoOk ? "\u2713 game.iso" : "\u2022 game.iso " + localized("Pendente")));
        status.setTextColor(imageOk && isoOk ? GREEN : MUTED);
    }

    private void launchGame() {
        final String[] games = { "Chains of Olympus", "Ghost of Sparta" };
        dialog().setTitle(localized("Escolher jogo"))
                .setItems(games, (d, which) -> launchGameFor(which == 0 ? "coo" : "gos"))
                .setNegativeButton(localized("Cancelar"), null)
                .show();
    }

    private void launchGameFor(String gameId) {
        prefs().edit().putString("selected_game", gameId).apply();
        writeArgs();
        File iso = new File(gameDir(), "game.iso");
        File image = new File(gameDir(), "image.bin");
        if (!iso.isFile() || iso.length() == 0 || !image.isFile() || image.length() == 0) {
            dialog().setTitle(localized("Arquivos do jogo"))
                    .setMessage(localized("Jogo: ") + ("gos".equals(gameId) ? "Ghost of Sparta" : "Chains of Olympus")
                            + "\n" + localized("game.iso: ") + (iso.isFile() && iso.length() > 0 ? localized("pronto") : localized("pendente"))
                            + "\n" + localized("image.bin: ") + (image.isFile() && image.length() > 0 ? localized("pronto") : localized("pendente"))
                            + "\n\n" + localized("O jogo pode não iniciar sem os arquivos necessários."))
                    .setNegativeButton(localized("Voltar"), null)
                    .setNeutralButton(localized("Instalar arquivos"), (d, w) -> showGameFiles())
                    .setPositiveButton(localized("Tentar jogar"), (d, w) -> startSelectedGame(gameId))
                    .show();
            return;
        }
        startSelectedGame(gameId);
    }

    private void startSelectedGame(String gameId) {
        Intent intent = new Intent(this, GameActivity.class);
        intent.putExtra("gow_game_id", gameId);
        startActivity(intent);
    }

    private void showGameFiles() {
        File iso = new File(gameDir(), "game.iso");
        File image = new File(gameDir(), "image.bin");
        String isoState = iso.isFile() && iso.length() > 0
                ? String.format(Locale.US, localized("Pronto (%.1f GB)"), iso.length() / 1073741824.0) : localized("Pendente");
        String imageState = image.isFile() && image.length() > 0
                ? String.format(Locale.US, localized("Pronto (%.1f MB)"), image.length() / 1048576.0) : localized("Pendente");
        dialog().setTitle(localized("Instalar arquivos do jogo"))
                .setMessage(localized("game.iso — ") + isoState + "\n"
                        + localized("image.bin — incluído no APK e instalado automaticamente")
                        + "\n\n" + localized("Para jogar, importe somente a ISO do jogo escolhido. O image.bin correto já vem no APK."))
                .setPositiveButton(localized("Selecionar game.iso"), (d, w) -> pickGameFile(PICK_GAME_ISO, "game.iso"))
                .setNegativeButton(localized("Fechar"), null)
                .show();
    }

    private void pickGameFile(int requestCode, String expectedName) {
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        pick.putExtra(Intent.EXTRA_TITLE, localized("Selecione ") + expectedName);
        try {
            startActivityForResult(pick, requestCode);
        } catch (ActivityNotFoundException e) {
            toast(localized("Não foi possível abrir o seletor de arquivos."));
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
        final String expectedName;
        if (requestCode == PICK_GAME_ISO) expectedName = "game.iso";
        else if (requestCode == PICK_IMAGE_BIN) expectedName = "image.bin";
        else return;
        importGameFile(data.getData(), expectedName);
    }

    private String displayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri,
                new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (index >= 0) return cursor.getString(index);
            }
        } catch (Exception ignored) { }
        String last = uri.getLastPathSegment();
        return last == null ? "" : last;
    }

    private void importGameFile(Uri uri, String expectedName) {
        String pickedName = displayName(uri);
        String requiredExtension = expectedName.endsWith(".iso") ? ".iso" : ".bin";
        if (pickedName == null || !pickedName.toLowerCase(Locale.US).endsWith(requiredExtension)) {
            toast(localized("Selecione um arquivo ") + requiredExtension + localized(" para ") + expectedName + ".");
            return;
        }
        toast(localized("Copiando ") + pickedName + localized(" para o armazenamento do app..."));
        new Thread(() -> {
            File dir = gameDir();
            File temp = new File(dir, expectedName + ".importing");
            File target = new File(dir, expectedName);
            File backup = new File(dir, expectedName + ".previous");
            String error = null;
            try {
                if (!dir.isDirectory() && !dir.mkdirs()) throw new java.io.IOException("Não foi possível criar a pasta do app.");
                if (temp.exists() && !temp.delete()) throw new java.io.IOException("Não foi possível limpar a cópia temporária.");
                long total = 0;
                try (InputStream in = getContentResolver().openInputStream(uri);
                     FileOutputStream out = new FileOutputStream(temp)) {
                    if (in == null) throw new java.io.IOException("O seletor não forneceu acesso ao arquivo.");
                    byte[] buffer = new byte[1024 * 1024];
                    int count;
                    while ((count = in.read(buffer)) != -1) {
                        out.write(buffer, 0, count);
                        total += count;
                    }
                    out.getFD().sync();
                }
                if (total <= 0 || !temp.isFile() || temp.length() != total)
                    throw new java.io.IOException("O arquivo copiado está vazio ou incompleto.");
                if (backup.exists() && !backup.delete()) throw new java.io.IOException("Não foi possível preparar o backup.");
                if (target.exists() && !target.renameTo(backup))
                    throw new java.io.IOException("Não foi possível preservar o arquivo anterior.");
                if (!temp.renameTo(target)) {
                    if (backup.exists()) backup.renameTo(target);
                    throw new java.io.IOException("Não foi possível concluir a importação.");
                }
                if (backup.exists()) backup.delete();
            } catch (Exception e) {
                error = e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage();
                if (temp.exists()) temp.delete();
                if (!target.exists() && backup.exists()) backup.renameTo(target);
            }
            final String failure = error;
            runOnUiThread(() -> {
                if (failure == null) {
                    toast(localized(expectedName + " instalado (") + new File(dir, expectedName).length() / 1048576L + localized(" MB)."));
                    if ("main".equals(current) && status != null) refreshStatus();
                } else {
                    toast(localized("Falha ao importar ") + expectedName + ": " + failure);
                }
            });
        }, "gow-file-import").start();
    }

    private void checkForUpdates() {
        toast(localized("Verificando atualizações..."));
        new Thread(() -> {
            HttpURLConnection connection = null;
            try {
                URL url = new URL("https://api.github.com/repos/flavinxs0/gow-psp-recomp-android/releases/latest");
                connection = (HttpURLConnection) url.openConnection();
                connection.setConnectTimeout(8000);
                connection.setReadTimeout(8000);
                connection.setRequestProperty("Accept", "application/vnd.github+json");
                connection.setRequestProperty("User-Agent", "GodOfWar-PSP-Recomp-Android");
                StringBuilder body = new StringBuilder();
                try (BufferedReader reader = new BufferedReader(new InputStreamReader(connection.getInputStream(), StandardCharsets.UTF_8))) {
                    String line;
                    while ((line = reader.readLine()) != null) body.append(line);
                }
                JSONObject release = new JSONObject(body.toString());
                final String tag = release.optString("tag_name", "");
                final String releaseName = release.optString("name", tag);
                final String releaseUrl = release.optString("html_url", "https://github.com/flavinxs0/gow-psp-recomp-android/releases");
                if (tag.isEmpty()) throw new java.io.IOException("A API não retornou uma versão publicada.");
                final boolean sameVersion = normalizeVersion(tag).equals(normalizeVersion(VERSION));
                String foundUrl = null;
                String foundName = "GodOfWar-PSP-Recomp-Android.apk";
                org.json.JSONArray assets = release.optJSONArray("assets");
                if (assets != null) {
                    for (int i = 0; i < assets.length(); i++) {
                        JSONObject asset = assets.optJSONObject(i);
                        if (asset != null && asset.optString("name", "").toLowerCase(Locale.US).endsWith(".apk")) {
                            foundUrl = asset.optString("browser_download_url", null);
                            foundName = asset.optString("name", foundName);
                            break;
                        }
                    }
                }
                final String apkUrl = foundUrl;
                final String apkName = foundName;
                runOnUiThread(() -> {
                    AlertDialog.Builder builder = dialog().setTitle(localized("Verificador de atualizações"))
                            .setMessage(sameVersion
                                    ? localized("Versão instalada: ") + VERSION + "\n" + localized("Última release: ") + tag + "\n\n" + localized("Você está na versão mais recente publicada.")
                                    : localized("Nova release: ") + releaseName + " (" + tag + ")\n" + localized("Versão instalada: ") + VERSION + "\n\n" + localized("Baixe o APK para atualizar pelo instalador do Android."));
                    if (!sameVersion && apkUrl != null && !apkUrl.isEmpty()) {
                        builder.setPositiveButton(localized("Baixar atualização"), (d, w) -> downloadUpdate(apkUrl, apkName));
                        builder.setNeutralButton(localized("Ver release"), (d, w) -> openUrl(releaseUrl));
                    } else {
                        builder.setPositiveButton(localized("Abrir release"), (d, w) -> openUrl(releaseUrl));
                    }
                    builder.setNegativeButton(localized("Fechar"), null).show();
                });
            } catch (Exception e) {
                final String reason = e.getClass().getSimpleName()
                        + (e.getMessage() == null || e.getMessage().trim().isEmpty() ? "" : ": " + e.getMessage());
                runOnUiThread(() -> dialog().setTitle(localized("Verificador de atualizações"))
                        .setMessage(localized("Não foi possível consultar o GitHub.\n\nDetalhe: ") + reason
                                + "\n\nConfira sua conexão ou abra a página de releases.")
                        .setPositiveButton(localized("Abrir releases"), (d, w) -> openUrl("https://github.com/flavinxs0/gow-psp-recomp-android/releases"))
                        .setNegativeButton(localized("Fechar"), null).show());
            } finally {
                if (connection != null) connection.disconnect();
            }
        }, "gow-update-check").start();
    }

    private void downloadUpdate(String apkUrl, String apkName) {
        toast(localized("Baixando atualização..."));
        new Thread(() -> {
            File target = null;
            HttpURLConnection connection = null;
            try {
                File dir = new File(getCacheDir(), "updates");
                if (!dir.exists() && !dir.mkdirs()) throw new java.io.IOException("Não foi possível criar a pasta temporária.");
                String safeName = apkName == null || !apkName.toLowerCase(Locale.US).endsWith(".apk") ? "GodOfWar-PSP-Recomp-Android.apk" : apkName.replaceAll("[^A-Za-z0-9._-]", "_");
                target = new File(dir, safeName);
                connection = (HttpURLConnection) new URL(apkUrl).openConnection();
                connection.setConnectTimeout(15000);
                connection.setReadTimeout(30000);
                connection.setRequestProperty("User-Agent", "GodOfWar-PSP-Recomp-Android");
                connection.connect();
                if (connection.getResponseCode() < 200 || connection.getResponseCode() >= 300) throw new java.io.IOException("HTTP " + connection.getResponseCode());
                try (InputStream in = connection.getInputStream(); FileOutputStream out = new FileOutputStream(target)) {
                    byte[] buffer = new byte[32768]; int count;
                    while ((count = in.read(buffer)) != -1) out.write(buffer, 0, count);
                    out.getFD().sync();
                }
                if (target.length() < 1024) throw new java.io.IOException("O APK baixado está incompleto.");
                try (FileInputStream in = new FileInputStream(target)) {
                    if (in.read() != 'P' || in.read() != 'K') throw new java.io.IOException("O arquivo baixado não é um APK válido.");
                }
                File ready = target;
                runOnUiThread(() -> {
                    if (Build.VERSION.SDK_INT >= 26 && !getPackageManager().canRequestPackageInstalls()) {
                        prefs().edit().putString("pending_update_apk", ready.getAbsolutePath()).apply();
                        dialog().setTitle(localized("Permitir instalação"))
                            .setMessage(localized("O APK foi baixado. Permita que este aplicativo instale APKs nas configurações do Android; depois volte e toque em Verificar atualizações para concluir."))
                            .setPositiveButton(localized("Abrir permissões"), (d, w) -> {
                                try { startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + getPackageName()))); }
                                catch (Exception e) { toast(localized("Ative a permissão de instalação nas configurações do Android.")); }
                            }).setNegativeButton(localized("Fechar"), null).show();
                        return;
                    }
                    launchApkInstaller(ready);
                });
            } catch (Exception e) {
                if (target != null) target.delete();
                final String message = e.getMessage() == null ? "erro desconhecido" : e.getMessage();
                runOnUiThread(() -> dialog().setTitle(localized("Falha na atualização")).setMessage(localized("Não foi possível baixar o APK: ") + message).setPositiveButton(localized("OK"), null).show());
            } finally { if (connection != null) connection.disconnect(); }
        }, "gow-update-download").start();
    }

    private void launchApkInstaller(File apk) {
        try {
            Uri uri = FileProvider.getUriForFile(this, getPackageName() + ".fileprovider", apk);
            Intent install = new Intent(Intent.ACTION_VIEW);
            install.setDataAndType(uri, "application/vnd.android.package-archive");
            install.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(install);
        } catch (Exception e) { toast(localized("Não foi possível abrir o instalador do Android.")); }
    }
    private String normalizeVersion(String version) {
        String value = version == null ? "" : version.trim().toLowerCase(Locale.US);
        if (value.startsWith("android-")) value = value.substring("android-".length());
        if (value.startsWith("v")) value = value.substring(1);
        return value;
    }

    private void openUrl(String url) {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (ActivityNotFoundException e) {
            toast(localized("Nenhum navegador encontrado."));
        }
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
                seg(new String[] { "16:9", "4:3", localized("Esticada") }, gAspect, i -> gAspect = i), false));
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
        toast(localized("Salvo. Vale ao tocar em JOGAR.") + "\nargs.txt: " + (line.isEmpty() ? localized("(vazio)") : line));
        showMain();
    }

    // ------------------------------------------------------------------ 3. Desempenho / HUD

    private void showHud() {
        LinearLayout left = column();
        left.addView(tv("Painel compacto durante o jogo. FPS e tempo de quadro usam medições nativas; RAM mostra a memória PSS do processo Android.", 13, MUTED, false), matchWrap());

        LinearLayout right = column();
        right.addView(row("Mostrar HUD em jogo", null,
                toggle(prefs().getBoolean("performance_hud_enabled", false),
                        on -> prefs().edit().putBoolean("performance_hud_enabled", on).apply()), false));
        right.addView(row("FPS", null, toggle(prefs().getBoolean("performance_hud_fps", true),
                on -> prefs().edit().putBoolean("performance_hud_fps", on).apply()), false));
        right.addView(row("Tempo de quadro", null, toggle(prefs().getBoolean("performance_hud_frame_time", true),
                on -> prefs().edit().putBoolean("performance_hud_frame_time", on).apply()), false));
        right.addView(row("Memória (RAM)", null, toggle(prefs().getBoolean("performance_hud_ram", true),
                on -> prefs().edit().putBoolean("performance_hud_ram", on).apply()), false));
        right.addView(row("Estilo", null, seg(new String[] { "Em linhas", "Uma linha" },
                prefs().getInt("performance_hud_style", 0),
                i -> prefs().edit().putInt("performance_hud_style", i).apply()), false));
        right.addView(row("Posição", null, seg(new String[] { "Esq.", "Dir." },
                prefs().getInt("performance_hud_position", 0),
                i -> prefs().edit().putInt("performance_hud_position", i).remove("performance_hud_x").apply()), false));
        right.addView(row("Opacidade", "20–100%", hudOpacityControl(), false));
        showScreen("hud", "Desempenho / HUD", left, right);
    }


    private View hudOpacityControl() {
        final int min = 20;
        final int initial = Math.max(min, Math.min(100, prefs().getInt("performance_hud_opacity", 70)));
        LinearLayout control = rowLayout();
        TextView valueLabel = tv(initial + "%", 13, TEXT, true);
        android.widget.SeekBar seek = new android.widget.SeekBar(this);
        seek.setMax(100 - min);
        seek.setProgress(initial - min);
        seek.setProgressTintList(ColorStateList.valueOf(ROSE));
        seek.setThumbTintList(ColorStateList.valueOf(ROSE));
        seek.setContentDescription("Opacidade do HUD, de 20 a 100 por cento");
        seek.setOnSeekBarChangeListener(new android.widget.SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(android.widget.SeekBar bar, int progress, boolean fromUser) {
                int opacity = min + progress;
                valueLabel.setText(opacity + "%");
                if (fromUser) prefs().edit().putInt("performance_hud_opacity", opacity).apply();
            }
            @Override public void onStartTrackingTouch(android.widget.SeekBar bar) { }
            @Override public void onStopTrackingTouch(android.widget.SeekBar bar) { }
        });
        control.addView(seek, new LinearLayout.LayoutParams(dp(142), dp(36)));
        LinearLayout.LayoutParams labelParams = new LinearLayout.LayoutParams(dp(42), dp(36));
        labelParams.leftMargin = dp(6);
        valueLabel.setGravity(Gravity.CENTER);
        control.addView(valueLabel, labelParams);
        return control;
    }

    // ------------------------------------------------------------------ 4. Controles

    /** Cart\u00e3o de pr\u00e9via do layout dos bot\u00f5es na tela (PlayStation ou Xbox). */
    private final class PadPreview extends View {
        private final boolean xbox;
        private boolean picked;
        private final android.graphics.Paint pt = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
        private final android.graphics.RectF rc = new android.graphics.RectF();
        private final android.graphics.Path path = new android.graphics.Path();

        PadPreview(boolean xbox, boolean picked) {
            super(LauncherActivity.this);
            this.xbox = xbox;
            this.picked = picked;
            pt.setTextAlign(android.graphics.Paint.Align.CENTER);
        }

        void setPicked(boolean p) {
            picked = p;
            invalidate();
        }

        private void fillRound(float l, float t, float r, float b, float rad, int color) {
            pt.setStyle(android.graphics.Paint.Style.FILL);
            pt.setColor(color);
            rc.set(l, t, r, b);
            cv.drawRoundRect(rc, rad, rad, pt);
        }

        private android.graphics.Canvas cv;

        private void label(String s, float cx, float cy, float size, int color) {
            pt.setStyle(android.graphics.Paint.Style.FILL);
            pt.setColor(color);
            pt.setTextSize(size);
            cv.drawText(s, cx, cy - (pt.descent() + pt.ascent()) / 2f, pt);
        }

        private void ring(float cx, float cy, float r, int color) {
            pt.setStyle(android.graphics.Paint.Style.STROKE);
            pt.setStrokeWidth(dp(2));
            pt.setColor(color);
            cv.drawCircle(cx, cy, r, pt);
        }

        @Override protected void onDraw(android.graphics.Canvas c) {
            cv = c;
            float w = getWidth(), h = getHeight();
            fillRound(0, 0, w, h, dp(12), CARD);
            pt.setStyle(android.graphics.Paint.Style.STROKE);
            pt.setStrokeWidth(dp(picked ? 2 : 1));
            pt.setColor(picked ? RED : BRD);
            rc.set(dp(1), dp(1), w - dp(1), h - dp(1));
            c.drawRoundRect(rc, dp(12), dp(12), pt);

            int grey = Color.rgb(70, 70, 78);
            // ombros
            fillRound(w * 0.07f, h * 0.08f, w * 0.27f, h * 0.22f, dp(6), grey);
            fillRound(w * 0.73f, h * 0.08f, w * 0.93f, h * 0.22f, dp(6), grey);
            label(xbox ? "LB" : "L", w * 0.17f, h * 0.15f, dp(10), TEXT);
            label(xbox ? "RB" : "R", w * 0.83f, h * 0.15f, dp(10), TEXT);
            // direcional
            float cx = w * 0.22f, cy = h * 0.40f, arm = w * 0.075f, th = w * 0.03f;
            fillRound(cx - arm, cy - th, cx + arm, cy + th, dp(3), grey);
            fillRound(cx - th, cy - arm, cx + th, cy + arm, dp(3), grey);
            // anal\u00f3gico
            float sy = h * 0.74f, sr = w * 0.07f;
            pt.setStyle(android.graphics.Paint.Style.FILL);
            pt.setColor(grey);
            c.drawCircle(cx, sy, sr, pt);
            ring(cx, sy, sr, MUTED);
            // bot\u00f5es de a\u00e7\u00e3o
            float fx = w * 0.76f, fy = h * 0.52f, off = w * 0.085f, fr = w * 0.058f;
            float[][] pos = { { fx, fy - off }, { fx - off, fy }, { fx + off, fy }, { fx, fy + off } };
            if (xbox) {
                String[] ls = { "Y", "X", "B", "A" };
                int[] cs = { RED, Color.rgb(90, 150, 255), Color.rgb(255, 110, 110), Color.rgb(110, 210, 130) };
                for (int i = 0; i < 4; i++) {
                    ring(pos[i][0], pos[i][1], fr, cs[i]);
                    label(ls[i], pos[i][0], pos[i][1], dp(11), cs[i]);
                }
            } else {
                int[] cs = { Color.rgb(110, 210, 160), Color.rgb(240, 150, 220), Color.rgb(255, 110, 110), Color.rgb(120, 170, 255) };
                for (int i = 0; i < 4; i++) ring(pos[i][0], pos[i][1], fr, cs[i]);
                float s = fr * 0.5f;
                pt.setStyle(android.graphics.Paint.Style.STROKE);
                pt.setStrokeWidth(dp(2));
                pt.setColor(cs[0]);
                path.reset();
                path.moveTo(pos[0][0], pos[0][1] - s * 1.1f);
                path.lineTo(pos[0][0] + s, pos[0][1] + s * 0.8f);
                path.lineTo(pos[0][0] - s, pos[0][1] + s * 0.8f);
                path.close();
                c.drawPath(path, pt);
                pt.setColor(cs[1]);
                c.drawRect(pos[1][0] - s, pos[1][1] - s, pos[1][0] + s, pos[1][1] + s, pt);
                pt.setColor(cs[2]);
                c.drawCircle(pos[2][0], pos[2][1], s, pt);
                pt.setColor(cs[3]);
                c.drawLine(pos[3][0] - s, pos[3][1] - s, pos[3][0] + s, pos[3][1] + s, pt);
                c.drawLine(pos[3][0] - s, pos[3][1] + s, pos[3][0] + s, pos[3][1] - s, pt);
            }
            // start / select
            fillRound(w * 0.36f, h * 0.70f, w * 0.48f, h * 0.78f, dp(4), grey);
            fillRound(w * 0.52f, h * 0.70f, w * 0.64f, h * 0.78f, dp(4), grey);
            // nome
            label(xbox ? "Xbox" : "PlayStation", w / 2f, h * 0.92f, dp(12), picked ? TEXT : MUTED);
        }
    }

    /** Nome do primeiro controle f\u00edsico conectado, ou null. */
    private String detectPad() {
        for (int id : android.view.InputDevice.getDeviceIds()) {
            android.view.InputDevice dev = android.view.InputDevice.getDevice(id);
            if (dev == null || dev.isVirtual()) continue;
            int s = dev.getSources();
            boolean isPad = (s & android.view.InputDevice.SOURCE_GAMEPAD) == android.view.InputDevice.SOURCE_GAMEPAD
                    || (s & android.view.InputDevice.SOURCE_JOYSTICK) == android.view.InputDevice.SOURCE_JOYSTICK;
            if (!isPad) {
                try {
                    boolean[] k = dev.hasKeys(android.view.KeyEvent.KEYCODE_BUTTON_A);
                    isPad = k.length > 0 && k[0];
                } catch (RuntimeException ignored) {
                }
            }
            if (isPad) return dev.getName();
        }
        return null;
    }

    private void showControls() {
        final SharedPreferences p = prefs();
        final int[] ops = { 30, 50, 70, 100 };
        int opIndex = 1;
        for (int i = 0; i < ops.length; i++) {
            if (ops[i] == p.getInt("ctl_opacity", 50)) opIndex = i;
        }
        final boolean xbox0 = "xbox".equals(p.getString("ctl_style", "ps"));
        final PadPreview[] prev = { new PadPreview(false, !xbox0), new PadPreview(true, xbox0) };

        LinearLayout left = column();
        LinearLayout previews = rowLayout();
        LinearLayout.LayoutParams pa = new LinearLayout.LayoutParams(0, dp(150), 1f);
        LinearLayout.LayoutParams pb = new LinearLayout.LayoutParams(0, dp(150), 1f);
        pb.leftMargin = dp(10);
        previews.addView(prev[0], pa);
        previews.addView(prev[1], pb);
        left.addView(previews, withBottom(dp(10)));
        final TextView note = tv(autoNote(p.getBoolean("ctl_auto", true)), 12, MUTED, false);
        left.addView(note, matchWrap());

        final LinearLayout styleSeg = seg(new String[] { "PlayStation", "Xbox" }, xbox0 ? 1 : 0, i -> {
            p.edit().putString("ctl_style", i == 1 ? "xbox" : "ps").apply();
            prev[0].setPicked(i == 0);
            prev[1].setPicked(i == 1);
        });
        prev[0].setOnClickListener(v -> styleSeg.getChildAt(0).performClick());
        prev[1].setOnClickListener(v -> styleSeg.getChildAt(1).performClick());

        String padName = detectPad();
        TextView padState = tv(padName == null ? "N\u00e3o conectado" : "\u25cf Conectado", 14,
                padName == null ? MUTED : GREEN, false);

        LinearLayout right = column();
        right.addView(row("Controles na tela", "Dire\u00e7\u00e3o, anal\u00f3gico e bot\u00f5es",
                toggle(p.getBoolean("ctl_screen", true),
                        on -> p.edit().putBoolean("ctl_screen", on).apply()), false));
        right.addView(row("Estilo dos bot\u00f5es", "Vale na pr\u00f3xima vez que tocar em JOGAR", styleSeg, false));
        right.addView(row("Autom\u00e1tico", "Oculta os controles na tela ao conectar um controle",
                toggle(p.getBoolean("ctl_auto", true), on -> {
                    p.edit().putBoolean("ctl_auto", on).apply();
                    note.setText(autoNote(on));
                }), false));
        right.addView(row("Controle detectado", padName == null ? "Nenhum controle f\u00edsico" : padName,
                padState, false));
        right.addView(row("Opacidade dos controles", "20–100%",
                controlOpacityControl(p), false));
        right.addView(row("Vibração do controle", "Resposta tátil ao tocar nos controles virtuais", toggle(p.getBoolean("ctl_vibration", true), on -> p.edit().putBoolean("ctl_vibration", on).apply()), false));
        left.addView(row("Zona morta do anal\u00f3gico", "Menor valor responde com menos movimento",
                deadZoneControl(p), false));
        right.addView(row("Editor de controles individuais", "Personalize a posição dos botões durante a partida",
                button("Como usar", false, () -> showControlEditorHelp()), false));
        right.addView(row("Restaurar posição", "Voltar ao layout original",
                actions(new String[] { localized("Restaurar") }, new Runnable[] { () -> {
                    SharedPreferences.Editor editor = p.edit().remove("ctl_offset_x").remove("ctl_offset_y").putBoolean("ctl_edit_layout", false);
                    for (String profile : new String[] { "ps", "xbox" }) {
                        for (int control = 0; control < 16; control++) {
                            editor.remove("ctl_pos_" + profile + "_" + control + "_x");
                            editor.remove("ctl_pos_" + profile + "_" + control + "_y");
                        }
                    }
                    editor.apply();
                    toast(localized("Posições originais restauradas."));
                    showControls();
                } }, false), false));
        showScreen("controls", "Controles", left, right);
    }

    private View deadZoneControl(SharedPreferences p) {
        final int initial = Math.max(0, Math.min(50, p.getInt("ctl_deadzone", 5)));
        LinearLayout control = rowLayout();
        TextView valueLabel = tv(initial + "%", 13, TEXT, true);
        android.widget.SeekBar seek = new android.widget.SeekBar(this);
        seek.setMax(50);
        seek.setProgress(initial);
        seek.setProgressTintList(ColorStateList.valueOf(ROSE));
        seek.setThumbTintList(ColorStateList.valueOf(ROSE));
        seek.setContentDescription("Zona morta do analógico, de zero a cinquenta por cento");
        seek.setOnSeekBarChangeListener(new android.widget.SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(android.widget.SeekBar bar, int value, boolean fromUser) {
                valueLabel.setText(value + "%");
                if (fromUser) p.edit().putInt("ctl_deadzone", value).apply();
            }
            @Override public void onStartTrackingTouch(android.widget.SeekBar bar) { }
            @Override public void onStopTrackingTouch(android.widget.SeekBar bar) { }
        });
        control.addView(seek, new LinearLayout.LayoutParams(dp(142), dp(36)));
        LinearLayout.LayoutParams labelParams = new LinearLayout.LayoutParams(dp(42), dp(36));
        labelParams.leftMargin = dp(6);
        valueLabel.setGravity(Gravity.CENTER);
        control.addView(valueLabel, labelParams);
        return control;
    }

    private View controlOpacityControl(SharedPreferences p) {
        final int min = 20;
        final int initial = Math.max(min, Math.min(100, p.getInt("ctl_opacity", 50)));
        LinearLayout control = rowLayout();
        TextView valueLabel = tv(initial + "%", 13, TEXT, true);
        android.widget.SeekBar seek = new android.widget.SeekBar(this);
        seek.setMax(100 - min);
        seek.setProgress(initial - min);
        seek.setProgressTintList(ColorStateList.valueOf(ROSE));
        seek.setThumbTintList(ColorStateList.valueOf(ROSE));
        seek.setContentDescription("Opacidade dos controles, de 20 a 100 por cento");
        seek.setOnSeekBarChangeListener(new android.widget.SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(android.widget.SeekBar bar, int progress, boolean fromUser) {
                int opacity = min + progress;
                valueLabel.setText(opacity + "%");
                if (fromUser) p.edit().putInt("ctl_opacity", opacity).apply();
            }
            @Override public void onStartTrackingTouch(android.widget.SeekBar bar) { }
            @Override public void onStopTrackingTouch(android.widget.SeekBar bar) { }
        });
        control.addView(seek, new LinearLayout.LayoutParams(dp(142), dp(36)));
        LinearLayout.LayoutParams labelParams = new LinearLayout.LayoutParams(dp(42), dp(36));
        labelParams.leftMargin = dp(6);
        valueLabel.setGravity(Gravity.CENTER);
        control.addView(valueLabel, labelParams);
        return control;
    }

    private void showButtonMapping(SharedPreferences p) {
        final String[] labels = { "Cruz", "Círculo", "Quadrado", "Triângulo" };
        final String profile = "xbox".equals(p.getString("ctl_style", "ps")) ? "xbox" : "ps";
        final String[] keys = { "ctl_map_" + profile + "_cross", "ctl_map_" + profile + "_circle", "ctl_map_" + profile + "_square", "ctl_map_" + profile + "_triangle" };
        final int[] defaults = { 0, 1, 2, 3 };
        final String[] targets = { "Cruz", "Círculo", "Quadrado", "Triângulo" };

        LinearLayout left = column();
        LinearLayout info = card();
        info.addView(tv("MAPEAMENTO PERSONALIZADO", 12, ROSE, true), withBottom(dp(8)));
        info.addView(tv(localized("Perfil ") + ("xbox".equals(profile) ? "Xbox" : "PlayStation") + localized(". Escolha qual ação cada botão representa. Altere o perfil na tela Controles para configurar o outro layout."),
                14, TEXT, false), matchWrap());
        left.addView(info, withBottom(dp(12)));
        left.addView(button("Restaurar padrão", false, () -> {
            SharedPreferences.Editor e = p.edit();
            for (String key : keys) e.remove(key);
            e.apply();
            toast(localized("Mapeamento padrão restaurado. Vale ao abrir o jogo novamente."));
            showButtonMapping(p);
        }), withBottom(dp(8)));

        LinearLayout right = column();
        for (int i = 0; i < labels.length; i++) {
            final int index = i;
            int selected = Math.max(0, Math.min(3, p.getInt(keys[i], defaults[i])));
            LinearLayout item = card();
            item.addView(tv(labels[i], 16, TEXT, true), withBottom(dp(8)));
            item.addView(tv("Ação atribuída", 11, MUTED, false), withBottom(dp(5)));
            item.addView(seg(targets, selected, which -> {
                p.edit().putInt(keys[index], which).apply();
                showButtonMapping(p);
            }), matchWrap());
            right.addView(item, withBottom(dp(8)));
        }
        showScreen("mapping", "Mapeamento dos botões", left, right, this::showControls);
    }

    private String autoNote(boolean on) {
        return on ? "Autom\u00e1tico ligado: os controles na tela somem ao conectar o controle."
                : "Autom\u00e1tico desligado: os controles na tela ficam sempre vis\u00edveis.";
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
                actions(new String[] { localized("Backup"), localized("Restaurar") },
                        new Runnable[] { this::backupSaves, this::restoreSaves }, false), false));
        right.addView(row("Logs", "Ver, compartilhar ou limpar o log.txt",
                actions(new String[] { localized("Ver"), localized("Compartilhar"), localized("Limpar") },
                        new Runnable[] { this::showLog, this::shareLog, this::clearLog }, false), false));
        right.addView(row("Argumentos avan\u00e7ados", "Ver e editar o args.txt",
                actions(new String[] { localized("Editar") }, new Runnable[] { this::editArgs }, false), false));
        right.addView(row("Arquivos do jogo", "Importar somente a ISO; image.bin vem no APK",
                actions(new String[] { "Gerenciar" }, new Runnable[] { this::showGameFiles }, false), false));
        right.addView(row("Restaurar padr\u00f5es", "Volta todas as configura\u00e7\u00f5es ao original",
                actions(new String[] { localized("Restaurar") }, new Runnable[] { this::confirmReset }, true), false));
        right.addView(row("Idioma", "Idioma da interface do launcher", seg(new String[] { "Português", "English" }, "en".equals(prefs().getString("launcher_language", "pt")) ? 1 : 0, i -> { prefs().edit().putString("launcher_language", i == 1 ? "en" : "pt").apply(); toast(i == 1 ? "English selected." : localized("Idioma Português selecionado.")); showExtras(); }), false));
        right.addView(row("Verificar atualizações", "Consultar a versão mais recente do APK no GitHub",
                actions(new String[] { "Verificar" }, new Runnable[] { this::checkForUpdates }, false), false));
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
            toast(localized("Nenhum save encontrado ainda."));
            return;
        }
        final String stamp = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date());
        final File dst = new File(backupsDir(), stamp);
        new Thread(() -> {
            final boolean ok = copyTree(src, dst);
            runOnUiThread(() -> toast(ok ? localized("Backup criado: backup_saves/") + stamp
                    : localized("Falha ao criar o backup.")));
        }).start();
    }

    private void restoreSaves() {
        File[] backups = backupsDir().listFiles();
        if (backups == null || backups.length == 0) {
            toast(localized("Nenhum backup encontrado."));
            return;
        }
        Arrays.sort(backups);
        final File latest = backups[backups.length - 1];
        dialog().setTitle(localized("RESTAURAR SAVES"))
                .setMessage(localized("Restaurar o backup ") + latest.getName()
                        + "?\n\nOs saves atuais ser\u00e3o sobrescritos.")
                .setNegativeButton(localized("CANCELAR"), null)
                .setPositiveButton(localized("RESTAURAR"), (d, w) -> new Thread(() -> {
                    final boolean ok = copyTree(latest, savesDir());
                    runOnUiThread(() -> toast(ok ? localized("Saves restaurados.") : localized("Falha ao restaurar.")));
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
            return localized("Não foi possível ler o log: ") + e.getMessage();
        }
        if (out.length() > maxChars) return out.substring(out.length() - maxChars);
        return out.toString();
    }

    private void showLog() {
        String text = readLog(60000);
        if (text.isEmpty()) text = localized("Nenhum log encontrado ainda. Inicie o jogo para gerar o registro.");
        TextView body = new TextView(this);
        body.setText(text);
        body.setTextColor(TEXT);
        body.setTextSize(11);
        body.setTypeface(Typeface.MONOSPACE);
        body.setPadding(dp(16), dp(8), dp(16), dp(8));
        ScrollView scroller = new ScrollView(this);
        scroller.addView(body);
        dialog().setTitle(localized("LOG")).setView(scroller).setPositiveButton(localized("FECHAR"), null).show();
    }

    private void shareLog() {
        String text = readLog(200000);
        if (text.isEmpty()) {
            toast(localized("Nenhum log encontrado ainda."));
            return;
        }
        Intent send = new Intent(Intent.ACTION_SEND);
        send.setType("text/plain");
        send.putExtra(Intent.EXTRA_SUBJECT, "log.txt");
        send.putExtra(Intent.EXTRA_TEXT, text);
        startActivity(Intent.createChooser(send, localized("Compartilhar log")));
    }

    private void clearLog() {
        if (!logFile().isFile()) {
            toast(localized("Nenhum log para limpar."));
            return;
        }
        dialog().setTitle(localized("LIMPAR LOG"))
                .setMessage(localized("Apagar o log.txt da \u00faltima sess\u00e3o?"))
                .setNegativeButton(localized("CANCELAR"), null)
                .setPositiveButton(localized("LIMPAR"), (d, w) -> {
                    toast(logFile().delete() ? localized("Log apagado.") : localized("N\u00e3o foi poss\u00edvel apagar o log."));
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
        dialog().setTitle(localized("ARGUMENTOS AVAN\u00c7ADOS"))
                .setMessage(localized("Edite com cuidado. A propor\u00e7\u00e3o de tela (--aspect) \u00e9 controlada pela tela Gr\u00e1ficos."))
                .setView(input)
                .setNegativeButton(localized("CANCELAR"), null)
                .setPositiveButton(localized("SALVAR"), (d, w) -> {
                    saveArgsRaw(input.getText().toString());
                    toast(localized("args.txt salvo."));
                    showExtras();
                })
                .show();
    }

    private void confirmReset() {
        LinearLayout left = column();
        LinearLayout warning = card();
        warning.addView(tv("RESTAURAR PADRÕES", 13, ROSE, true), withBottom(dp(8)));
        warning.addView(tv("Isso redefine proporção, escala, FXAA, limite de FPS e HUD. Outros argumentos do args.txt serão mantidos.",
                14, TEXT, false), matchWrap());
        left.addView(warning, withBottom(dp(12)));
        left.addView(button("Voltar sem alterar", false, this::showExtras), withBottom(dp(8)));

        LinearLayout right = column();
        LinearLayout confirm = card();
        confirm.addView(tv("Confirmar restauração", 17, TEXT, true), withBottom(dp(8)));
        confirm.addView(tv("As configurações gráficas voltarão aos valores originais do launcher.",
                13, MUTED, false), withBottom(dp(14)));
        confirm.addView(button("RESTAURAR AGORA", true, () -> {
            resetDefaults();
            toast(localized("Padrões restaurados."));
            showExtras();
        }), matchWrap());
        right.addView(confirm, matchWrap());
        showScreen("reset-confirm", "Restaurar padrões", left, right, this::showExtras);
    }
    // ------------------------------------------------------------------ 6. Sobre

    private void showAbout() {
        LinearLayout left = column();
        LinearLayout brand = card();
        brand.addView(tv("G O D   O F   W A R", 12, MUTED, false), wrapWrap());
        SpannableStringBuilder name = new SpannableStringBuilder("Ghost of Sparta");
        name.setSpan(new ForegroundColorSpan(ROSE), 9, 15, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        TextView title = tv("", 26, TEXT, true);
        title.setText(name);
        brand.addView(title, wrapWrap());
        brand.addView(tv("Launcher n\u00e3o oficial \u00b7 " + VERSION, 14, MUTED, false), wrapWrap());
        TextView by = tv("By: flavinxs0", 14, ROSE, true);
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
                actions(new String[] { localized("Ver") }, new Runnable[] { this::showLicenses }, false), false));
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
            toast(localized("Nenhum navegador encontrado."));
        }
    }

    private void showLicenses() {
        dialog().setTitle(localized("LICEN\\u00c7AS"))
                .setMessage(localized("Este app usa a biblioteca SDL (licen\\u00e7a zlib).\n\nA lista completa de componentes est\\u00e1 no reposit\\u00f3rio do projeto."))
                .setPositiveButton(localized("OK"), null)
                .show();
    }

    // ------------------------------------------------------------------ args.txt

    private SharedPreferences prefs() {
        return getSharedPreferences(PREFS, MODE_PRIVATE);
    }

    private File gameDir() {
        String id = prefs().getString("selected_game", "coo");
        File base = getExternalFilesDir(null);
        if (base == null) base = getFilesDir();
        File dir = new File(new File(base, "gow-games"), "gos".equals(id) ? "UCUS98737" : "UCUS98653");
        if (!dir.isDirectory()) dir.mkdirs();
        return dir;
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

    /** Remove as opções de proporção gerenciadas; os demais argumentos só quando pedido. */
    private List<String> filterTokens(List<String> tokens, boolean res, boolean fxaa, boolean fps, boolean vsync) {
        List<String> out = new ArrayList<>();
        for (int i = 0; i < tokens.size(); i++) {
            String t = tokens.get(i);
            if (t.equals("--aspect")) { i++; continue; }
            if (t.equals("--stretch")) continue;
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
            out.add("--stretch");
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
                .remove("performance_hud_enabled").remove("performance_hud_fps").remove("performance_hud_frame_time").remove("performance_hud_ram").remove("performance_hud_style").remove("performance_hud_position").remove("performance_hud_opacity").remove("ctl_screen").remove("ctl_auto").remove("ctl_opacity").remove("ctl_style").remove("ctl_deadzone").remove("ctl_map_cross").remove("ctl_map_circle").remove("ctl_map_square").remove("ctl_map_triangle").putString("screen_mode", "16:9").apply();
        writeLine(join(filterTokens(readArgTokens(), true, true, true, true)));
    }
}
