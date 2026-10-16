# שלב 11: סיור אוטומטי בכל השלבים

**תאריך:** 2026-10-16 · **branch:** `phase2/threads-and-fs` · **קוד:** `runtime/rt_debug.c` ו-`rt_debug.h` (חדשים: ‏`--warp`, ‏`--god`, ‏`--level-log`, ‏`--load-level`, ‏`--dump-ram`), שורת קריאה ב-`runtime/rt_host.c` ופירוק האפשרויות ב-`runtime/rt_main.c`, ‏`tools/level_tour.py`, ‏`tools/hle_audit.py`, ‏`tools/hle_calls.py` (חדשים)

## השורה התחתונה

- **כל האזורים של Chains of Olympus נבדקו בלי לשחק.** יש 75 אזורים שהמשחק מגיע אליהם: 70 בסיפור ו-5 אתגרים.
  - בכל אחד: boot, ‏`--warp` לאזור, ו-90 שניות של תסריט קלט כללי עם `--god`, ב-GPU וב-both.
  - ועוד 84 ריצות שהתחילו מכל נקודת spawn אחרת באזורים.
  - **אין קריסה, trap או deadlock. אין פונקציית HLE חסרה, ואין קובץ שלא נמצא.** כל שלושת הסרטונים בדרך התנגנו עד הסוף.
- **‏`--warp LEVEL[:SPAWN]`:** ה-port קורא לפונקציה של המשחק עצמו שמתחילה משחק באזור נתון. זו הפונקציה של תפריט Challenge of the Gods (`0x08976FE8`). היא נקראת מהתפריט הראשי בפריים 1000, דטרמיניסטית. ‏**`--god`** מחזיק את הבריאות של Kratos מלאה: ה-component שלה נמצא בהשוואת dumps של ה-RAM.
- **‏16 קבצים עוצרים בטעינה,** ב-assert של `free` של המשחק. כולם אזורים שאף אזור אחר לא מזכיר. הגרף נבנה מהשמות שבקבצים, וכל 93 האזורים והתוספות שמחוברים אליו נטענים. כנראה אלה שאריות מהפיתוח, אבל זה עוד לא אומת ב-PPSSPP (באג 1).
- **ה-audit של ה-HLE:**
  - ל-199 ה-imports של CoO יש handler לכולם: ‏158 ממומשים ו-41 stubs שמחזירים קבוע.
  - אף שלב מאוחר לא קורא לפונקציה שההתחלה של המשחק לא קוראת לה.
  - ה-stubs שנקראים בטוחים. הסיכון נמצא במסלולים לא נבדקים בתוך פונקציות ממומשות.
- **ה-renderers:** ‏294 השוואות בין התוכנה ל-GPU, חציון 36.6 dB. באזורים שבמשחק לכל היותר 0.19% מהפיקסלים שונים ביותר מ-32. לא נראה הבדל בעין.
- **נקודת שמירה אמיתית נבדקה:** התסריט נתקל במזבח ב-`foa_path_a`, והמשחק שמר את "The Falls of Oceanum". ‏Load Game החזיר את Kratos לשם. זה הסעיף הפתוח של שלב 9.
- **קצב (headless, ‏`--renderer gpu`, כל אזור לבד):** כל 73 האזורים מעל 60 FPS, מינימום 66.6 וחציון 112.6. בשלושה אזורים 14–40% מהפריימים לוקחים יותר מ-16.7 ms, ובחלון זה יאט את המשחק לרגעים: `foa_path_c`, ‏`ec_5_parry_challenge`, ‏`tar_caves_05_titanhead` (באג 2).
- **איך המשחק טוען שלב:** מנהל שלבים עם streaming של אזורים שכנים, טעינה נדחית באמצע update, ונקודות spawn בכל אזור. הכתובות והמבנים מתועדים למטה. אין במשחק תפריט debug ואין בחירת שלבים.

## ה-audit של ה-HLE

‏`tools/hle_audit.py` עובר על כל ה-imports של שני המשחקים (`build/gen/<GAME>_full/module.json`), מוצא לכל אחד את ה-handler בטבלאות `HleEntry` של `runtime/*.c` (לפי השם, כמו `rt_hle.c`), ומסווג:
- **ממומש:** handler שעושה משהו. גם handler של שורה אחת שמחזיר מצב, כמו `sceKernelGetThreadId`, נחשב ממומש.
- **stub:** כל הגוף הוא `RET(מספר)`, כמו `hle_Zero`.
- **חסר:** אין handler. ‏`rt_hle` רושם `UNIMPLEMENTED` פעם אחת ומחזיר 0.

לכל import הוא סופר גם את מספר הקריאות אליו בקוד המתורגם.

| משחק | imports | ממומשים | stubs | חסרים |
|---|---|---|---|---|
| Chains of Olympus ‏(UCUS98653) | 199 | 158 | 41 | 0 |
| Ghost of Sparta ‏(UCUS98737) | 206 | 166 | 40 | 0 |

הספירה של Ghost of Sparta היא אחרי commit `385773d` (שלב 10). לפניו `__sceSasSetVoicePCM` עוד היה חסר.

**אילו stubs שלבים מאוחרים פוגשים.** ‏`tools/hle_calls.py` מריץ קטע מהמשחק עם `--trace-hle` ושומר רק את רשימת הפונקציות שנקראו ואת מספר הקריאות. הוא הריץ:
- את ההתחלה: boot, התפריט, New Game, סרטון הפתיחה והשלב הראשון עד פריים 6,800 (הקלט של `build/rec_v9`).
- 10 שלבים מכל אזורי המשחק, 40 שניות כל אחד, דרך `--warp`: ‏`pi_king_fight`, ‏`mar_02_village`, ‏`ht07_throne_room`, ‏`charon_galley_1`, ‏`tar_caves_04_maintitan`, ‏`poh_persephone_chamber`, ‏`ef_03_crash_landing`, ‏`foa_deathknight_arena`, ‏`ec_3_eos_chamber`, ‏`cog_01`.

**אף שלב מאוחר לא קורא לפונקציית HLE שההתחלה לא קוראת לה.** ההתחלה קוראת ל-145 פונקציות, וכל אחד מהשלבים ל-112 עד 136 מהן. רק בהתחלה נקראות שבע פונקציות של סגירת סרטון ו-`sceKernelDeleteEventFlag`: רק שם סרטון נגמר בתוך הריצה. גם ב-`poh_persephone_chamber` וב-`ec_3_eos_chamber` מתנגן סרטון, אבל הוא לא נגמר תוך 40 שניות.

ה-stubs שנקראים, ומה הסיכון:

| stub | מחזיר | איפה נקרא | סיכון |
|---|---|---|---|
| ‏`sceMpegAvcDecodeFlush`, ‏`sceMpegRingbufferDestruct` | 0 | בסוף כל סרטון | נמוך. ב-PPSSPP ה-flush מוציא תמונות שנשארו ב-decoder. אצלנו OpenH264 מוציא כל תמונה מיד, וב-`--movie-test` יצאו כל התמונות של כל 27 הסרטונים ([שלב 8](phase8-movies.md)). |
| ‏`sceKernelCheckCallback` | 0 | בכל פריים. בזמן סרטון 1.6–2.5 מיליון פעמים | נכון: אין callbacks שצריך להפעיל. המספר הגדול הוא הלולאה העסוקה של נגן הסרטונים, ו-`k_eat` כבר מטפל בזמן שלה. |
| ‏`sceGeListSync`, ‏`sceKernelDcacheWriteback*` | 0 | בכל פריים | נכון: ה-GE מסיים כל רשימה מיד, ואין cache. |
| ‏`sceKernelIsCpuIntrEnable` | 1 | 20–200 אלף פעמים בריצה | נכון במתזמן שלא קוטע threads. |
| ‏`sceUmd*`, ‏`scePower*`, ‏`sceCtrlSet*`, ‏`sceDisplaySetMode`, ‏`sceKernelPower*`, ‏`sceKernelVolatileMemUnlock`, ‏`sceImposeSetLanguageMode`, ‏`sceIoChangeAsyncPriority` | 0 / 1 | ב-boot או מעט פעמים | אין: הגדרות שאין להן משמעות ב-port. |
| ‏`sceKernelStart/Stop/UnloadModule`, ‏`sceKernelRegisterExitCallback`, ‏`sceKernelSetCompiled*` | 0 | ב-boot | אין: המודולים של המערכת מומשו ב-HLE. |
| ‏`sceGeEdramGetAddr/GetSize/SetAddrTranslation` | הערכים של ה-PSP | ב-boot | אין: אלה הערכים האמיתיים. |

