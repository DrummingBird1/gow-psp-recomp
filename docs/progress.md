# יומן התקדמות

תיעוד מצולם של העבודה על הפרויקט, לפי תאריך. כל רשומה מציינת את ה-commit שעליו נעשו הצילומים והסרטונים.

> התמונות והסרטונים נשמרים ב-`media/` (מקומי בלבד, מוחרג ב-`.gitignore`): הם מציגים תמונות מהמשחק.
> צילומי מסך של פלט מסוף מופקים מהפלט האמיתי של הפקודה בעזרת `tools/render_terminal.py`.
> בכותרת של כל תמונה מופיעים הפקודה, התאריך וה-commit.
> מסגרות מהמשחק עצמו נשמרות ישירות מה-framebuffer של ה-PSP על ידי הריצה (`--frames-dir`).
> ההקלטות מופקות דרך ffmpeg (`--record`).

## סיכום שלבים

| תאריך | שלב | מצב |
|---|---|---|
| 2026-10-15 | ניתוח Ghidra מלא של שני ה-EBOOT, דוח היתכנות | הושלם |
| 2026-10-15 | שלב 1: אב-טיפוס של המהדר מחדש (recompiler) עם runtime מינימלי | הושלם: 100% מהפונקציות ניתנות לתרגום בשני המשחקים |
| 2026-10-15 | שלב 2: מתזמן תהליכונים, מערכת קבצים (CHD/ISO), שכבת HLE | הושלם (commit d12a000) |
| 2026-10-16 | שלב 3: VFPU | הושלם (commit 9f3b497) |
| 2026-10-16 | שלב 4: מפרש GE ו-renderer תוכנה | התפריט הראשי של CoO על המסך, ‏24–28 FPS ([פירוט](phase4-ge.md)) |
| 2026-10-16 | שלב 5: חלון, קלט, sceMpeg, coroutines | נכנסים למשחק: השלב הראשון משוחק בחלון, ~19 FPS ([פירוט](phase5-window-input.md)) |
| 2026-10-16 | צמצום flushes (v4) | פי 37 פחות flushes, ‏29–35 FPS במשחק ([פירוט](renderer-decision.md)) |
| 2026-10-16 | שלב 6, ‏P1: renderer חומרה על SDL3 GPU (v5) | התפריטים והשלב הראשון מה-GPU, ‏77.5 FPS במשחק (headless); render-to-texture נשאר ל-P2 ([פירוט](phase6-gpu-renderer.md)) |
| 2026-10-16 | שלב 6, ‏P2: ניהול framebuffers, ‏render-to-texture, ‏stencil (v6) | כל האפקטים מה-GPU, בלי רעש; 60 FPS יציב בחלון, ‏116 FPS במשחק (headless) ([פירוט](phase6-gpu-renderer.md#p2-ניהול-framebuffers)) |
| 2026-10-16 | שלב 7: סאונד (sceAudio, ‏SAS, ‏ATRAC3+) | מוזיקת התפריט והשלב ואפקטי הקרב; 60 FPS בחלון בלי underruns; ה-decoder זהה ל-FFmpeg ([פירוט](phase7-audio.md)) |
| 2026-10-16 | שלב 8: סרטונים (sceMpeg, ‏H.264, ‏ATRAC3+) | סרטון הפתיחה מתנגן עם תמונה, כתוביות וסאונד, בזמן אמת בחלון; התמונות זהות ל-FFmpeg בכל 27 הסרטונים ([פירוט](phase8-movies.md)) |
| 2026-10-16 | שלב 9: שמירות, הקלטת קלט ו-replay | שמירה וטעינה דרך קוד השמירה של המשחק; משחק בחלון שהוקלט חוזר ב-headless עם פריימים ו-WAV זהים ([פירוט](phase9-saves-replay.md)) |
| 2026-10-16 | שלב 10: Ghost of Sparta | פענוח PGD, תיקון במהדר, ‏coroutines: מהתפריט הראשי ועד קרב בשלב הראשון (Atlantis), בחלון ב-60 FPS עם סאונד וקלט; ‏CoO לא השתנה ([פירוט](phase10-ghost-of-sparta.md)) |
| 2026-10-16 | שלב 11: סיור אוטומטי בכל השלבים (CoO) | ‏`--warp`/`--god`: כל 75 האזורים שבמשחק ו-84 נקודות spawn רצו בלי קריסה ובלי HLE חסר; ‏16 אזורים לא נגישים עוצרים בטעינה, גם ב-PPSSPP: שאריות מהפיתוח, לא באג של ה-port ([פירוט](phase11-level-tour.md)) |

## 2026-10-16: ה-boot של Chains of Olympus, ובדיקות האימות

נעשה על commit `9f3b497` ועוד שינויים שעדיין לא נכנסו ל-commit: מפרש ה-GE ו-block transfers.

### ה-boot המקורי

Chains of Olympus עולה כקוד native, כלומר ה-EBOOT המתורגם ל-C בלי אמולטור. ב-4 שניות CPU הריצה מגיעה ל-150 שניות של זמן וירטואלי.

בדרך:
- נוצרים כל התהליכונים של המשחק.
- נטענים מהדיסק ספריות הסאונד (Scream / snd_stream) ומוזיקת התפריט `MAINMENU.AT3`.
- המשחק מגיע ללולאה הראשית שלו, 60 פריימים לשנייה בזמן וירטואלי.

![boot log](../media/screenshots/2026-10-16_01_coo_boot_log.png)

### בדיקות אימות

- **בדיקת VFPU עצמית:** 66/66.
- **פענוח אופרנדים של VFPU מול Ghidra:** 2460/2460.
- **בדיקה דיפרנציאלית מול Unicorn:** 0 פונקציות עם אי-התאמות בשני המשחקים. שלוש תוצאות שונות הן קידוד NaN של המעבד המארח, וזה צפוי.
- **כיסוי המהדר:**
  - Chains of Olympus: 15,935/15,935 פונקציות.
  - Ghost of Sparta: 20,328/20,328 פונקציות.

![validation](../media/screenshots/2026-10-16_02_validation.png)

### סטטיסטיקות GE

לפני שיש renderer, מפרש רשימות התצוגה רק סופר מה המשחק מבקש לצייר. אחרי 150 שניות וירטואליות:

| מדד | ערך |
|---|---|
| רשימות תצוגה | 8,996 |
| פרימיטיבים | 20 מיליון |
| מתוכם עם תאורה | 18.4 מיליון |
| מתוכם עם skinning | 8.9 מיליון |
| העתקות בלוקים | 686 אלף |
| רשימות שלא הסתיימו | 0 |

הטבלאות קובעות מה ה-renderer צריך לתמוך בו:
- פורמטים של vertex
- טקסטורות CLUT מסוג swizzled
- מצבי blending

![GE statistics](../media/screenshots/2026-10-16_03_coo_ge_stats.png)

## 2026-10-16: התפריט הראשי של Chains of Olympus על המסך

נעשה על commit `9f3b497` ועוד השינויים שעדיין לא נכנסו ל-commit (מפרש ה-GE וה-renderer). הפירוט ב-[phase4-ge.md](phase4-ge.md).

### הפריים הראשון

ה-renderer התוכנה צייר את מסך הזכויות של Sony ו-Ready At Dawn. זו התמונה הראשונה מהמשחק שצוירה על ידי ה-port.

![הפריים הראשון](../media/screenshots/2026-10-16_04_coo_first_rendered_frame.png)

### התפריט הראשי

אחרי כ-860 פריימים, בשנייה הווירטואלית 14.4, מופיע התפריט:
- Kratos בתלת-ממד, עם תאורה ו-skinning.
- העיר הבוערת עם עשן ואש.
- הלוגו והתפריט: New Game, ‏Load Game, ‏Options, ‏Treasures.

![התפריט הראשי](../media/screenshots/2026-10-16_05_coo_main_menu.png)

### ביצועים: v1 מול v2

- **v1** צייר ב-thread אחד: ‏3.6 FPS בתפריט.
- **v2** מצייר בכל 20 ה-threads של המעבד, בפסים שמחולקים דינמית, ומשתמש ב-palette מורחבת מראש: ‏24–28 FPS בתפריט.
- **הפלט זהה פיקסל-אחר-פיקסל.**
- **ה-boot כולו**, עד פריים 1343, ירד מ-140 שניות ל-22 שניות. זה כמעט זמן אמת: 1.02x.

![זמני פריים](../media/screenshots/2026-10-16_06_perf_frame_times_v1_v2.png)

![סיכום הריצה](../media/screenshots/2026-10-16_07_coo_v2_run_summary.png)

### הקלטות

כל ההקלטות נמצאות ב-`media/videos/`.

| קובץ | מה רואים |
|---|---|
| `2026-10-16_coo_boot_to_menu_v1_gameclock.mp4` | ה-boot עד התפריט, במהירות המשחק האמיתית: ‏22.5 שניות. |
| `2026-10-16_coo_boot_to_menu_v1_hostclock_1thread.mp4` | v1 לפי שעון המחשב: ‏140 שניות. |
| `2026-10-16_coo_boot_to_menu_v2_hostclock_20threads.mp4` | v2 לפי שעון המחשב: ‏22 שניות. |
| `2026-10-16_coo_perf_v1_vs_v2_hostclock.mp4` | v1 ו-v2 זה לצד זה, עם מונה FPS ומספר פריים חיים. |

זמני הפריים הגולמיים נמצאים ב-`media/perf/*.csv`.

## 2026-10-16: חלון, קלט, ונכנסים למשחק

הפירוט ב-[phase5-window-input.md](phase5-window-input.md).

### הדרך לשלב הראשון

ל-port יש עכשיו חלון אמיתי (SDL3) שרץ בזמן אמת, וקלט ממקלדת ומגיימפד. בדרך לשלב הראשון היו צריך לפתור שני מכשולים:
- **סרטון הפתיחה:** הוא הפיל את המשחק. sceMpeg מומש בלי decoder, כמו PPSSPP בלי FFmpeg.
- **coroutines של הסקריפטים:** המשחק מחליף בהן מחסנית, וזה לא ניתן לביטוי בקוד סטטי. הן מומשו על fibers.

![בחירת רמת קושי בחלון](../media/screenshots/2026-10-16_08_window_difficulty.png)

![השלב הראשון: The Shores of Attica](../media/screenshots/2026-10-16_09_coo_shores_of_attica_title.png)

![ה-tutorial בחלון](../media/screenshots/2026-10-16_10_window_tutorial.png)

![קרב בחלון: Blades of Chaos, קומבו של 4](../media/screenshots/2026-10-16_09_window_combat.png)

צילומי החלון נלקחו מהחלון של ה-port בלבד, עם PrintWindow (`tools/capture_window.ps1`). הלחיצות בהם הגיעו מתסריט קלט (`--input`).

### הקלטת משחק

הלחיצות מגיעות מתסריט קבוע (`media/perf/2026-10-16_v3_gameplay_input.txt`). לכן ההקלטה ניתנת לשחזור, ואפשר להשוות אותה בין גרסאות. התסריט:
- עובר בתפריט.
- סוגר את מסכי ה-tutorial.
- מבצע קומבואים: □□□△.
- מזיז את Kratos עם הסטיק.

| קובץ | מה רואים |
|---|---|
| `2026-10-16_coo_gameplay_shores_of_attica_gameclock.mp4` | 57 שניות של משחק במהירות האמיתית. השלב נפתח, יש קרבות, נאספים red orbs ומופיעים מסכי tutorial. |
| `2026-10-16_coo_gameplay_shores_of_attica_hostclock_v3_overlay.mp4` | אותו קטע לפי שעון המחשב, עם מונה FPS חי: כמה חלק זה נראה על המחשב הזה (181 שניות). |

![זמני פריים במשחק](../media/screenshots/2026-10-16_11_perf_gameplay_v3.png)

בשלב הראשון ה-renderer התוכנה נותן כ-19 FPS (‏52 ms לפריים), ולכן בחלון המשחק רץ לאט. יש כ-850 flushes לכל פריים של משחק: כמעט כל draw call משתמש ב-render target או בטקסטורה ב-VRAM. זה היעד הבא לשיפור ביצועים, לפני מעבר ל-renderer חומרה.

## 2026-10-16: צמצום ה-flushes (v4)

הפירוט ב-[renderer-decision.md](renderer-decision.md).

**מה השתנה:** התור של ה-renderer נמשך (flush) רק כשיש סכנת זיכרון אמיתית. ההחלטה מתבססת על אזורים דו-ממדיים וטווחי UV, במקום על "כל טקסטורה ב-VRAM".

**התוצאות:**
- מספר ה-flushes ירד פי 37.
- המשחק עלה מ-19 ל-26 FPS בזמן הקלטה, ול-29–35 FPS בלי הקלטה.
- הפלט זהה פיקסל-אחר-פיקסל.

![v3 מול v4](../media/screenshots/2026-10-16_12_perf_gameplay_v3_v4.png)

**השוואה בווידאו:** `media/videos/2026-10-16_coo_gameplay_v3_vs_v4_hostclock.mp4` מציג את v3 ואת v4 זה לצד זה, לפי שעון המחשב, עם מונה FPS חי.

## 2026-10-16: renderer חומרה על SDL3 GPU, שלב P1 (v5)

הפירוט ב-[phase6-gpu-renderer.md](phase6-gpu-renderer.md).

**מה נבנה:** renderer חומרה על SDL3 GPU. אותו קוד רץ על Direct3D 12 ועל Vulkan. ה-renderer התוכנה נשאר, ומשמש fallback ו"אורקל" לבדיקות. במצב `--renderer both` שניהם רצים יחד, ו-`--gpu-compare-at` משווה ביניהם פריים מול פריים.

**התוצאות:**
- **התאמה:** ‏44–49 dB בתפריטים, ‏34–36 dB במשחק.
- **מהירות:** במשחק, headless, ה-GPU נותן 77.5 FPS לעומת 29.5 FPS בתוכנה. בזמן הקלטה: 62.4 לעומת 25.1.
- **בחלון:** ‏57–60 FPS בזמן אמת.

![השוואה במשחק: תוכנה | GPU | הבדל ×4](../media/screenshots/2026-10-16_14_gpu_compare_gameplay.png)

![זמני פריים: תוכנה מול GPU](../media/screenshots/2026-10-16_16_perf_gameplay_v4_v5_gpu.png)

![החלון עם ה-renderer החומרה](../media/screenshots/2026-10-16_15_window_gpu_renderer.png)

**מה עוד חסר:** במצב GPU בלבד, המקומות שבהם המשחק דוגם את ה-framebuffer כטקסטורה מציגים רעש: החלק התחתון של מסך המשחק, ורקע התפריט. זה render-to-texture, היעד של P2.

**השוואה בווידאו:** `media/videos/2026-10-16_coo_gameplay_v4_vs_v5_gpu_hostclock.mp4` מציג את v4 (תוכנה) ואת v5 (GPU) זה לצד זה, עם מונה FPS חי.

## 2026-10-16: ניהול framebuffers ו-render-to-texture, שלב P2 (v6)

הפירוט ב-[phase6-gpu-renderer.md](phase6-gpu-renderer.md#p2-ניהול-framebuffers).

**מה נבנה:** ה-GPU מנהל render targets כאזורים ב-VRAM.
- **draws בהיסט:** draw לכתובת בתוך target קיים נכנס אליו בהיסט המתאים.
- **טקסטורה מ-target:** טקסטורה שנמצאת במה שה-GPU צייר מועתקת ל-snapshot באותה נקודה בפריים.
- **stencil:** לכל target יש stencil אמיתי. לפני שה-target נקרא, ערכי ה-stencil מועתקים לערוץ האלפא, כמו ב-PSP.
- **block transfers:** בין targets נעשית העתקה על ה-GPU, ויש העלאה מהזיכרון וקריאה חזרה אליו.
- **נקודות וקווים:** מצוירים על ה-GPU.

**איך זה התגלה:** המעקב הראה שה-glow מסנן פיקסלים לפי מזהה חומר שנכתב ב-stencil, ונקרא כאלפא של טקסטורה. הוא גם הראה שה-atlas של הצללים ב-565 מתנקה במילוי ב-8888 מעל אותו זיכרון. כלי ההשוואה החדש `RT_GPU_CHECK_FLIP` מצא את שני המקרים.

**התוצאות:**
- **בלי רעש:** אין יותר רעש במצב GPU בלבד. ה-glow, הצללים המוקרנים ורקע התפריט נוצרים על ה-GPU.
- **התאמה מול התוכנה:** ‏41–49 dB בתפריטים, ‏34.1–34.8 dB במשחק, עכשיו בלי עזרה מהזיכרון של התוכנה. רק 0.03%–0.04% מהפיקסלים סוטים ביותר מ-32.
- **התוכנה לא השתנתה:** ה-renderer התוכנה זהה פיקסל-אחר-פיקסל ל-v3.
- **מהירות:**
  - בחלון: **60.0 FPS יציב במשחק**, בדיוק בזמן אמת.
  - headless: ‏116 FPS במשחק (ב-P1 77.5).
  - בזמן הקלטה: 79.0 FPS (ב-P1 62.4).
- **תיקון ביצועים בדרך:** block transfers פוסלים עכשיו רק את הטקסטורות שבדפים שהם כתבו. ה-hash של הטקסטורות ירד מ-15% מזמן ה-CPU לכמעט כלום.

![P1 מול P2 במצב GPU בלבד](../media/screenshots/2026-10-16_17_gpu_p1_vs_p2_render_to_texture.png)

![השוואה במשחק, P2: תוכנה | GPU | הבדל ×4](../media/screenshots/2026-10-16_18_gpu_p2_compare_gameplay.png)

![זמני פריים: v4, ‏v5, ‏v6](../media/screenshots/2026-10-16_20_perf_gameplay_v4_v5_v6.png)

![החלון עם P2, ‏60 FPS](../media/screenshots/2026-10-16_19_window_gpu_p2.png)

צילומי החלון נלקחו מהחלון של ה-port בלבד, עם PrintWindow (`tools/capture_window.ps1`).

**השוואה בווידאו:** `media/videos/2026-10-16_coo_gameplay_v5_vs_v6_gpu_p2_hostclock.mp4` מציג את P1 ואת P2 זה לצד זה, לפי שעון המחשב, עם מונה FPS חי.

## 2026-10-16: סאונד (שלב 7)

נעשה על commit `8909f7f` ועוד השינויים שעדיין לא נכנסו ל-commit. הפירוט ב-[phase7-audio.md](phase7-audio.md).

**מה נבנה:**
- **sceAudio** (`rt_audio.c`): הערוצים מעורבבים על השעון הווירטואלי. בחלון הם מתנגנים דרך SDL3, ו-`--wav` כותב את כל המיקס בזמן המשחק.
- **ATRAC3+** (`rt_atrac.c`): המוזיקה. זה port של מכונת המצבים של PPSSPP, עם ה-decoder של FFmpeg בגרסה של PPSSPP ‏(`at3_standalone`, ‏LGPL). ‏CMake מוריד אותו, והוא לא נכנס לגיט.
- **SAS** (`rt_sas.c`): האפקטים. ‏VAG, ‏ADSR, ‏pitch, ווליום, רעש ו-reverb.

**התוצאות:**
- **מה שומעים:** מוזיקת התפריט, הקטע של New Game ומוזיקת השלב הראשון. בשלב יש גם את הסביבה והקרב: 1,098 key-ons של קולות SAS בריצת התסריט.
- **דיוק:** הפענוח של ATRAC3+ זהה ל-FFmpeg עד 1–2 LSB ‏(83–85 dB), והלולאה של מוזיקת התפריט חלקה עד הדגימה הבודדת. ה-VAG מתאים ל-`adpcm_psx` של FFmpeg ב-58–64 dB, וההפרש הוא עיגול.
- **בחלון:** ‏60 FPS במשחק (ממוצע 59.97, ‏p99 ‏18.7 ms), ‏0 underruns ב-85 שניות, ותור של ‏~51 ms.
- **דטרמיניזם:** ה-WAV זהה בית-לבית בין שתי ריצות headless, ובין headless לחלון. גם הפריימים של שתי ריצות ה-headless זהים.
- **פריימי ייחוס חדשים:** ה-threads של הסאונד ממתינים עכשיו כמו בחומרה, ולכן התזמון של המשחק השתנה. במשחק, המצב בפריים נתון שונה מ-v3. הייחוס להשוואות הוא עכשיו `build/rec_v7`.

![קרב בחלון, עם סאונד](../media/screenshots/2026-10-16_21_window_combat_with_sound.png)

![המיקס של ריצת התסריט](../media/screenshots/2026-10-16_22_audio_spectrogram_gameplay.png)

![סיכום ריצת החלון](../media/screenshots/2026-10-16_23_window_audio_summary.png)

הספקטרוגרמה נוצרה מה-WAV של הריצה עם `tools/audio_report.py`. צילום החלון נלקח מהחלון של ה-port בלבד (`tools/capture_window.ps1`).

**הקלטה עם סאונד:** `media/videos/2026-10-16_coo_gameplay_with_sound_gameclock.mp4` היא ריצת התסריט במהירות המשחק, 85 שניות. הווידאו (`--record`) וה-WAV (`--wav`) נכתבו באותה ריצה, ושניהם מתחילים בזמן וירטואלי 0, ולכן הם מסונכרנים.

## 2026-10-16: סרטונים (שלב 8)

נעשה על commit `9ca6fa6` ועוד השינויים שעדיין לא נכנסו ל-commit. הפירוט ב-[phase8-movies.md](phase8-movies.md).

**מה נבנה:**
- **sceMpeg** (`rt_mpeg.c`): ‏demuxer של PSMF שמושך packets מה-ring של המשחק רק כשמבקשים access unit. התמונות נכתבות ל-buffer של המשחק בפורמט שהוא מבקש, והסאונד יוצא דרך קריאות ה-sceAudio של המשחק עצמו.
- **H.264:** ה-decoder של OpenH264 ‏(BSD-2-Clause). ‏CMake מוריד את 105 הקבצים שלו מ-tag ‏v2.6.0 ובודק כל אחד ב-SHA-256 (`cmake/openh264-v2.6.0.sha256`). הקוד לא נכנס לגיט.
- **ATRAC3+:** אותו decoder של שלב 7.
- **`k_eat`:** בין התמונות המשחק מסתובב בלולאה עסוקה. עכשיו הלולאה מקדמת את השעון הווירטואלי, כמו ה-`hleEatCycles` של PPSSPP, ובחלון השעון נשאר צמוד לשעון האמיתי.
- **`--movie-test`:** מפענח קובץ PMF בלי המשחק, להשוואה מול FFmpeg. ‏`--extract` מקבל עכשיו גם תיקייה (רשימה עם הסקטורים) ושמות `sce_lbn`.

**התוצאות:**
- **סרטון הפתיחה** (`dx_persinv.pmf`, ‏60 שניות) מתנגן אחרי New Game, עם הכתוביות שהמשחק מצייר מעל התמונה. קודם הוא עבר ב-1.3 שניות של מסך שחור.
- **דיוק:** כל התמונות של כל 27 הסרטונים בדיסק זהות בית-לבית לפענוח של FFmpeg. הסאונד במיקס של המשחק זהה לדגימה לפענוח הקובץ, ובלי סחיפה.
- **סנכרון:** כל תמונה מוצגת בטווח ‏±1.1 ms מה-timestamp שלה ביחס לסאונד.
- **בחלון:** הסרטון בזמן אמת, ב-29.99 FPS, שזה הקצב שלו, עם p99 של 34.2 ms. השלב שאחריו ב-60.05 FPS. ‏0 underruns, וה-WAV זהה לריצת headless. באחת מחמש ריצות החלון התהליך נעצר פעמיים, ל-269 ול-596 ms, והסיבה לא נמצאה. ‏`[io] slow disc read` מדפיס עכשיו קריאות איטיות מהדיסק.
- **דטרמיניזם:** שתי ריצות headless נתנו פריימים ו-WAV זהים. עד הסרטון הכול זהה ל-`rec_v7`.
- **ייחוס פריימים חדש:** הסרטון מוסיף 1,794 פריימים, ולכן הייחוס להשוואות הוא עכשיו `build/rec_v8`. התסריט שלו הוא של `rec_v7`, כשהלחיצות שאחרי הסרטון זזות ב-1,794 פריימים. המשחק שאחרי הסרטון קרוב ל-`rec_v7` אבל לא זהה לו: העולם המשיך לרוץ במשך הסרטון, ולכן הקרב מתפתח אחרת.

![סרטון הפתיחה בחלון](../media/screenshots/2026-10-16_24_window_intro_movie.png)

![ארבעה רגעים מהסרטון בחלון](../media/screenshots/2026-10-16_25_window_intro_movie_sequence.png)

![המיקס: התפריט, הסרטון, השלב](../media/screenshots/2026-10-16_26_audio_intro_movie.png)

![כל 27 הסרטונים מול FFmpeg](../media/screenshots/2026-10-16_27_movie_test_all_vs_ffmpeg.png)

![סיכום ריצת החלון](../media/screenshots/2026-10-16_28_window_movie_summary.png)

צילומי החלון נלקחו מהחלון של ה-port בלבד, עם PrintWindow (`tools/capture_window.ps1`). הספקטרוגרמה נוצרה מה-WAV של ריצת החלון עם `tools/audio_report.py`, והטבלאות הן פלט אמיתי דרך `tools/render_terminal.py`.

**הקלטה עם סאונד:** `media/videos/2026-10-16_coo_intro_movie_with_sound_gameclock.mp4` היא 97 שניות במהירות המשחק: התפריט, סרטון הפתיחה ותחילת השלב. הווידאו (`--record`) וה-WAV (`--wav`) נכתבו באותה ריצה.

## 2026-10-16: שמירות, הקלטת קלט ו-replay (שלב 9)

נעשה על commit `e7b118a` ועוד השינויים שעדיין לא נכנסו ל-commit. הפירוט ב-[phase9-saves-replay.md](phase9-saves-replay.md).

**מה נבנה:**
- **sceUtilitySavedata** (`rt_savedata.c`): לפי `PSPSaveDialog` ו-`SavedataParam` של PPSSPP.
  - השמירות בתיקיות `ms0:/PSP/SAVEDATA/<gameName><saveName>/` (`build/ms0` ב-`tools/play.cmd`), עם קובץ הנתונים, `PARAM.SFO` ו-`ICON0.PNG`.
  - זרימת הסטטוס בזמן וירטואלי, וקודי החזרה של PPSSPP.
  - בלי הצפנה: כמו PPSSPP כשהצפנת השמירות כבויה.
  - אין רשימה על המסך, אז ה-port בוחר: שמירה למשבצת הריקה הראשונה (אחר כך לישנה ביותר), וטעינה של החדשה ביותר. ‏`--save-slot` קובע משבצת.
- **`--record-input`:** כל שינוי במה שהמשחק קרא מהבקר, לפי הפריים, בפורמט של `--input`. ‏`--input` מקבל עכשיו גם קובץ.
  - בחלון הבקר נדגם פעם בפריים.
  - ההקלטה נסגרת גם בקריסה.
- **‏`tools/play.cmd`** מקליט כל משחק ל-`build/sessions/<תאריך_שעה>/`, עם עותק של השמירות שהיו בהתחלה. **`tools/replay.cmd`** מריץ אותו שוב headless, עד הנקודה שבה המשחק נעצר או קרס.
- **השעון של ה-guest** (`sceRtc`, ‏`sceKernelLibcTime`) זז עם הזמן הווירטואלי מנקודת התחלה שנשמרת בהקלטה.
- **`--fast-forward N`:** לא מצייר עד פריים N, והמשחק רץ אותו דבר, פי 2 מהר יותר.

**התוצאות:**
- **שמירה וטעינה:**
  - שמירה באמצע השלב הראשון דרך בקשת השמירה של המשחק עצמו: ‏LISTSAVE ל-`DATA00`, ‏14,010 בתים, ואחריה הפרופיל, 72 בתים.
  - אחרי הפעלה מחדש, Load Game טוען את השמירה, והמשחק חוזר לאותו חדר עם אותם 520 red orbs.
  - שמירה שנייה הולכת ל-`DATA01`, והטעינה בוחרת בה.
  - את נקודת השמירה הראשונה (אחרי הקרב ב-Basilisk) ריצת התסריט לא השיגה. היא הגיעה עד ה-Basilisk והשלימה את ה-QTE הראשון שלו.
- **replay:** משחק בחלון עם מקלדת אמיתית (הודעות מקלדת לחלון של ה-port), שהוקלט והורץ שוב ב-headless:
  - ב-renderer תוכנה: ‏5,421 פריימים. ‏45/45 פריימים זהים, ‏WAV זהה, ואותם זמן וירטואלי ומספר החלפות thread.
  - ב-GPU: ‏6,695 פריימים בזמן אמת. ‏55/55 פריימים זהים ו-WAV זהה. גם replay ב-renderer תוכנה נותן אותו WAV.
- **ייחוס פריימים חדש: `build/rec_v9`**, עם `ms0` ריק. ההשהיה של 200 ms בפתיחת דיאלוג השמירה ב-boot, כמו ב-PPSSPP, מזיזה את מה שאחריה.

![שמירה, יציאה, טעינה](../media/screenshots/2026-10-16_29_save_load_roundtrip.png)

![משחק בחלון מול ה-replay שלו](../media/screenshots/2026-10-16_30_replay_window_vs_headless.png)

המסגרות נשמרו מה-framebuffer של ה-PSP על ידי הריצות עצמן (`--frames-dir`). בשורה העליונה של התמונה השנייה, ריצת החלון עם `--renderer gpu`, כך שהמסגרות שלה נקראו מה-GPU.

## 2026-10-16: Ghost of Sparta (שלב 10)

נעשה על commit `29b9638` ועוד השינויים שעדיין לא נכנסו ל-commit. הפירוט ב-[phase10-ghost-of-sparta.md](phase10-ghost-of-sparta.md).

**מה נבנה:**
- **פענוח PGD** (`rt_io.c`): ‏`sceIoIoctl` ‏`0x04100001`/`02`/`10` לפי `sceIo` של PPSSPP, עם המפתח שהמשחק עצמו נותן. הפענוח הוא `ext/libkirk` של PPSSPP. ‏CMake מוריד אותו ובודק אותו ב-SHA-256, ושום מפתח לא נכנס לגיט.
  - בדיסק יש קובץ PGD אחד, ‏`ATT_000_STARTSCREEN.BIN`. זו בדיקת מקוריות שהמשחק חוזר עליה בלולאה עד שהיא מצליחה.
- **‏`scePspNpDrm_user`** לפי PPSSPP. המשחק בודק אם יש DLC ב-`ms0`.
- **‏`sceUtilityLoadModule`** לפי PPSSPP: כך GoS טוען את הקודקים.
- **‏`__sceSasSetVoicePCM`:** קולות PCM ב-SAS.
- **תיקון במהדר מחדש:** פונקציה שהגוף שלה מתחיל מתחת ל-entry מתחילה עכשיו בקפיצה ל-entry. ב-GoS זו פונקציה אחת, והיא תקעה את המשחק בלולאה אינסופית בשנייה 24.4. ב-CoO אין אף אחת.
- **ה-coroutines של GoS:** ה-context switch ב-`0x08AD775C`, ב-`recomp/runtime_funcs.json`.
- **‏`tools/play_gos.cmd`:** משחק בחלון עם `--renderer gpu`, ושמירות ב-`build\ms0_gos`. ‏`tools/replay.cmd` מזהה עכשיו גם סשנים של GoS.

**התוצאות:**
- **מה-boot ועד השלב הראשון:** האזהרה, הלוגואים, התפריט הראשי, רמת הקושי, סרטון הפתיחה (105 שניות) והשלב Atlantis. בשלב יש קרב, ‏tutorials, ‏QTE ו-cutscenes במנוע.
- **בחלון** (`--renderer gpu`):
  - 60 FPS בשלב, 58.5 בתפריט, והסרטון ב-30.
  - הסאונד במיקס זהה בית-לבית לריצת headless.
  - הדרך לשלב שוחקה גם עם הודעות מקלדת אמיתיות לחלון, וה-replay של ההקלטה נתן WAV זהה.
- **דיוק:** כל 34 הסרטונים שבדיסק זהים בית-לבית ל-FFmpeg (60,637 תמונות). המוזיקה מתאימה לפענוח של FFmpeg ב-75–80 dB.
- **CoO לא השתנה:** ‏34/34 פריימים זהים ל-`rec_v9` ב-renderer התוכנה, ו-WAV זהה.

![רצף ה-boot של GoS](../media/screenshots/2026-10-16_31_gos_boot_sequence.png)

![התפריט הראשי בחלון](../media/screenshots/2026-10-16_32_gos_window_main_menu.png)

![קרב ב-Atlantis בחלון](../media/screenshots/2026-10-16_33_gos_window_atlantis_combat.png)

![לוג ה-boot](../media/screenshots/2026-10-16_34_gos_boot_log.png)

![רגעים מריצת החלון](../media/screenshots/2026-10-16_35_gos_window_sequence.png)

![המיקס של ריצת החלון](../media/screenshots/2026-10-16_36_gos_audio_spectrogram.png)

![ריצת המקלדת בחלון](../media/screenshots/2026-10-16_37_gos_window_keyboard_session.png)

צילומי החלון נלקחו מהחלון של ה-port בלבד, עם PrintWindow (`tools/capture_window.ps1`). רצף ה-boot נשמר מה-framebuffer של ה-PSP על ידי הריצה עצמה (`--frames-dir`, ‏`--renderer gpu`). הספקטרוגרמה נוצרה מה-WAV של ריצת החלון עם `tools/audio_report.py`, והלוג הוא פלט אמיתי דרך `tools/render_terminal.py`.
