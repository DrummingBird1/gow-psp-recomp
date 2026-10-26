"""The launcher's window in other languages: T("English text") gives the text in the chosen language.

The window's labels, buttons, options and the games' state are translated; longer help texts (tooltips, dialogs) and
the log stay in English. The language is chosen in Options > Advanced and applies from the next start. A text
missing from a table is shown in English.
"""
LANGS = [("English", "en"), ("Español", "es"), ("Français", "fr"), ("עברית", "he")]
RTL = {"he"}
_lang = "en"

STRINGS = {
    "es": {
        "Step 1 of 3": "Paso 1 de 3", "Step 2 of 3": "Paso 2 de 3",
        "Choose the folder with your games": "Elige la carpeta con tus juegos",
        "Choose game folder…": "Elegir carpeta de juegos…", "Show log": "Ver registro",
        "Game folder": "Carpeta de juegos", "Change…": "Cambiar…", "Add to Steam": "Añadir a Steam",
        "Create desktop shortcut": "Crear acceso directo", "Build": "Compilar", "Rebuild": "Recompilar",
        "Play": "Jugar", "Check for updates": "Buscar actualizaciones", "Report a bug": "Informar de un error",
        "Open logs folder": "Abrir carpeta de registros", "License (GPL-3.0)": "Licencia (GPL-3.0)",
        "Third-party notices": "Avisos de terceros", "Options": "Opciones", "Options (F1)": "Opciones (F1)",
        "Preset": "Preajuste", "Custom": "Personalizado", "Display": "Pantalla", "Resolution": "Resolución",
        "Anti-aliasing": "Antialiasing", "Frame rate": "Fotogramas por segundo", "Aspect ratio": "Relación de aspecto",
        "Window size": "Tamaño de ventana", "Fullscreen": "Pantalla completa",
        "Show the frame rate (F3)": "Mostrar los FPS (F3)", "CRT filter (scanlines)": "Filtro CRT (líneas)",
        "HUD at the screen edges at 21:9 / 32:9 (experimental)": "HUD en los bordes en 21:9 / 32:9 (experimental)",
        "Separate graphics settings for each game": "Gráficos distintos para cada juego",
        "Game": "Juego", "Language": "Idioma", "Music volume": "Volumen de la música",
        "Effects and voices": "Efectos y voces", "Skip the movies (cutscene videos)": "Saltar los vídeos",
        "Infinite health (cheat)": "Salud infinita (truco)",
        "Gamepad rumble when Kratos is hit": "Vibración del mando cuando golpean a Kratos",
        "Circle confirms, Cross cancels (Japanese layout)": "Círculo confirma, Equis cancela (estilo japonés)",
        "Show what I play on Discord": "Mostrar en Discord a qué juego",
        "Advanced  ▸": "Avanzado  ▸", "Advanced  ▾": "Avanzado  ▾", "Renderer": "Renderizador",
        "Effects resolution": "Resolución de efectos", "Anisotropic filtering": "Filtrado anisotrópico",
        "Graphics API": "API gráfica", "Graphics card": "Tarjeta gráfica", "Sharpening": "Nitidez",
        "V-Sync": "V-Sync", "Mute": "Silenciar", "Texture pack folder": "Carpeta del pack de texturas",
        "Dump textures (to make a pack)": "Extraer texturas (para hacer un pack)", "Controls…": "Controles…",
        "Upscale dumped textures (AI)…": "Mejorar las texturas extraídas (IA)…",
        "Import PPSSPP saves…": "Importar partidas de PPSSPP…",
        "Check for launcher updates at start": "Buscar actualizaciones al iniciar",
        "Reduce motion (launcher animations)": "Reducir el movimiento (animaciones)",
        "Launcher language": "Idioma del launcher",
        "Ready to play": "Listo para jugar", "Needs building": "Hay que compilarlo",
        "No disc image found": "No se encontró la imagen del disco",
        "Original": "Original", "Light": "Ligero", "Balanced": "Equilibrado", "High": "Alto", "Ultra": "Ultra",
        "60 (original)": "60 (original)", "Unlimited (VRR)": "Sin límite (VRR)", "4:3 (crops the sides)": "4:3 (recorta los lados)",
        "Auto": "Auto", "Off": "No", "Hardware": "Hardware", "Software (accurate, slow)": "Software (exacto, lento)",
        "Match resolution": "Igual que la resolución", "Original glow": "Brillo original",
        "Auto (Direct3D 12)": "Auto (Direct3D 12)", "Fast (dedicated)": "Rápida (dedicada)",
        "Power saving (integrated)": "Ahorro (integrada)", "English": "Inglés", "French": "Francés", "Spanish": "Español",
        "Controls": "Controles", "PSP button": "Botón de PSP", "Key": "Tecla", "Key 2": "Tecla 2", "Gamepad": "Mando",
        "Gamepad 2": "Mando 2", "Defaults": "Por defecto", "Save": "Guardar", "Cancel": "Cancelar",
        "Gamepad right stick evades (L + R + stick, as in the PS2 games)":
            "El stick derecho esquiva (L + R + stick, como en PS2)",
    },
    "fr": {
        "Step 1 of 3": "Étape 1 sur 3", "Step 2 of 3": "Étape 2 sur 3",
        "Choose the folder with your games": "Choisissez le dossier de vos jeux",
        "Choose game folder…": "Choisir le dossier des jeux…", "Show log": "Voir le journal",
        "Game folder": "Dossier des jeux", "Change…": "Changer…", "Add to Steam": "Ajouter à Steam",
        "Create desktop shortcut": "Créer un raccourci", "Build": "Compiler", "Rebuild": "Recompiler",
        "Play": "Jouer", "Check for updates": "Rechercher des mises à jour", "Report a bug": "Signaler un bug",
        "Open logs folder": "Ouvrir le dossier des journaux", "License (GPL-3.0)": "Licence (GPL-3.0)",
        "Third-party notices": "Mentions des tiers", "Options": "Options", "Options (F1)": "Options (F1)",
        "Preset": "Préréglage", "Custom": "Personnalisé", "Display": "Affichage", "Resolution": "Résolution",
        "Anti-aliasing": "Anticrénelage", "Frame rate": "Images par seconde", "Aspect ratio": "Format d'image",
        "Window size": "Taille de la fenêtre", "Fullscreen": "Plein écran",
        "Show the frame rate (F3)": "Afficher les IPS (F3)", "CRT filter (scanlines)": "Filtre CRT (lignes)",
        "HUD at the screen edges at 21:9 / 32:9 (experimental)": "HUD sur les bords en 21:9 / 32:9 (expérimental)",
        "Separate graphics settings for each game": "Réglages graphiques propres à chaque jeu",
        "Game": "Jeu", "Language": "Langue", "Music volume": "Volume de la musique",
        "Effects and voices": "Effets et voix", "Skip the movies (cutscene videos)": "Passer les vidéos",
        "Infinite health (cheat)": "Santé infinie (triche)",
        "Gamepad rumble when Kratos is hit": "Vibration de la manette quand Kratos est touché",
        "Circle confirms, Cross cancels (Japanese layout)": "Rond valide, Croix annule (disposition japonaise)",
        "Show what I play on Discord": "Montrer mon jeu sur Discord",
        "Advanced  ▸": "Avancé  ▸", "Advanced  ▾": "Avancé  ▾", "Renderer": "Rendu",
        "Effects resolution": "Résolution des effets", "Anisotropic filtering": "Filtrage anisotrope",
        "Graphics API": "API graphique", "Graphics card": "Carte graphique", "Sharpening": "Netteté",
        "V-Sync": "V-Sync", "Mute": "Muet", "Texture pack folder": "Dossier du pack de textures",
        "Dump textures (to make a pack)": "Extraire les textures (pour un pack)", "Controls…": "Commandes…",
        "Upscale dumped textures (AI)…": "Agrandir les textures extraites (IA)…",
        "Import PPSSPP saves…": "Importer les sauvegardes PPSSPP…",
        "Check for launcher updates at start": "Rechercher les mises à jour au démarrage",
        "Reduce motion (launcher animations)": "Réduire les animations",
        "Launcher language": "Langue du launcher",
        "Ready to play": "Prêt à jouer", "Needs building": "À compiler",
        "No disc image found": "Image du disque introuvable",
        "Original": "Original", "Light": "Léger", "Balanced": "Équilibré", "High": "Élevé", "Ultra": "Ultra",
        "60 (original)": "60 (original)", "Unlimited (VRR)": "Illimité (VRR)", "4:3 (crops the sides)": "4:3 (rogne les côtés)",
        "Auto": "Auto", "Off": "Non", "Hardware": "Matériel", "Software (accurate, slow)": "Logiciel (exact, lent)",
        "Match resolution": "Comme la résolution", "Original glow": "Halo d'origine",
        "Auto (Direct3D 12)": "Auto (Direct3D 12)", "Fast (dedicated)": "Rapide (dédiée)",
        "Power saving (integrated)": "Économie (intégrée)", "English": "Anglais", "French": "Français", "Spanish": "Espagnol",
        "Controls": "Commandes", "PSP button": "Bouton PSP", "Key": "Touche", "Key 2": "Touche 2", "Gamepad": "Manette",
        "Gamepad 2": "Manette 2", "Defaults": "Par défaut", "Save": "Enregistrer", "Cancel": "Annuler",
        "Gamepad right stick evades (L + R + stick, as in the PS2 games)":
            "Le stick droit esquive (L + R + stick, comme sur PS2)",
    },
    "he": {
        "Step 1 of 3": "שלב 1 מתוך 3", "Step 2 of 3": "שלב 2 מתוך 3",
        "Choose the folder with your games": "בחר את התיקייה עם המשחקים שלך",
        "Choose game folder…": "בחירת תיקיית משחקים…", "Show log": "הצגת היומן",
        "Game folder": "תיקיית המשחקים", "Change…": "שינוי…", "Add to Steam": "הוספה ל-Steam",
        "Create desktop shortcut": "קיצור דרך בשולחן העבודה", "Build": "בנייה", "Rebuild": "בנייה מחדש",
        "Play": "שחק", "Check for updates": "בדיקת עדכונים", "Report a bug": "דיווח על באג",
        "Open logs folder": "פתיחת תיקיית היומנים", "License (GPL-3.0)": "רישיון (GPL-3.0)",
        "Third-party notices": "הודעות צד שלישי", "Options": "אפשרויות", "Options (F1)": "אפשרויות (F1)",
        "Preset": "פריסט", "Custom": "מותאם אישית", "Display": "תצוגה", "Resolution": "רזולוציה",
        "Anti-aliasing": "החלקת קצוות", "Frame rate": "קצב פריימים", "Aspect ratio": "יחס מסך",
        "Window size": "גודל חלון", "Fullscreen": "מסך מלא",
        "Show the frame rate (F3)": "הצגת קצב הפריימים (F3)", "CRT filter (scanlines)": "פילטר CRT (קווי סריקה)",
        "HUD at the screen edges at 21:9 / 32:9 (experimental)": "HUD בקצוות המסך ב-21:9 / 32:9 (ניסיוני)",
        "Separate graphics settings for each game": "הגדרות גרפיקה נפרדות לכל משחק",
        "Game": "משחק", "Language": "שפה", "Music volume": "עוצמת המוזיקה",
        "Effects and voices": "אפקטים וקולות", "Skip the movies (cutscene videos)": "דילוג על הסרטונים",
        "Infinite health (cheat)": "בריאות אינסופית (צ'יט)",
        "Gamepad rumble when Kratos is hit": "רטט בשלט כש-Kratos נפגע",
        "Circle confirms, Cross cancels (Japanese layout)": "עיגול מאשר, איקס מבטל (פריסה יפנית)",
        "Show what I play on Discord": "להציג ב-Discord במה אני משחק",
        "Advanced  ▸": "מתקדם  ◂", "Advanced  ▾": "מתקדם  ▾", "Renderer": "רנדרר",
        "Effects resolution": "רזולוציית האפקטים", "Anisotropic filtering": "סינון אנאיזוטרופי",
        "Graphics API": "ממשק גרפי", "Graphics card": "כרטיס גרפי", "Sharpening": "חידוד",
        "V-Sync": "V-Sync", "Mute": "השתקה", "Texture pack folder": "תיקיית חבילת טקסטורות",
        "Dump textures (to make a pack)": "חילוץ טקסטורות (ליצירת חבילה)", "Controls…": "שליטה…",
        "Upscale dumped textures (AI)…": "שיפור הטקסטורות שחולצו (AI)…",
        "Import PPSSPP saves…": "ייבוא שמירות מ-PPSSPP…",
        "Check for launcher updates at start": "בדיקת עדכונים בהפעלה",
        "Reduce motion (launcher animations)": "פחות תנועה (אנימציות)",
        "Launcher language": "שפת הלאנצ'ר",
        "Ready to play": "מוכן למשחק", "Needs building": "צריך לבנות",
        "No disc image found": "לא נמצאה תמונת דיסק",
        "Original": "מקורי", "Light": "קל", "Balanced": "מאוזן", "High": "גבוה", "Ultra": "אולטרה",
        "60 (original)": "60 (מקורי)", "Unlimited (VRR)": "ללא הגבלה (VRR)", "4:3 (crops the sides)": "4:3 (חותך בצדדים)",
        "Auto": "אוטומטי", "Off": "כבוי", "Hardware": "חומרה", "Software (accurate, slow)": "תוכנה (מדויק, איטי)",
        "Match resolution": "כמו הרזולוציה", "Original glow": "זוהר מקורי",
        "Auto (Direct3D 12)": "אוטומטי (Direct3D 12)", "Fast (dedicated)": "מהיר (ייעודי)",
        "Power saving (integrated)": "חיסכון (משולב)", "English": "אנגלית", "French": "צרפתית", "Spanish": "ספרדית",
        "Controls": "שליטה", "PSP button": "כפתור PSP", "Key": "מקש", "Key 2": "מקש 2", "Gamepad": "שלט",
        "Gamepad 2": "שלט 2", "Defaults": "ברירת מחדל", "Save": "שמירה", "Cancel": "ביטול",
        "Gamepad right stick evades (L + R + stick, as in the PS2 games)":
            "הסטיק הימני מתחמק (L + R + סטיק, כמו ב-PS2)",
    },
}


def set_language(code):
    global _lang
    _lang = code if code in STRINGS else "en"


def language():
    return _lang


def T(text):
    return STRINGS.get(_lang, {}).get(text, text)