תשעה stubs לא נקראו באף ריצה: ‏`sceKernelCpuSuspendIntr/ResumeIntr`, ‏`sceKernelStdin/out/err`, ‏`sceKernelGetModuleId*`, וגם `sceGeContinue` ו-`sceIoRename`, שאין אליהם אף קריאה בקוד. גם 45 פונקציות ממומשות לא נקראו, למשל:
- ‏`sceUtilityMsgDialog*` ו-`sceAudioOutput2*`.
- חלק מ-SAS: ‏`__sceSasCoreWithMix`, ‏`SetNoise`, ‏`SetSimpleADSR`, ‏`SetPause`.
- ‏`sceKernelExitGame`.
- ‏`sceIoWrite` ו-`sceIoMkdir`: השמירות עוברות ב-`sceUtilitySavedata`.
- ‏`sceRtcGetCurrentClockLocalTime`: רק בשמירה.

הרשימה המלאה ב-`build/tour/hle_audit.json`.

**מסקנה:** הסיכון בשלבים המאוחרים לא נמצא ב-stubs. הוא במסלולים לא נבדקים בתוך פונקציות ממומשות. דוגמאות: ה-reverb של SAS (עד עכשיו רק `RevType -1`), מצבי ה-buffer של ATRAC, ‏`LISTALLDELETE` בשמירות, ו-`sceIoIoctl` שמחזיר 0. הסיור לא פגש אף אחד מהם בצורה שעצרה את המשחק (ראו התוצאות).

## איך המשחק טוען שלב

### השלבים על הדיסק

כל שלב הוא קובץ `Pack` ב-`disc0:/PSP_GAME/USRDIR/data/<שם>.bin`, ‏1–6 MB. ליד רבים מהם יש קבצי שפה (`_eng`, ‏`_fre`, ‏`_ger`, ‏`_ita`, ‏`_spa`) עם הדיבור והכתוביות. ב-boot המשחק מונה את התיקיות ופותח קבצים לפי מספר הסקטור (`disc0:/sce_lbn0x86bc0_size0x594840`), ולכן ב-log רואים סקטורים ולא שמות. ‏`gow_rt --extract` מפרט את התיקייה עם הסקטורים, וכך ממפים חזרה לשמות.

114 קבצים הם שלבים:
- **83 אזורים:** `pi_*` (‏Attica, החוף והעיר), ‏`mar_*` (‏Marathon), ‏`ht*` (מקדש Helios), ‏`ec_*`, ‏`foa_*`, ‏`charon_*`, ‏`tar_*`, ‏`poh_*`, ‏`ef_*`, וחמשת האתגרים `cog_01`–`cog_05`. שמות האזורים כאן הם לפי הקבצים, לא לפי שמות הפרקים במשחק.
- **31 תוספות:** `*_enc*`, ‏`*_enemies`, ‏`*_pushbox`. המשחק טוען אותן ליד אזור, עם גלי האויבים או חלקים זזים.

### מנהל השלבים

המשחק לא טוען "שלב" אחד. הוא מחזיק כמה אזורים בזיכרון, ואזור טוען את השכנים שלו כש-Kratos מתקרב (streaming). הכתובות הן של ה-EBOOT בזיכרון (`0x08804000` + הכתובת ב-Ghidra).

| מה | איפה |
|---|---|
| מנהל השלבים (singleton) | מצביע ב-`0x08B3CB44` (`FUN_00282d1c`). ‏`+0x24` מערך האזורים הטעונים, ‏`+0x38` מספרם, ‏`+0x48` הקיבולת, ‏`+0x40` האזור הנוכחי, ‏`+0x4C` שם של טעינה שנדחתה |
| ‏`FindLevel(mgr, name)` | ‏`0x08833E78`: משווה ל-`level+0xB4` |
| ‏`LoadLevel(mgr, name, a, b)` | ‏`0x088347B8`: אם האזור לא טעון, ומערך האזורים מלא, משחרר את מי שה-vfunc ‏`+0x4C` בוחר. אחר כך `0x088342C0`: אם הדגל `0x08B2F268` דולק (המשחק באמצע update), רק שומר את השם ב-`mgr+0x4C` לטעינה מאוחרת. אחרת מקצה אזור ומתחיל לקרוא אותו |
| ‏`SetCurrentLevel(mgr, level)` | ‏`0x088343E0`: מחשב את ההפרש בין מרכז האזור הקודם לחדש, ומעביר אותו לשאר האזורים הטעונים ולעוד שתי מערכות |
| ‏`UnloadLevel(mgr, level)` | ‏`0x08834678`. באמצע update רק מסמן (`0x400` ב-`level+0x12C`) |
| מצב הטעינה | הבית התחתון של `level+0x12C`. בסיור: 3 בזמן הקריאה מהדיסק, 4 באתחול, 5 ומעלה טעון, 11 בסוף |
| נקודות spawn | ‏`level+0x134`: מערך של 0x1C בתים (מיקום ב-`+8`, סיבוב ב-`+0x14`), ומספרן ב-`level+0x148` |

**ה-streaming:** האזורים טוענים את השכנים שלהם דרך אירועים בסקריפט שלהם (`0x08834AAC`). הפרמטרים מזוהים ב-hash: ‏`0xCB26F255` הוא שם האזור, ‏`0xB37DDF5B` ו-`0xB098C32A` שני הדגלים של `LoadLevel`. גם "החלף אזור נוכחי" הוא אירוע כזה. שמות השכנים כתובים בקובץ האזור, ולכן אפשר לבנות את הגרף מהדיסק (`build/tour/level_graph.json`): מ-`pi_beach_00` דרך כל המשחק עד `ef_03_crash_landing_2`. הסדר שעולה מהגרף:

| אזור | האזורים, לפי הסדר |
|---|---|
| Attica | ‏`pi_beach_00` ← `pi_beach_01` ← `pi_beach_02` ← `pi_king_01` ← `pi_city_wall_00` ← `pi_city_wall_01` ← `pi_king_fight` ← `pi_city_wall_02` ← `pi_city_streets_01` ← `02` ← `03` ← `pi_bazaar_01` ← `pi_bath_house_01` ← `pi_connector` ← `pi_basilisk` |
| Marathon | ‏`mar_01_rock_climb` ← `mar_02_village` ← `mar_03_gate_puzzle` ← `mar_04_helios_approach` |
| מקדש Helios | ‏`ht01_courtyard_low` ← `ht02` ← `ht03_foyer_hallway` ← `ht04_elevator_mid` ← `ht06_astrolabe_low` ← `ht07_throne_room` ← `ht08_fire_pit` ← `ht11_courtyard_return` ← `ht12_stair_puzzle_1` ← `ht13_astrolabe_high` |
| ‏ec | ‏`ec_1_entrance` (ממנו גם `ec_0_underwater_ruins`) ← `ec_2_combat_arena` ← `ec_3_eos_chamber` ← `ec_5_parry_challenge` ← `ec_6_exit` |
| מקדש Helios, החזרה | ‏`ht14_astrolabe_fire` ← `ht15_elevator_return_low` ← `ht16_elevator_return_high` ← `ht17_balcony` ← `ht18_astrolabe_final` ← `ht19_stair_puzzle_2` ← `mar_05_steed_return` |
| ‏foa | ‏`foa_path_a` ← `foa_path_b` ← `foa_path_c` ← `foa_path_connector` ← `foa_deathknight_arena` |
| ‏Charon ו-tar | ‏`charon_galley_1` ← `tar_caves_01_welcomeparty` ← `tar_caves_01_5_zeusgate` (וממנו `tar_keeper_01_crypt` ↔ `tar_keeper_02_zeustemple`) ← `tar_caves_02_titanpit` ← `03_arenacavern` ← `04_maintitan` ← `05_titanhead` ← `06_zeusgate` ← חזרה ל-`foa_deathknight_arena` |
| ‏Charon ו-poh | ‏`charon_galley_2` ← `poh_01_entrance` ← `poh_02_hades_vista` ← `02b` ← `poh_03_elysium_vista` ← `03b` ← `poh_04_elevator_low` ← `poh_05_elevator_high` (וממנו `poh_04b_elevator_low`) ← `poh_persephone_chamber` |
| ‏ef | ‏`ef_01_fields` ← `ef_03_crash_landing` ← `ef_03_crash_landing_2` (הסוף: אין לו שכנים) |

70 אזורים בשרשרת אחת, ועוד חמשת האתגרים מהתפריט. **21 קבצים לא נגישים:** אף אזור לא מזכיר אותם (ו-`pi_bazaar_02` ו-`pi_temple_01` מוזכרים רק מאזורים לא נגישים): ‏`ec_4_hallway`, ‏`ht05_elevator_low`, ‏`ht09_light_room`, ‏`pi_bath_house_02`, ‏`pi_bazaar_02`, ‏`pi_temple_01`, ‏`poh_premod`, ‏`tar_caves_09_titanhead`, ועוד 13 תוספות. ייתכן שאלה שאריות מהפיתוח. הסיור מצא שרובם קורסים בטעינה (למטה).

### אובייקט המשחק

מצביע ב-`0x08B3655C` (`FUN_0022bfb4`):

| שדה | מה |
|---|---|
| ‏`+0x84` | ‏Kratos |
| ‏`+0x9C` | 1 ב-Challenge of the Gods |
| ‏`+0xA0` | שם האזור ש-Kratos בו. זה מה שנכנס לשמירה (`SECURE`, היסט `0x258`) |
| ‏`+0x120` | נקודת ה-spawn באזור הזה. ‏`0x088C1D18` מציב שם את Kratos |
| ‏`+0x70` | דגלים. ‏`0x40`: החלפת אזור ממתינה |

### "התחל באזור X": הפונקציה של האתגרים

תפריט Challenge of the Gods מתחיל כל אתגר בקריאה ל-`0x08976FE8` עם השם (`"cog_01"` ועד `"cog_05"`):
1. איפוס מצב המשחק (vfunc ‏`+0x64` של אובייקט המשחק).
2. ‏`0x088C3F6C`: משחרר אזור אם המערך מלא, `LoadLevel(name, 1, 0)`, מעתיק את השם ל-`+0xA0`, נקודת spawn 0, דגל `0x40`.
3. ‏`+0x9C = 1`, התלבושת הרגילה (`kratos_model`), ערכי הקושי.
4. סוגר את התפריט (`0x08961118`).

הפונקציה לא משתמשת בפרמטר הראשון, והיא עובדת עם כל שם של אזור. זו הדרך של ה-`--warp`.

**New Game** הולך בדרך אחרת: התפריט טוען מראש את `pi_beach_00` ליד `start_screen_demo` (`0x0896C610`), ובבחירה ב-New Game ‏(`0x0896CC74`) הוא הופך אותו לאזור הנוכחי ומציב בו את Kratos.

### אין תפריט debug ואין בחירת שלבים

אין שום מחרוזת של בחירת שלבים, debug, cheat או warp: לא ב-EBOOT, לא ב-`frontend_assets.bin` (התפריטים) ולא ב-`game.bin` (הטקסטים). "God Mode" שבטקסטים הוא רמת הקושי. ב-EBOOT יש טבלה אחת של 12 שמות אזורים (`0x088C51C0`), עם ארבעה hashes לכל אחד. כשאזור מהטבלה נטען, `0x088C4310` מצמיד לו את ה-hashes. אלה הגדרות לאזור, לא רשימת שלבים.

### הבריאות של Kratos

‏Kratos (`game+0x84`) מחזיק מערך components ב-`+0x24`. ה-component עם ה-vtable ‏`0x08B3E248` מחזיק את הבריאות: float ב-`+0x64` והמקסימום ב-`+0x70` (100 בהתחלה). ה-HUD (vtable ‏`0x08B4B084`) שומר עותק משלו ב-`+0x74`/`+0x78`, שמתעדכן אחרי ה-component.

כך זה נמצא:
1. ‏`--dump-ram` כתב את ה-RAM בשבעה פריימים, בזמן ש-Kratos עמד בלי לזוז והחיילים על החוף הכו בו.
2. חיפוש של float שיורד בין כל שני dumps מצא ארבעה ערכים: ‏100 ← 94 ← 93 ← 88 ← 84 ← 81.
3. אחד מהם הוא ה-component ברשימת ה-components של Kratos, עם המקסימום שלו לידו. אחד אחר הוא העותק של ה-HUD. שני האחרים הם עותקים במבנים אחרים, והם לא נבדקו.
4. **הבדיקה:** אותו קרב עם `--god`. פס הבריאות נשאר באורך מלא, ‏38 פיקסלים בכל הפריימים, במקום לרדת מ-37 ל-32. ‏`--god` מילא את הבריאות 13 פעמים.

## ה-port: ‏`rt_debug.c`

| אפשרות | מה |
|---|---|
| ‏`--warp LEVEL[:SPAWN][@FRAME]` | בקריאת הבקר הראשונה של פריים FRAME (ברירת מחדל 1000: התפריט הראשי אחרי boot עם `ms0` ריק) קורא ל-`0x08976FE8` עם LEVEL, ואז מחזיר את `+0x9C` ל-0 (משחק רגיל), חוץ מ-`cog_*`. ‏SPAWN כותב את `+0x120` |
| ‏`--god` | בכל קריאת בקר: הבריאות של Kratos חוזרת למקסימום. נפילה ומוות מיידי עדיין קורים, ונרשמים ב-log ("Kratos died") |
| ‏`--level-log` | כל שינוי ברשימת האזורים הטעונים: שם, מצב טעינה, האזור הנוכחי, ונקודת ה-spawn מתוך כמה. ‏`--warp` מדליק אותו |
| ‏`--load-level NAME@FRAME` | ‏`LoadLevel(mgr, NAME, 1, 0)` בפריים FRAME: כמו שאזור טוען את השכן שלו |
| ‏`--dump-ram N,...:PFX` | ה-RAM של המשתמש (24 MB) בפריימים N. ‏`stop` כותב אותו בסוף הריצה, גם אחרי trap |

הקריאות למשחק רצות ב-thread שקורא את הבקר (`user_main`), דרך `rt_call_guest`, שמחזיר אחר כך את כל הרגיסטרים. כך קריאת הבקר עצמה לא משתנה. הכול קורה בפריימים קבועים, ולכן הריצות נשארות דטרמיניסטיות.

**שינויים בקבצים משותפים:**
- שורה ב-`rt_host_buttons` (קריאה ל-`rt_debug_input`).
- ב-`rt_main.c`: שורה בלולאת האפשרויות, שורה בדוח של סוף הריצה ושורה ב-usage.
- הקובץ החדש ב-`CMakeLists.txt`.

בלי האפשרויות האלה `rt_debug_input` לא עושה כלום. ריצה של הקלט של `build/rec_v9` נתנה 15 מתוך 15 פריימים זהים ל-`rec_v9`, עד פריים 3,000 (`tools/check_frames.py`).

## הסיור: ‏`tools/level_tour.py`

לכל שלב:
1. boot עם `ms0` ריק. התפריט מצויר רק מפריים 990 (`--fast-forward`).
2. ‏`--warp` בפריים 1000, ו-`--god`.
3. תסריט קלט כללי בזמן וירטואלי, מ-20 שניות ועד 110: ריצה בשמונה כיוונים לסירוגין, קומבו של ריבוע ומשולש, קפיצה וקפיצה כפולה, תפיסה (עיגול), גלגול (L+R עם הסטיק), R (חסימה, ופתיחה של דלתות ותיבות), ואיקס שגם סוגר חלונות טקסט. ל-PSP אין סטיק שני, והמצלמה במשחק קבועה.
4. שתי ריצות:
   - **‏`--renderer gpu`:** כל 90 השניות. ‏`perf.csv`, ופריים כל 600 פריימים (10 תמונות) ל-contact sheet.
   - **‏`--renderer both`:** ‏`--gpu-compare-at` בפריימים 5400, ‏6000 ו-6600 (‏90–111 שניות וירטואליות, כלומר 70–90 שניות אחרי תחילת הקלט). ‏`--both-from 5300`: עד פריים 5240 לא מצויר כלום. ה-renderer התוכנה הוא החלק האיטי: ריצה מלאה ב-both לקחה 13 דקות לשלב כש-6 רצו יחד, כלומר יותר מארבע שעות לכל הסיור.
5. מה-log: איך הריצה נגמרה (מגבלת הפריימים, trap, deadlock, abort, קריסה או timeout), שורות `UNIMPLEMENTED`, קבצים שלא נמצאו, אילו אזורים נטענו ומי האזור הנוכחי בסוף, מספר נקודות ה-spawn, ומה `--god` עשה.

התוצאות ב-`build/tour/tour1/`: לכל שלב `gpu/` ו-`both/` (‏`run.log`, פריימים, `compare_*.png`, ‏`perf.csv`) ו-`sheet.png`. בתיקייה עצמה `summary.json`, ‏`summary.md` ו-`overview.png`.

## התוצאות

### במספרים

- **114 קבצים, 98 נטענו, 16 עצרו ב-trap.** כל 16 ה-traps הם באזורים שאף אזור לא מגיע אליהם. כל 93 האזורים והתוספות שאפשר להגיע אליהם נטענו.
- **75 האזורים שבמשחק** (70 בשרשרת ו-5 אתגרים):
  - בכולם Kratos שיחק 90 שניות.
  - אין קריסה, trap, ‏deadlock או abort, לא ב-GPU ולא ב-both.
  - אין אף שורת `UNIMPLEMENTED`, ואין קובץ שלא נמצא.
  - ב-59 מהם המשחק טען בעצמו אזור שכן תוך 90 השניות. ‏11 תוספות נטענו ליד האזור שלהן (גלי אויבים), כמו במשחק.
- **סרטונים:** שלושה אזורים מתחילים בסרטון: ‏`pi_beach_00` (הפתיחה, 59.9 שניות), ‏`ec_3_eos_chamber` (90 שניות, 2,698 תמונות) ו-`poh_persephone_chamber` (121.7 שניות, 3,647 תמונות). כולם התנגנו עד הסוף, בלי אף כשל בפענוח.
- **PSNR בין ה-renderers:** ‏294 השוואות. החציון 36.6 dB, הטווח 26.4–52.1 dB.
  - הנמוכים, מתחת ל-31 dB, הם מסכי "YOU ARE DEAD" ו-"YOU HAVE FAILED", חוץ מ-`mar_ha_*_enc` (בסעיף הבא). המסך כולו מטושטש ב-render-to-texture חוזר, ולכן הרבה פיקסלים שונים בקצת. אף פיקסל במסכים האלה לא שונה ביותר מ-32.
  - הכי הרבה פיקסלים ששונים ביותר מ-32: ‏3.1%, בתוספת `mar_ha_*_enc` שנטענה לבד. ‏Kratos נופל בה לחלל לבן, וההפרש הוא בשוליים של הזוהר ובתאורה של Kratos.
  - באזורים שבמשחק, לכל היותר 0.19% מהפיקסלים שונים ביותר מ-32 (`ef_01_fields`). אלה נקודות בדשא, שמצויר ב-alpha test, וכתם רך בערפל.
  - **בעין, אין הבדל.** ‏`compare_*.png` שמים את התוכנה, ה-GPU וההפרש פי 4 זה ליד זה.
- **מוות למרות `--god`:** ב-8 אזורים הבריאות הגיעה ל-0. זה קרה באתגרים שנכשלו (`cog_01`, ‏`02`, ‏`04`, ו-`ec_5_parry_challenge`), בנפילות (`foa_path_b`, ‏`tar_caves_06_zeusgate`), ובשני האזורים בלי נקודת spawn (למטה). המשחק הציג "YOU ARE DEAD" ו-"Restart from last checkpoint", והאיקס של התסריט החזיר את Kratos לנקודת הביקורת.
- **נקודת שמירה אמיתית:** ב-`foa_path_a` התסריט הכללי נתקל במזבח שמירה, והמשחק שמר בעצמו:
  - ‏`LISTSAVE` ל-`DATA00` (‏4,932 בתים), עם הכותרת `The Falls of Oceanum` ופרטים "Total Time: 00:00:23 / Red Orbs: 0 Health: 100 Magic: 0 / Difficulty: Hero (Normal)".
  - אחר כך boot עם השמירה ו-Load Game: ‏Kratos חזר ל-`foa_path_a`, לנקודת spawn 4, ליד המזבח.
  - זה סוגר את הסעיף הפתוח של [שלב 9](phase9-saves-replay.md) ("נקודת שמירה אמיתית עדיין לא נבדקה"). אחרי הטעינה `game+0x120` הוא 4: נקודת השמירה היא אחת מנקודות ה-spawn של האזור.

### נקודות spawn (נקודות הביקורת)

ל-41 מהאזורים יש יותר מנקודת spawn אחת, עד 6 (`tar_keeper_01_crypt`). נקודות השמירה הן חלק מהן (למעלה). אחרי הסיור הראשי, ריצה נוספת התחילה מכל אחת מ-84 הנקודות האחרות, 45 שניות ב-GPU (`build/tour/spawns`). ‏**כל 84 הריצות עברו**, בלי trap ובלי `UNIMPLEMENTED`. ב-13 מהן Kratos נפל: למשל `ht04_elevator_mid:1` ו-`ht16_elevator_return_high:1`, נקודות על מעליות שבמשחק חדש לא נמצאות שם.

### הסרטונים: ריצה ארוכה

ב-`ec_3_eos_chamber` וב-`poh_persephone_chamber` הסרטון תופס את רוב 90 השניות. לכן הם רצו שוב ל-200 שניות (`build/tour/long`): אחרי הסרטון Kratos נלחם במערה של Eos, ובאזור של Persephone מופיע QTE. שתי הריצות עברו עד הסוף (266 ו-282 שניות וירטואליות).

### קצב (headless, ‏`--renderer gpu`)

כל אזור רץ לבד, 30 שניות מ-`--warp`, ונמדד הזמן של המחשב לכל פריים (`perf.csv`), מפריים 1200. **כל ריצה נמדדה פעמיים, והמספר הוא של המהירה מביניהן:**
- הריצה הראשונה על כל 73 האזורים הופרעה מעבודה אחרת שרצה אז במחשב.
- בין שתי ריצות של אותו אזור, הפריימים והזמן הווירטואלי זהים. רעש במחשב רק מאט, ולכן הריצה המהירה היא המדידה.
- ‏`pi_beach_01` נתן 110 FPS, כמו ה-116 של [שלב 6](phase6-gpu-renderer.md) בשלב הראשון.
- בריצת ה-GPU של הסיור, עם 5 ריצות במקביל, יצאו מספרים נמוכים בהרבה. הם לא בטבלה.

| | FPS ‏headless | חציון לפריים | p99 | פריימים מעל 16.7 ms |
|---|---|---|---|---|
| ‏73 האזורים שבמשחק | מינימום 66.6, חציון 112.6 | 8.9 ms (חציון) | 13.7 ms (חציון) | ב-21 אזורים p99 מעל 16.7 ms |
| ‏`foa_path_c` | 69.5 | 13.6 ms | 23.6 ms | **‏40%** |
| ‏`ec_5_parry_challenge` | 80.6 | 10.9 ms | 22.4 ms | **‏23%** |
| ‏`tar_caves_05_titanhead` | 66.6 | 14.9 ms | 22.9 ms | **‏14%** |
| ‏`mar_02_village` | 78.6 | 12.5 ms | 19.1 ms | 9% |
| ‏`pi_connector` | 71.4 | 13.9 ms | 21.2 ms | 9% |
| ‏`pi_city_streets_02`, ‏`ht01_courtyard_low`, ‏`ht17_balcony` | 85–112 | 8.6–11.1 ms | 18.4–21.5 ms | 5–7% |

**המשמעות בחלון:**
- כל האזורים עומדים ב-60 FPS בממוצע.
- בחלון הזמן הווירטואלי צמוד לשעון ([שלב 5](phase5-window-input.md)), ולכן פריים שלוקח יותר מ-16.7 ms מאט את המשחק לרגע.
- ב-`foa_path_c`, ב-`ec_5_parry_challenge` וב-`tar_caves_05_titanhead` זה יקרה הרבה: 14–40% מהפריימים.
- הזמן הווירטואלי עצמו מתקדם ב-59.98 FPS בכל אזור. היוצאים מן הכלל הם שלושת האזורים עם סרטון, ו-`foa_path_a` (‏59.34) בגלל השמירה.

![73 האזורים שבמשחק לפי הסדר, ריצת ה-GPU: מכל אזור הפריים הבהיר מבין פריימי האמצע (בלי שני האזורים שאין בהם נקודת spawn)](../media/screenshots/2026-10-16_40_coo_level_tour_overview.png)

### האזורים

"איך מגיעים" הוא המקום בשרשרת שעולה מהגרף, או השכן שטוען את האזור. ‏"spawn" הוא מספר נקודות ה-spawn באזור. ‏PSNR בפריימים 5400, ‏6000 ו-6600. ‏FPS ‏GPU הוא קצב ה-headless כשהאזור רץ לבד, המהירה מבין שתי מדידות (ראו "קצב").

| שלב | אזור | איך מגיעים | spawn | תוצאה | נטענו לידו | PSNR soft/GPU (dB) | FPS ‏GPU (p99 ms) | הערות |
|---|---|---|---|---|---|---|---|---|
| `pi_beach_00` | Attica | #1 | 1 | ✓ | `pi_beach_01` | 35.0 / 34.9 / 35.0 | 361 (5.3) | סרטון (46 FPS במשחק) |
| `pi_beach_01` | Attica | #2 | 1 | ✓ | `pi_beach_02` | 36.4 / 36.0 / 36.4 | 110 (13.1) |  |
| `pi_beach_02` | Attica | #3 | 3 | ✓ | `pi_beach_01` | 33.2 / 32.9 / 32.7 | 99 (18.3) | נקודות 1–2: ✓ |
| `pi_king_01` | Attica | #4 | 1 | ✓ | `pi_beach_02` | 38.6 / 35.8 / 37.8 | 129 (12.2) |  |
| `pi_city_wall_00` | Attica | #5 | 1 | ✓ | `pi_city_wall_01` | 37.0 / 35.8 / 35.8 | 118 (14.0) |  |
| `pi_city_wall_01` | Attica | #6 | 1 | ✓ | `pi_city_wall_00` | 35.5 / 32.8 / 31.9 | 113 (11.8) |  |
| `pi_king_fight` | Attica | #7 | 4 | ✓ | `pi_city_wall_02` | 37.4 / 35.6 / 36.1 | 93 (17.4) | נקודות 1–3: ✓ |
| `pi_city_wall_02` | Attica | #8 | 1 | ✓ | `pi_city_streets_01` | 35.1 / 33.7 / 34.5 | 105 (13.0) |  |
| `pi_city_streets_01` | Attica | #9 | 1 | ✓ | `pi_city_wall_02`, `pi_city_streets_02` | 35.8 / 35.9 / 36.0 | 99 (18.2) |  |
| `pi_city_streets_02` | Attica | #10 | 2 | ✓ | `pi_city_streets_01` | 35.9 / 35.7 / 35.7 | 85 (18.4) | נקודה 1: ✓ |
| `pi_city_streets_03` | Attica | #11 | 1 | ✓ | `pi_city_streets_02`, `pi_bazaar_01` | 36.7 / 36.9 / 36.2 | 96 (14.2) |  |
| `pi_bazaar_01` | Attica | #12 | 1 | ✓ | `pi_bath_house_01` | 37.0 / 36.3 / 36.7 | 98 (22.7) |  |
| `pi_bath_house_01` | Attica | #13 | 1 | ✓ | `pi_bazaar_01` | 36.8 / 36.3 / 36.6 | 79 (18.6) |  |
| `pi_connector` | Attica | #14 | 2 | ✓ | `pi_basilisk`, `pi_bath_house_01` | 37.9 / 36.1 / 36.3 | 71 (21.2) | נקודה 1: ✓ |
| `pi_basilisk` | Attica | #15 | 1 | ✓ | `mar_01_rock_climb` | 35.4 / 32.3 / 32.3 | 126 (12.6) |  |
| `mar_01_rock_climb` | Marathon | #16 | 3 | ✓ | – | 34.9 / 34.2 / 35.4 | 147 (12.3) | נקודות 1–2: ✓ |
| `mar_02_village` | Marathon | #17 | 2 | ✓ | `mar_01_rock_climb` | 37.2 / 37.7 / 38.1 | 79 (19.1) | נקודה 1: ✓ |
| `mar_03_gate_puzzle` | Marathon | #18 | 5 | ✓ | – | 37.8 / 37.2 / 36.9 | 122 (17.4) | נקודות 1–4: ✓ |
| `mar_04_helios_approach` | Marathon | #19 | 4 | ✓ | `mar_03_gate_puzzle`, `mar_ha_arena_enc` | 35.7 / 36.9 / 36.6 | 89 (17.1) | נקודות 1–3: ✓ |
| `ht01_courtyard_low` | Temple of Helios | #20 | 3 | ✓ | `ht02_courtyard_high` | 36.3 / 36.8 / 35.9 | 86 (21.5) | נקודות 1–2: ✓ |
| `ht02_courtyard_high` | Temple of Helios | #21 | 3 | ✓ | `ht01_courtyard_low` | 37.3 / 37.7 / 36.6 | 91 (15.9) | נקודות 1–2: ✓ |
| `ht03_foyer_hallway` | Temple of Helios | #22 | 4 | ✓ | `ht02_courtyard_high`, `ht03_foyer_hallway_enemies` | 36.8 / 35.8 / 36.4 | 160 (12.9) | נקודות 1–3: ✓ |
| `ht04_elevator_mid` | Temple of Helios | #23 | 2 | ✓ | `ht03_foyer_hallway` | 37.3 / 37.1 / 36.8 | 116 (13.7) | נקודה 1: ✓ |
| `ht06_astrolabe_low` | Temple of Helios | #24 | 1 | ✓ | `ht07_throne_room` | 38.4 / 37.3 / 37.5 | 102 (18.0) |  |
| `ht07_throne_room` | Temple of Helios | #25 | 2 | ✓ | `ht08_fire_pit` | 35.7 / 36.6 / 35.6 | 93 (15.9) | נקודה 1: ✓ |
| `ht08_fire_pit` | Temple of Helios | #26 | 1 | ✓ | `ht07_throne_room` | 36.6 / 35.7 / 35.2 | 206 (8.0) |  |
| `ht11_courtyard_return` | Temple of Helios | #27 | 2 | ✓ | `ht08_fire_pit` | 34.7 / 35.8 / 35.3 | 112 (13.3) | נקודה 1: ✓ |
| `ht12_stair_puzzle_1` | Temple of Helios | #28 | 1 | ✓ | `ht13_astrolabe_high` | 37.7 / 37.4 / 37.6 | 87 (15.6) |  |
| `ht13_astrolabe_high` | Temple of Helios | #29 | 2 | ✓ | `ec_1_entrance` | 36.6 / 35.7 / 37.0 | 137 (12.4) | נקודה 1: ✓ |
| `ec_1_entrance` | ec (Eos) | #30 | 3 | ✓ | `ec_0_underwater_ruins` | 35.4 / 34.9 / 34.1 | 99 (19.1) | נקודות 1–2: ✓ |
| `ec_0_underwater_ruins` | ec (Eos) | #31 | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 51.5 / 44.8 / 44.8 | – |  |
| `ec_2_combat_arena` | ec (Eos) | #32 | 2 | ✓ | `ec_2_combat_arena_enc` | 37.6 / 37.4 / 37.5 | 98 (14.9) | נקודה 1: ✓ |
| `ec_3_eos_chamber` | ec (Eos) | #33 | 2 | ✓ | `ec_5_parry_challenge` | 35.2 / 35.2 / 35.2 | 402 (4.2) | נקודה 1: ✓; סרטון (41 FPS במשחק) |
| `ec_5_parry_challenge` | ec (Eos) | #34 | 1 | ✓ | – | 35.5 / 36.4 / 35.3 | 81 (22.4) | Kratos מת 1× |
| `ec_6_exit` | ec (Eos) | #35 | 2 | ✓ | `ht14_astrolabe_fire` | 33.8 / 34.1 / 34.0 | 109 (15.1) | נקודה 1: ✓ |
| `ht14_astrolabe_fire` | Temple of Helios | #36 | 1 | ✓ | – | 36.4 / 36.5 / 36.3 | 85 (17.6) |  |
| `ht15_elevator_return_low` | Temple of Helios | #37 | 1 | ✓ | `ht16_elevator_return_high`, `ht14_astrolabe_fire` | 36.7 / 38.5 / 37.7 | 139 (10.3) |  |
| `ht16_elevator_return_high` | Temple of Helios | #38 | 2 | ✓ | `ht17_balcony` | 37.9 / 36.4 / 37.2 | 109 (13.1) | נקודה 1: ✓ |
| `ht17_balcony` | Temple of Helios | #39 | 3 | ✓ | `ht17_balcony_enemies` | 36.6 / 36.0 / 35.3 | 112 (18.6) | נקודות 1–2: ✓ |
| `ht18_astrolabe_final` | Temple of Helios | #40 | 1 | ✓ | `ht19_stair_puzzle_2` | 32.9 / 35.5 / 35.0 | 142 (15.2) |  |
| `ht19_stair_puzzle_2` | Temple of Helios | #41 | 2 | ✓ | `mar_05_steed_return`, `ht18_astrolabe_final` | 36.3 / 37.0 / 36.6 | 169 (11.9) | נקודה 1: ✓ |
| `mar_05_steed_return` | Marathon | #42 | 2 | ✓ | `ht19_stair_puzzle_2` | 38.7 / 38.4 / 39.1 | 142 (9.6) | נקודה 1: ✓ |
| `foa_path_a` | foa | #43 | 5 | ✓ | `foa_path_a_enemies` | 34.9 / 33.7 / 34.4 | 130 (12.2) | נקודות 1–4: ✓ |
| `foa_path_b` | foa | #44 | 5 | ✓ | `foa_path_a`, `foa_path_b_enemies` | 34.7 / 36.6 / 36.7 | 164 (14.6) | נקודות 1–4: ✓; Kratos מת 2× |
| `foa_path_c` | foa | #45 | 3 | ✓ | `foa_path_connector` | 37.9 / 37.5 / 37.5 | 70 (23.6) | נקודות 1–2: ✓ |
| `foa_path_connector` | foa | #46 | 1 | ✓ | `foa_path_c`, `foa_deathknight_arena` | 36.3 / 36.6 / 36.5 | 190 (8.8) |  |
| `foa_deathknight_arena` | foa | #47 | 5 | ✓ | `foa_path_connector` | 33.3 / 34.2 / 35.8 | 115 (16.0) | נקודות 1–4: ✓ |
| `charon_galley_1` | Charon | #48 | 1 | ✓ | `tar_caves_01_welcomeparty` | 36.6 / 34.6 / 34.7 | 107 (14.2) |  |
| `tar_caves_01_welcomeparty` | tar (Tartarus) | #49 | 4 | ✓ | – | 36.5 / 35.7 / 36.2 | 96 (16.1) | נקודות 1–3: ✓ |
| `tar_caves_01_5_zeusgate` | tar (Tartarus) | #50 | 4 | ✓ | `tar_zg_enc_01` | 35.7 / 35.4 / 35.5 | 100 (15.7) | נקודות 1–3: ✓ |
| `tar_caves_02_titanpit` | tar (Tartarus) | #51 | 4 | ✓ | `tar_tp_enc_01` | 38.8 / 39.1 / 39.0 | 98 (13.6) | נקודות 1–3: ✓ |
| `tar_caves_03_arenacavern` | tar (Tartarus) | #52 | 1 | ✓ | `tar_caves_04_maintitan` | 38.1 / 36.9 / 36.5 | 119 (12.8) |  |
| `tar_caves_04_maintitan` | tar (Tartarus) | #53 | 3 | ✓ | `tar_mt_enc_01` | 36.2 / 36.4 / 35.9 | 151 (12.3) | נקודות 1–2: ✓ |
| `tar_caves_05_titanhead` | tar (Tartarus) | #54 | 3 | ✓ | `tar_th_enc_01` | 37.5 / 38.2 / 36.9 | 67 (22.9) | נקודות 1–2: ✓ |
| `tar_caves_06_zeusgate` | tar (Tartarus) | #55 | 3 | ✓ | – | 37.1 / 37.2 / 37.9 | 126 (12.6) | נקודות 1–2: ✓; Kratos מת 3× |
| `tar_keeper_01_crypt` | tar (Tartarus) | #56 | 6 | ✓ | `tar_kp_enc_01`, `tar_caves_01_5_zeusgate` | 36.6 / 36.8 / 36.5 | 163 (10.3) | נקודות 1–5: ✓ |
| `tar_keeper_02_zeustemple` | tar (Tartarus) | #57 | 2 | ✓ | `tar_keeper_01_crypt` | 35.7 / 35.3 / 34.3 | 118 (12.2) | נקודה 1: ✓ |
| `charon_galley_2` | Charon | #58 | 1 | ✓ | `poh_01_entrance` | 36.6 / 34.6 / 34.7 | 104 (14.6) |  |
| `poh_01_entrance` | poh (Hades) | #59 | 2 | ✓ | – | 35.4 / 35.2 / 35.3 | 74 (19.6) | נקודה 1: ✓ |
| `poh_02_hades_vista` | poh (Hades) | #60 | 2 | ✓ | `poh_02b_hades_vista` | 39.1 / 38.8 / 38.7 | 178 (10.1) | נקודה 1: ✓ |
| `poh_02b_hades_vista` | poh (Hades) | #61 | 2 | ✓ | `poh_03_elysium_vista` | 37.3 / 37.4 / 37.1 | 122 (11.8) | נקודה 1: ✓ |
| `poh_03_elysium_vista` | poh (Hades) | #62 | 1 | ✓ | `poh_02b_hades_vista`, `poh_03b_elysium_vista` | 36.9 / 36.6 / 35.7 | 150 (11.2) |  |
| `poh_03b_elysium_vista` | poh (Hades) | #63 | 2 | ✓ | `poh_03_elysium_vista`, `poh_04_elevator_low` | 32.3 / 36.3 / 35.6 | 135 (10.3) | נקודה 1: ✓ |
| `poh_04_elevator_low` | poh (Hades) | #64 | 1 | ✓ | `poh_05_elevator_high` | 36.5 / 36.3 / 37.1 | 137 (12.2) |  |
| `poh_05_elevator_high` | poh (Hades) | #65 | 5 | ✓ | `poh_04_elevator_low` | 36.9 / 36.9 / 37.1 | 214 (6.6) | נקודות 1–4: ✓ |
| `poh_04b_elevator_low` | poh (Hades) | #66 | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 29.1 / 46.6 / 46.6 | – |  |
| `poh_persephone_chamber` | poh (Hades) | #67 | 1 | ✓ | `poh_05_elevator_high`, `ef_01_fields` | 36.1 / 36.1 / 36.1 | 314 (5.8) | סרטון (36 FPS במשחק) |
| `ef_01_fields` | ef (Elysium) | #68 | 4 | ✓ | – | 33.9 / 33.8 / 33.6 | 144 (10.9) | נקודות 1–3: ✓ |
| `ef_03_crash_landing` | ef (Elysium) | #69 | 1 | ✓ | `ef_03_crash_landing_2` | 35.5 / 37.4 / 36.8 | 93 (16.5) |  |
| `ef_03_crash_landing_2` | ef (Elysium) | #70 | 1 | ✓ | – | 36.0 / 36.1 / 35.9 | 78 (19.4) |  |
| `cog_01` | Challenge of the Gods | תפריט האתגרים | 1 | ✓ | – | 38.4 / 37.3 / 37.1 | 144 (11.4) | Kratos מת 1× |
| `cog_02` | Challenge of the Gods | תפריט האתגרים | 1 | ✓ | – | 30.3 / 37.6 / 35.7 | 234 (8.9) | Kratos מת 4× |
| `cog_03` | Challenge of the Gods | תפריט האתגרים | 1 | ✓ | – | 36.4 / 36.7 / 37.3 | 142 (10.8) |  |
| `cog_04` | Challenge of the Gods | תפריט האתגרים | 1 | ✓ | – | 32.4 / 37.0 / 36.5 | 100 (17.1) | Kratos מת 1× |
| `cog_05` | Challenge of the Gods | תפריט האתגרים | 1 | ✓ | – | 36.6 / 35.5 / 37.5 | 139 (12.4) |  |
| `ec_4_hallway` | ec (Eos) | לא נגיש | – | trap בטעינה (`break` ב-`free`) | – | – | – |  |
| `ht05_elevator_low` | Temple of Helios | לא נגיש | – | trap בטעינה (`break` ב-`free`) | – | – | – |  |
| `ht09_light_room` | Temple of Helios | לא נגיש | – | trap בטעינה (`break` ב-`free`) | – | – | – |  |
| `pi_bath_house_02` | Attica | לא נגיש | – | trap בטעינה (`break` ב-`free`) | – | – | – |  |
| `pi_bazaar_02` | Attica | לא נגיש | – | trap בטעינה (`break` ב-`free`) | – | – | – |  |
| `pi_temple_01` | Attica | לא נגיש | – | trap בטעינה (`break` ב-`free`) | – | – | – |  |
| `poh_premod` | poh (Hades) | לא נגיש | – | trap בטעינה (`break` ב-`free`) | – | – | – |  |
| `tar_caves_09_titanhead` | tar (Tartarus) | לא נגיש | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 51.1 / 45.9 / 45.9 | – |  |

### התוספות

התוספות נטענו לבד, בלי האזור שלהן. אין בהן רצפה ואין נקודת spawn, ולכן Kratos נופל עד המוות בלולאה. הבדיקה כאן היא רק שהטעינה עוברת. את התוכן שלהן (האויבים) רואים כשהאזור שלהן טוען אותן, כמו 11 מהן בסיור.

| שלב | אזור | איך מגיעים | spawn | תוצאה | נטענו לידו | PSNR soft/GPU (dB) | FPS ‏GPU (p99 ms) | הערות |
|---|---|---|---|---|---|---|---|---|
| `ec_2_combat_arena_enc` | ec (Eos) | דרך `ec_2_combat_arena` | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 29.7 / 46.6 / 46.6 | – | נטען ליד `ec_2_combat_arena` |
| `ec_7_exit_enc` | ec (Eos) | לא נגיש | – | trap בטעינה (`break` ב-`free`) | – | – | – |  |
| `ef_04_enc_01` | ef (Elysium) | דרך `ef_01_fields` | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 46.6 / 29.0 / 31.5 | – |  |
| `ef_05_enc_02` | ef (Elysium) | לא נגיש | – | trap בטעינה (`break` ב-`free`) | – | – | – |  |
| `ef_06_enc_03` | ef (Elysium) | לא נגיש | – | trap בטעינה (`break` ב-`free`) | – | – | – |  |
| `ef_07_enc_04` | ef (Elysium) | לא נגיש | – | trap בטעינה (`break` ב-`free`) | – | – | – |  |
| `ef_08_enc_05` | ef (Elysium) | לא נגיש | – | trap בטעינה (`break` ב-`free`) | – | – | – |  |
| `foa_path_a_enemies` | foa | דרך `foa_path_a` | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 40.2 / 39.1 / 40.1 | – | נטען ליד `foa_path_a` |
| `foa_path_b_enemies` | foa | דרך `foa_path_b` | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 40.2 / 39.8 / 40.1 | – | נטען ליד `foa_path_b` |
| `foa_path_b_pushbox` | foa | דרך `foa_path_b` | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 40.2 / 40.0 / 40.1 | – |  |
| `ht03_foyer_hallway_enemies` | Temple of Helios | דרך `ht03_foyer_hallway` | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 46.6 / 27.5 / 26.4 | – | נטען ליד `ht03_foyer_hallway` |
| `ht11_courtyard_enemies` | Temple of Helios | לא נגיש | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 51.1 / 45.9 / 45.9 | – |  |
| `ht17_balcony_enemies` | Temple of Helios | דרך `ht17_balcony` | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 35.7 / 46.6 / 46.6 | – | נטען ליד `ht17_balcony` |
| `mar_gp_beast_enc` | Marathon | לא נגיש | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 51.1 / 45.9 / 45.9 | – |  |
| `mar_gp_fog_enc` | Marathon | לא נגיש | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 51.1 / 45.9 / 45.9 | – |  |
| `mar_gp_gate_enc` | Marathon | לא נגיש | – | trap בטעינה (`break` ב-`free`) | – | – | – |  |
| `mar_ha_arena_enc` | Marathon | דרך `mar_04_helios_approach` | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 46.6 / 42.2 / 28.9 | – | נטען ליד `mar_04_helios_approach` |
| `mar_ha_yoke_enc` | Marathon | דרך `mar_05_steed_return` | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 46.6 / 43.9 / 28.9 | – |  |
| `poh_02_hades_vista_enemies` | poh (Hades) | לא נגיש | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 52.1 / 45.9 / 45.9 | – |  |
| `tar_kp_enc_01` | tar (Tartarus) | דרך `tar_keeper_01_crypt` | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 36.7 / 46.6 / 46.6 | – | נטען ליד `tar_keeper_01_crypt` |
| `tar_kp_enc_02` | tar (Tartarus) | לא נגיש | – | trap בטעינה (`break` ב-`free`) | – | – | – |  |
| `tar_kp_enc_03` | tar (Tartarus) | דרך `tar_keeper_01_crypt` | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 36.7 / 46.6 / 46.6 | – |  |
| `tar_kp_enc_04` | tar (Tartarus) | לא נגיש | – | trap בטעינה (`break` ב-`free`) | – | – | – |  |
| `tar_kp_enc_05` | tar (Tartarus) | לא נגיש | – | trap בטעינה (`break` ב-`free`) | – | – | – |  |
| `tar_mt_enc_01` | tar (Tartarus) | דרך `tar_caves_04_maintitan` | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 35.6 / 46.6 / 46.6 | – | נטען ליד `tar_caves_04_maintitan` |
| `tar_th_enc_01` | tar (Tartarus) | דרך `tar_caves_05_titanhead` | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 36.7 / 46.6 / 46.6 | – | נטען ליד `tar_caves_05_titanhead` |
| `tar_tp_enc_01` | tar (Tartarus) | דרך `tar_caves_02_titanpit` | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 45.9 / 39.5 / 41.6 | – | נטען ליד `tar_caves_02_titanpit` |
| `tar_tp_enc_02` | tar (Tartarus) | דרך `tar_caves_02_titanpit` | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 44.4 / 45.9 / 45.9 | – |  |
| `tar_wp_enc_01` | tar (Tartarus) | דרך `tar_caves_01_welcomeparty` | 1 | ✓ | – | 45.9 / 39.3 / 42.3 | – | Kratos מת 8× |
| `tar_wp_enc_02` | tar (Tartarus) | דרך `tar_caves_01_welcomeparty` | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 45.9 / 39.3 / 42.6 | – |  |
| `tar_zg_enc_01` | tar (Tartarus) | דרך `tar_caves_01_5_zeusgate` | 0 | נטען; אין נקודת spawn, Kratos נופל | – | 41.9 / 45.9 / 45.9 | – | נטען ליד `tar_caves_01_5_zeusgate` |

### מה הסיור לא בודק

- **מצב המשחק אחרי `--warp` הוא של משחק חדש.** אין ל-Kratos כוחות או נשקים מפרקים קודמים, ודגלי הסיפור לא דולקים. חידות, בוסים ו-QTE עלולים להתנהג אחרת ממשחק אמיתי, והתסריט הכללי לא פותר אותם.
- **מכל אזור רק 90 שניות מנקודת spawn 0,** ועוד 45 שניות מכל נקודת spawn אחרת. שאר האזור נבדק רק כמה שהריצה האקראית הגיעה אליו.
- **‏`ec_0_underwater_ruins` ו-`poh_04b_elevator_low`** הם חלק מהמשחק, אבל אין בהם נקודת spawn. נכנסים אליהם מהשכן תוך כדי משחק. ‏`ec_0` נטען בסיור ליד `ec_1_entrance` ‏(מצב 11). ב-`--warp` אליהם Kratos נופל.
- **מוזיקה:** אחרי `--warp`, ברוב האזורים לא נשמעת מוזיקה בהתחלה. בריצות של `tools/hle_calls.py`, ערוץ המוזיקה (5) קיבל 64 בלוקים ברוב האזורים, וכ-980 ב-`cog_01`, ‏`charon_galley_1` ו-`ef_03_crash_landing`. המוזיקה בשלבים מופעלת מאירועים בסקריפט, כמו `PERCOM2L.AT3` בשלב הראשון (85 שניות אחרי New Game). זה לא נבדק במשחק אמיתי.
- **הקצב הוא של headless,** לא של החלון.

## רשימת באגים לפי עדיפות

לא נמצא באג שעוצר משחק רגיל. הרשימה מסודרת לפי מה שצריך לבדוק קודם.

| # | עדיפות | מה | מה ידוע | הצעד הבא |
|---|---|---|---|---|
| 1 | בינונית: לוודא שאינו באג של ה-port | **trap בטעינה של 16 אזורים לא נגישים.** ‏`break` (קוד 0xD) ב-`0x088096AC`, מתוך `free` של dlmalloc ‏(`0x0880BE78`) שנקרא מ-`0x0880E988`, שמשחרר מהמאגר הנוכחי (`0x08B2D840`). כנראה הבלוק לא שייך למאגר הזה. זה קורה במצב טעינה 4, האתחול | ה-trap קורה רק באזורים שאף אזור לא מזכיר. כל 93 הנגישים נטענים. גם בדרך של ה-streaming זה קורה: `--warp ht04_elevator_mid --load-level ht05_elevator_low@1300`. השכן האמיתי (`ht06_astrolabe_low`) נטען באותה דרך בלי בעיה. כנראה אזורים שנשארו מהפיתוח, עם נתונים שהקוד הסופי לא מצפה להם | לטעון את אחד מהם ב-PPSSPP (`LoadLevel` ב-`0x088347B8` מה-debugger). אם PPSSPP נעצר באותה בדיקה, לסגור. אם לא, ‏`--dump-ram stop:...` ומעבר על המחסנית כדי למצוא את מי ששחרר |
| 2 | בינונית | **פריימים ארוכים בשלושה אזורים** | ‏`foa_path_c` ‏(40% מהפריימים מעל 16.7 ms, ‏p99 של 23.6 ms), ‏`ec_5_parry_challenge` ‏(23%), ‏`tar_caves_05_titanhead` ‏(14%). ב-headless כולם מעל 60 FPS בממוצע, אבל בחלון המשחק יאט לרגעים | ‏`--profile` על האזורים האלה, כדי לראות אם הזמן הולך לקוד המשחק, ל-transform או ל-GPU. ואז לבדוק בחלון עם `--perf-csv` |
| 3 | נמוכה | **ה-HUD לא מופיע אחרי Load Game** בנקודת השמירה של `foa_path_a`: ‏1,300 פריימים בלי קלט, בלי HUD. אחרי `--warp` ה-HUD נשאר גם כש-Kratos עומד | נצפה פעם אחת, בלי קלט אחרי הטעינה | לטעון בחלון, לזוז, ולהשוות ל-PPSSPP |
| 4 | נמוכה | **הפרשים קטנים בין ה-renderers:** נקודות בדשא (alpha test, ‏`ef_01_fields`), שולי זוהר מלא-מסך, והתאורה של Kratos בחלל של `mar_ha_*_enc` | PSNR 26–52 dB. ‏≤0.19% מהפיקסלים מעל 32 באזורים שבמשחק. לא נראה בעין | להשוות `compare_*.png` של `ef_01_fields` למצב ה-alpha test וה-rasterization של ה-GPU |
| 5 | נמוכה, לא באג | **‏`ec_0_underwater_ruins` ו-`poh_04b_elevator_low` לא שוחקו:** אין בהם נקודת spawn | ‏`ec_0` נטען ליד `ec_1_entrance` | להגיע אליהם מהשכן: להוסיף ל-`--warp` הצבה של Kratos בנקודה של אזור אחר |
| 6 | לבדיקה | **מוזיקה בשלבים אחרי `--warp`** | ברוב האזורים לא נשמעת. כנראה כי היא מופעלת מסקריפט | לשמוע במשחק אמיתי, אחרי שעוברים ממש לאזור הבא |

**מה כבר תוקן תוך כדי:** כלום. לא נמצאה קריסה עם סיבה ברורה ב-port.

## איך מריצים

```
cmake --build build/cmake_tour --config Release --target gow_rt
.venv\Scripts\python -I tools\level_tour.py --out build\tour\tour1 --addons --modes gpu --jobs 5
.venv\Scripts\python -I tools\level_tour.py --out build\tour\tour1 --addons --modes both --both-from 5300 --jobs 6
.venv\Scripts\python -I tools\level_tour.py --out build\tour\fps --levels pi_beach_01,mar_02_village --modes gpu --jobs 1 --seconds 45
.venv\Scripts\python -I tools\level_tour.py --out build\tour\spawns --levels ef_01_fields:1,ef_01_fields:2 --modes gpu
.venv\Scripts\python -I tools\hle_audit.py --calls build\tour\hle_calls.json --md build\tour\hle_audit.md
.venv\Scripts\python -I tools\hle_calls.py --out build\tour\hle_calls.json --baseline-input build\rec_v9\input.txt
```

ריצה בודדת:

```
gow_rt_UCUS98653_full.exe --image build/gen/UCUS98653_full/image.bin --boot --disc "God of War - Chains of Olympus (USA).chd" --ms0 build/tour/ms0_empty --warp tar_caves_04_maintitan --god --window --renderer gpu
```

`--warp` עובד גם בחלון: בפריים 1000 התפריט נסגר והמשחק נטען באזור, באותו זמן וירטואלי כמו ב-headless (16.936 שניות, נבדק עם `pi_city_streets_01`). ‏`--ms0` חייב להיות ריק, כי פרופיל שמור משנה את ה-boot והתפריט מגיע בפריים אחר (אז צריך `@FRAME`).
