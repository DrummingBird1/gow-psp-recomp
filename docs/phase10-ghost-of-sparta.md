# שלב 10: Ghost of Sparta

**תאריך:** 2026-10-16 · **branch:** `phase2/threads-and-fs` · **קוד:** פענוח PGD ו-`scePspNpDrm_user` ב-`runtime/rt_io.c`, ‏`sceUtilityLoadModule` ב-`runtime/rt_hle.c`, קולות PCM ב-`runtime/rt_sas.c`, ‏`recomp/emitc.py` (תיקון במהדר), ‏`recomp/runtime_funcs.json`, ‏`CMakeLists.txt` (הורדת libkirk), ‏`runtime/rt_main.c`, ‏`tools/play_gos.cmd` (חדש), ‏`tools/replay.cmd`, ‏`tools/capture_window.ps1`

## השורה התחתונה

- **Ghost of Sparta ‏(UCUS98737) רץ כקוד native**, כמו Chains of Olympus, ועובר את כל הדרך עד השלב הראשון:
  - האזהרה, הלוגואים, התפריט הראשי ובחירת רמת הקושי.
  - סרטון הפתיחה: ‏105 שניות, עם הכתוביות של המשחק וסאונד.
  - השלב הראשון, Atlantis, על הספינה בסערה: קרב עם Blades of Athena, מונה hits, ה-tutorials, ה-QTE של ה-grab, ו-cutscenes במנוע.
- **בחלון, עם `--renderer gpu`:**
  - 60 FPS בשלב, 58.5 FPS בתפריט, והסרטון ב-30.0 FPS, שזה הקצב שלו.
  - סאונד: מוזיקת התפריט, הסרטון, מוזיקת השלב, ואפקטי ה-SAS של הקרב.
  - קלט: המקלדת עובדת דרך אותו קוד של CoO. הדרך לשלב שוחקה בחלון עם הודעות מקלדת אמיתיות (ראו "אימות").
- **מה חסם את GoS, ואיך זה נפתר:**
  1. **PGD:** בדיסק יש קובץ מוצפן אחד, ‏`ATT_000_STARTSCREEN.BIN`, והמשחק בודק אותו בלולאה עד שהפענוח מצליח. הפענוח ממומש עכשיו לפי `sceIo` של PPSSPP, עם המפתח שהמשחק עצמו נותן.
  2. **באג במהדר מחדש:** פונקציה אחת ב-GoS התחילה בכתובת הלא נכונה, והמשחק נתקע בלולאה אינסופית בשנייה 24.4. ב-CoO אין אף פונקציה כזו.
  3. **ה-coroutines:** ה-context switch של הסקריפטים נמצא ב-GoS בכתובת אחרת, ונוסף ל-`runtime_funcs.json`.
- **ה-HLE מלא:** כל 206 הפונקציות ש-GoS מייבא ממומשות. בריצות לא נקראה אף פונקציה לא ממומשת.
  - ‏`sceUtilityLoadModule` ממומש לפי PPSSPP.
  - ‏`scePspNpDrm_user` ממומש לפי PPSSPP. המשחק בודק אם יש DLC ב-`ms0`.
  - ‏`__sceSasSetVoicePCM` נוסף. GoS מייבא אותו, אבל עד השלב הראשון הוא לא נקרא.
- **הסאונד והסרטונים זהים לייחוס:**
  - כל 34 הסרטונים שבדיסק (60,637 תמונות) זהים בית-לבית לפענוח של FFmpeg.
  - המוזיקה מתאימה לפענוח של FFmpeg ב-75–80 dB, בלי סחיפה.
- **CoO לא השתנה:** ‏34/34 פריימים זהים ל-`build/rec_v9` ב-renderer התוכנה, ה-WAV זהה בית-לבית, ואותם זמן וירטואלי ומספר החלפות thread. גם ה-C שהמהדר מייצר ל-CoO זהה בית-לבית.

![רצף ה-boot של GoS, מה-framebuffer של ה-PSP](../media/screenshots/2026-10-16_31_gos_boot_sequence.png)

![התפריט הראשי של GoS בחלון](../media/screenshots/2026-10-16_32_gos_window_main_menu.png)

![קרב ב-Atlantis בחלון](../media/screenshots/2026-10-16_33_gos_window_atlantis_combat.png)

| | Chains of Olympus | Ghost of Sparta |
|---|---|---|
| פונקציות (כולן מתורגמות) | 15,935 | 20,328 |
| image | ‏`08804000+36CA7C` | ‏`08804000+45B26C` |
| פונקציות שמיובאות | 199 | 206 |
| טעינת מודולים | ‏`sceKernelLoadModule` ל-6 PRX מהדיסק | ‏`sceUtilityLoadModule`: ‏`0x300`, ‏`0x302`, ‏`0x301`, ‏`0x303`, ‏`0x500` |
| קבצים מוצפנים | אין | ‏PGD אחד, 176 בתים |
| ה-context switch של ה-coroutines | ‏`0x08A157B8` (slots ‏`0x08B36148`/`0x08B36150`) | ‏`0x08AD775C` (slots ‏`0x08C15D88`/`0x08C15D90`) |
| מוזיקת התפריט | ‏`MAINMENU.AT3` | ‏`MENULOOP.AT3` |
| סרטון הפתיחה | ‏`dx_persinv.pmf`, ‏60 שניות | ‏`R6_INTRO.PMF`, ‏105 שניות |
| השלב הראשון | The Shores of Attica | Atlantis (`ATT_010_STERN`, ‏`ATT_020_BOW`) |

## מצב ההתחלה

הקוד שנוצר ל-GoS ‏(`build/gen/UCUS98737_full`) היה מ-16 באוקטובר ב-01:35, מלפני השינויים במהדר של השלבים שאחרי שלב 3, ול-GoS לא היה context switch ב-`runtime_funcs.json`. הלוגים הישנים (`build/boot*_UCUS98737*.log`) נעצרו בפונקציית ה-VFPU הראשונה, או בלולאה שפותחת שוב ושוב את `ATT_000_STARTSCREEN.BIN`.

הקוד של GoS נוצר מחדש עם המהדר הנוכחי, ונבנה עם ה-runtime הנוכחי. ה-runtime לא תלוי במשחק: כתובות ספציפיות למשחק יש רק ב-`recomp/runtime_funcs.json`.

## 1. ה-coroutines

GoS מריץ סקריפטים ב-coroutines שמעתיקים מחסנית, בדיוק כמו CoO (ראו [שלב 5](phase5-window-input.md#coroutines-על-fibers-rt_coroc)).

- **ה-context switch** (`0x08AD775C`) זהה לזה של CoO ‏(`0x08A157B8`) בכל 58 הפקודות, חוץ מה-immediates. הוא נמצא בהשוואה כזו בין כל הפונקציות של שני המשחקים.
- **ה-slot של המחסנית** מתוך ה-`lui`/`lw` של ה-switch: ‏`0x08C15D88`.
- **ה-slot של האובייקט הרץ:** ה-resume ‏(`0x08AD7844`) כותב אותו ל-`0x08C15D90`.

```json
"UCUS98737": { "0x08AD775C": "rt_co_switch(c, 0x08C15D88u, 0x08C15D90u)" }
```

בשלב הראשון רץ coroutine אחד, עם כ-7,500 החלפות עד פריים 9,000.

## 2. PGD: בדיקת הדיסק

### מה המשחק עושה

בשנייה 0.225 ה-boot קורא לפונקציה `0x08A76024`, וזו בדיקת מקוריות של הדיסק:

1. פותחת את `disc0:/PSP_GAME/USRDIR/DATA/ATT_000_STARTSCREEN.BIN` עם `0x40000001`, כלומר `PSP_O_RDONLY | PSP_O_NPDRM`.
2. קוראת ל-`sceIoIoctl(fd, 0x04100001, key, 16)`, עם מפתח של 16 בתים מה-EBOOT ‏(`0x08C0C4D4`).
3. קוראת 8 בתים ומשווה אותם ל-8 הבתים שב-EBOOT מיד אחרי המפתח ‏(`0x08C0C4E4`).

הקורא ‏(`0x08A19208`) חוזר על הבדיקה בלולאה עד שהיא מצליחה, בלי לוותר על המעבד. כש-`sceIoIoctl` רק החזיר 0, נקראו הבתים המוצפנים, וההשוואה נכשלה. המשחק פתח את הקובץ שוב ושוב בלי לסגור אותו. אחרי 59 פתיחות נגמרו ה-descriptors, ומשם כל פתיחה נכשלה. זה ה-7.7 GB של `build/boot4_UCUS98737.log`.

**זה הקובץ המוצפן היחיד בדיסק.** נבדקו 4 הבתים הראשונים של כל 825 הקבצים בדיסק (`USRDIR` על כל התיקיות שלו, ‏`PSP_GAME` ו-`SYSDIR`):
- 435 מ-437 הקבצים ב-`DATA` הם ארכיונים פתוחים, עם magic ‏`Pack`.
- רק `ATT_000_STARTSCREEN.BIN` מתחיל ב-`\0PGD`.

**הקובץ:** ‏176 בתים. כותרת PGD של `0x90` בתים, 8 בתים של נתונים בבלוק של 1,024, וטבלת MAC. ‏DRM type 1 ו-key index 1, ולכן MAC type 1 ו-cipher type 1: מפתח קבוע של amctrl ועוד המפתח של המשחק.

### המימוש (`rt_io.c`)

הסמנטיקה של `sceIo` ב-PPSSPP ‏v1.20.4 (`__IoIoctl`, ‏`npdrmRead`, ‏`npdrmLseek`):

| פקודה | מה היא עושה |
|---|---|
| ‏`0x04100001` | קוראת `0x90` בתים של כותרת מה-offset של ה-PGD, וקוראת ל-`pgd_open(kirk, header, 2, key)`. המפתח הוא 16 הבתים שהמשחק נתן, או כלום. בהצלחה, הקריאות וה-seek של ה-fd עוברים לנתונים המפוענחים. בכישלון: ‏`0x80510204` אם הכותרת היא PGD (המפתח לא מתאים), אחרת 0 והקובץ נקרא כמו שהוא. |
| ‏`0x04100002` | ה-offset של כותרת ה-PGD (`0x90` בקובץ EDAT). |
| ‏`0x04100010` | גודל הנתונים: של ה-PGD, או של הקובץ. |
| ‏`0x01020003`, ‏`0x01020004` | גודל סקטור (2,048) והמיקום בקובץ, כמו ב-PPSSPP. |

- **קריאה:** בלוק אחרי בלוק, דרך cache של בלוק אחד: קוראים את הבלוק המוצפן מהקובץ ומפענחים עם `pgd_decrypt_block`. הקריאה נעצרת בסוף הנתונים. ‏PPSSPP מגביל רק קריאה אחת לגודל הנתונים, וקריאה שעוברת את הסוף מחזירה אצלו גם בתים מהטבלה.
- **‏seek:** במיקומים של הנתונים המפוענחים. ‏seek שעובר את הסוף מחזיר EINVAL.
- **‏`scePspNpDrm_user`** עבר מ-stubs ל-`rt_io.c`, לפי `scePspNpDrm_user.cpp` של PPSSPP:
  - ‏`sceNpDrmSetLicenseeKey` רק רושם שיש מפתח.
  - ‏`sceNpDrmEdataSetupKey` פותח קובץ EDAT ‏(`\0PSPEDAT`) עם `0x04100002` ‏(`0x90`) ו-`0x04100001` בלי מפתח.
  - ‏`sceNpDrmEdataGetDataSize` מחזיר את גודל הנתונים.
  - GoS טוען את מודול `np_drm` ‏(`0x500`) ומחפש שני DLC: ‏`ms0:/PSP/GAME/UCUS98737/dlcarena.EDAT` ו-`dlccostume.EDAT`. הם לא נמצאים, והמשחק ממשיך.

### מאיפה בא קוד ההצפנה, והרישיונות

הפענוח עצמו הוא `ext/libkirk` של PPSSPP. ‏CMake מוריד אותו בזמן ה-configure מ-tag ‏v1.20.4, בודק כל קובץ ב-SHA-256 (`KIRK_FILES` ב-`CMakeLists.txt`), ושם אותו ב-`build/<dir>/_deps/libkirk`. זה אותו מנגנון של `at3_standalone` ו-OpenH264.

| קבצים | מקור | רישיון |
|---|---|---|
| ‏`kirk_engine.c/h` | ‏KIRK engine, ‏Draan ואחרים (coyotebean, Davee, hitchhikr, kgsws, liquidzigong, Mathieulh, Proxima, SilverSpring) | ‏GPL-3.0-or-later |
| ‏`amctrl.c/h` | ‏reverse engineering של `amctrl.prx`, ‏tpu | בלי הודעת רישיון בקובץ. הוא חלק מ-PPSSPP, ש-GPL-2.0-or-later |
| ‏`bn.c`, ‏`ec.c` | ‏Segher Boessenkool | ‏GPL-2.0 או GPL-3.0 |
| ‏`AES.c/h` | ‏`rijndael-alg-fst.c` ‏(Rijmen, Bosselaers, Barreto), דרך OpenBSD | נחלת הכלל |
| ‏`SHA1.c/h` | ‏Peter Gutmann, ‏A.M. Kuchling, ‏David Ireland | נחלת הכלל |
| ‏`kirk_common.h` | ‏PPSSPP | ‏GPL-2.0-or-later |

**שום מפתח לא נכנס לגיט:**
- המפתחות של ה-KIRK ושל amctrl נמצאים רק בקבצים שהורדו, ב-`build/`, שמוחרג ב-`.gitignore`.
- המפתח של המשחק נשאר ב-EBOOT, ונקרא מהזיכרון של המשחק בזמן ריצה.
- גם הקובץ המפוענח לא נשמר בשום מקום: הוא מפוענח בזיכרון בכל ריצה.

## 3. הלולאה בשנייה 24.4: באג במהדר מחדש

אחרי ה-PGD המשחק הגיע ללוגואים, ל-AUTOLOAD של הפרופיל ולאתחול הסאונד, ואז נתקע. הזמן הווירטואלי נעצר בשנייה 24.389, בלי אף קריאת HLE, עם 172 מיליון קריאות `rt_call`.

**איך זה נמצא:** ה-watchdog עוצר ריצה שלא מוותרת על המעבד, ועכשיו הוא גם מדפיס את ה-profiler ‏(`--profile`). שלוש פונקציות של חישובי וקטורים (`0x08A89B48`, ‏`0x08A8A458`, ‏`0x08A8A5B4`) תפסו 77% מהזמן. הן נקראות ממפרש של רשימת פקודות: לולאה ב-`0x08A8B4BC` עושה `switch` על הפקודה, וכל `case` חוזר ל-`0x08A8B550`, שמקדם את המונה וחוזר לראש הלולאה.

**הבאג:** ‏`0x08A8B550` היא פונקציה ב-Ghidra. הסריקה של המהדר עוקבת אחרי ה-control flow מה-entry, הגיעה לקפיצה אחורה לראש הלולאה (`0x08A8B4BC`), והוסיפה את הקוד הזה לגוף הפונקציה. ‏`emitc.py` כותב את הגוף לפי סדר הכתובות, ולכן ה-C של `f_08A8B550` התחיל בראש הלולאה ולא ב-entry. כל `case` "חזר" לביצוע אותה פקודה שוב, בלי סוף.

**התיקון:** פונקציה שהגוף שלה מתחיל מתחת ל-entry מתחילה ב-`goto L_<entry>;`.

```c
void f_08A8B550(RtCtx *c) {
  goto L_08A8B550;
L_08A8B4BC:;
  ...
```

זה קורה בפונקציה אחת בלבד בשני המשחקים: ‏`0x08A8B550` ב-GoS. ב-CoO אין אף אחת. ה-C של CoO נוצר מחדש עם התיקון ויצא זהה בית-לבית.

## 4. מודולים, ‏NpDrm, ‏SAS

- **‏`sceUtilityLoadModule`/`Unload`** לפי `sceUtility.cpp` של PPSSPP:
  - טבלת המודולים, עם הגודל של כל אחד ועם התלויות שלו. למשל, ‏`av_atrac3plus` ‏(`0x302`) דורש את `av_avcodec` ‏(`0x300`).
  - הזיכרון של המודול מוקצה ב-user partition. ‏`av_atrac3plus` הוא ‏`0x8000` בתים, ו-`av_mpegbase` הוא ‏`0xC000`.
  - טעינה שנייה מחזירה `0x80111102`, ופריקה של מודול שלא נטען מחזירה `0x80111103`.
  - 25 ms של זמן טעינה, ו-0.13 ms ל-`0x3FF`.
  - GoS טוען `0x300`, ‏`0x302` ו-`0x301` פעמיים (עם פריקה ביניהן), ואחר כך `0x303` ו-`0x500`.
  - המודולים עצמם ממומשים ב-HLE מהשלבים הקודמים (`rt_atrac.c`, ‏`rt_sas.c`, ‏`rt_mpeg.c`), כמו ב-CoO. ‏38 קבצי ה-PRX שבתיקיית `MODULE` של הדיסק לא נטענים.
- **‏`__sceSasSetVoicePCM`:** קולות של PCM של 16 ביט, לפי `SasAudio.cpp` של PPSSPP:
  - קריאה ישירה מהזיכרון של המשחק, עם לולאה ממיקום הלולאה.
  - ההשהיה אחרי key-on היא 32 דגימות, בלי תלות ב-pitch מעל הבסיס.
  - הקול נגמר בסוף הדגימות.

## ה-boot של GoS

| זמן וירטואלי | פריים | מה קורה |
|---|---|---|
| 0.000 | 1 | ‏`module_start`, ה-heap ‏`radheap` ‏(`0x12A0C00` ב-`08C5F400`), סריקת התיקיות. ‏`DiscSpinnerThread` פותח את `R6_INTRO.PMF` לפי סקטור. |
| 0.000–0.199 | | ‏`sceUtilityLoadModule` לקודקים, ‏`sceSasInit` |
| 0.225 | | בדיקת ה-PGD מצליחה |
| 1.242–1.267 | | ‏`np_drm`, ובדיקת ה-DLC |
| 0–20 | 1–1,200 | אזהרה, ‏Sony, ‏Santa Monica, ‏Ready At Dawn |
| ~18 | ~1,080 | התפריט הראשי |
| 24.29–24.52 | | ‏AUTOLOAD של `PROFILE00`: אין שמירה |
| 24.57 | | ‏`MENULOOP.AT3` (ATRAC3+), ו-`ATT_010_STERN.BIN` נטען מאחורי התפריט. עד כאן התפריט שקט: המשחק מאתחל את הסאונד (`snd_stream`) מחדש אחרי בדיקת הפרופיל |
| 28.85 | | ‏New Game ורמת קושי: ‏`VideoSoundThread`, סרטון הפתיחה |
| 135.27 | ~4,920 | סוף הסרטון: ‏3,153 תמונות. ‏`ATT_020_BOW.BIN` ו-`ATTCOM1L.AT3` |
| ~158 | ~6,300 | הכותרת "Atlantis" |
| ~161–172 | ~6,450–7,100 | ה-tutorials של ההתקפות וה-grab, ‏"Press ✕ to continue" |
| ~173–180 | ~7,180–7,600 | ה-QTE: ‏L ו-R לסירוגין |
| 180– | 7,600– | קרב, ‏cutscene במנוע, ה-tutorials של Block ו-Launch |

![לוג ה-boot](../media/screenshots/2026-10-16_34_gos_boot_log.png)

## אימות

### הדרך לשלב, headless

‏`build/gos/ref_v1` הוא הייחוס של GoS: ‏`--renderer gpu`, התסריט `input.txt` (מתוך `build/gos/input_level1.txt`), פריים כל 150 עד 9,000, ‏`mix.wav` ו-`perf.csv`, עם `ms0` ריק.

התסריט:
- ‏✕ בפריימים 1400, ‏1550, ‏1700 ו-1850: ‏New Game ו-Hero (Normal).
- ‏✕ בפריים 7150. לפני כן ה-tutorial עוד לא מקבל קלט: ✕ ב-7000 לא סגר אותו.
- ‏L ו-R לסירוגין מ-7180 עד 7600.
- קומבואים: □□□△, תנועה עם הסטיק ו-○.

הריצה לוקחת 87 שניות: 103 FPS בממוצע, כולל שמירת הפריימים, והסרטון מפוענח מהר ככל שאפשר.

### בחלון

**ריצת התסריט** (`--window --renderer gpu --input`):

| קטע | פריימים | FPS | p99 |
|---|---|---|---|
| התפריט | 1,100–1,700 | 58.5 | 20.5 ms |
| הסרטון | 1,800–4,900 | 30.0 | 34.1 ms |
| השלב: ‏cutscene, הכותרת, ה-tutorials | 5,000–7,100 | 60.0 | 22.2 ms |
| השלב: ה-QTE וקרב | 7,200–9,000 | 59.3 | 24.2 ms |

- **דטרמיניזם:** ה-WAV של ריצת החלון זהה בית-לבית ל-WAV של ריצת ה-headless עם אותו תסריט. גם הזמן הווירטואלי (203.319 s) ומספר החלפות ה-thread ‏(74,072) זהים.
- **הסאונד:** תור של 54 ms, ‏playback ratio ‏1.0008, ו-9 underruns ב-203 שניות. בזמן הריצה, סשן אחר הריץ על אותו מחשב חמש ריצות headless של CoO (עומס של 56–73% על המעבד).

![רגעים מריצת החלון](../media/screenshots/2026-10-16_35_gos_window_sequence.png)

**ריצת המקלדת** (`--window --renderer gpu --record-input`, בלי תסריט). הקלט הגיע ממקלדת אמיתית, כמו בשלב 9: הודעות `WM_KEYDOWN`/`WM_KEYUP` שנשלחו לחלון של ה-port בלבד, לפי מספר הפריים בכותרת. ‏Z הוא ✕, ‏Q/W הם L/R, ‏A/S הם □/△, ‏I/J/K/L הם הסטיק.
- **הרצף:** ‏✕ על New Game ‏(1,536) ועל Hero ‏(1,718), סרטון הפתיחה, ‏✕ ב-tutorial של ה-grab ‏(7,172), ‏L/R לסירוגין, ודקה של קרב.
- **ביצועים:** אותם FPS כמו בריצת התסריט: 60 FPS בשלב.
- **הסאונד:** underrun אחד, בשנייה 135.6, בטעינת השלב אחרי הסרטון. תור של 50 ms, ‏playback ratio ‏1.0002.
- **ההקלטה:** ‏157 מקטעי קלט. ה-replay שלה ב-headless נתן WAV זהה בית-לבית לריצת החלון, ואותם זמן וירטואלי (203.297 s) ומספר החלפות thread ‏(73,956).

![ריצת המקלדת בחלון: ה-tutorial של ה-grab וקרב](../media/screenshots/2026-10-16_37_gos_window_keyboard_session.png)

צילומי החלון נלקחו מהחלון של ה-port בלבד, עם PrintWindow (`tools/capture_window.ps1`).

**הערה על הצילומים:** ‏PrintWindow מעתיק גם את ערוץ האלפא של החלון. בסרטונים ה-framebuffer מגיע עם אלפא 0 בחלק מהפיקסלים, ובקובץ PNG של 32 ביט הם יצאו שקופים, ונראו כמו כתמים לבנים. בחלון עצמו התמונה תקינה. ‏`capture_window.ps1` שומר עכשיו 24 ביט.

### הסרטונים

- **סרטון הפתיחה ‏(`R6_INTRO.PMF`):** ‏`--movie-test` מפענח את 3,153 התמונות זהות בית-לבית ל-`ffmpeg -f rawvideo -pix_fmt yuv420p`, עם 0 שגיאות פענוח.
- **כל 34 הסרטונים שבדיסק** (`MOVIES` מכילה 34 קבצי PMF וקובץ XML): כל 60,637 התמונות זהות בית-לבית ל-FFmpeg, עם 0 שגיאות פענוח. בתוך המשחק נבדק רק סרטון הפתיחה.
- **הסאונד של הסרטון במיקס של המשחק:** מתאים לפענוח של הקובץ ב-77–81 dB ‏(gain 0.9, הווליום של המשחק), באותו היסט לאורך כל הסרטון (29.943 s), כלומר בלי סחיפה.

### המוזיקה

במיקס של המשחק המוזיקה מעורבבת עם אפקטי ה-SAS (גשם ורעמים בתפריט, הקרב בשלב), ולכן אי אפשר להשוות אותה ישירות. בשביל הבדיקה נוסף `RT_SAS_MUTE`: הקולות של ה-SAS רצים כרגיל אבל לא נשמעים, כמו ה-mute של PPSSPP.

| מוזיקה | זמן במיקס | מול FFmpeg |
|---|---|---|
| ‏`MENULOOP.AT3` (התפריט) | ‏26, ‏35, ‏50 s | ‏79.4, ‏76.9, ‏80.5 dB, ‏gain 0.517 |
| ‏`ATTCOM1L.AT3` (השלב) | ‏140, ‏170, ‏200 s | ‏76.8, ‏76.9, ‏74.9 dB, ‏gain 0.517 |

ההפרש הגדול ביותר הוא 2 LSB, וההיסט קבוע.

![המיקס של ריצת החלון](../media/screenshots/2026-10-16_36_gos_audio_spectrogram.png)

### ה-renderer החומרה מול ה-renderer התוכנה

‏`--renderer both --gpu-compare-at`, על אותו תסריט:

| פריים | מה | PSNR | פיקסלים שסוטים ביותר מ-32 |
|---|---|---|---|
| 1,500 | התפריט הראשי | 30.6 dB | 0.00% |
| 1,650 | בחירת רמת הקושי | 29.3 dB | 0.00% |
| 2,400 | סרטון הפתיחה | 49.6 dB | 0.00% |
| 6,300 | הכותרת "Atlantis" | 31.8 dB | 0.00% |
| 7,050 | ה-tutorial של ה-grab | 33.7 dB | 0.01% |
| 7,800 | קרב | 33.3 dB | 0.00% |
| 8,250 | קרב | 32.1 dB | 0.00% |

ההבדלים קטנים ומפוזרים: סינון של טקסטורות, והרקע המטושטש של התפריט. בשלב זה קרוב ל-CoO ‏(34.1–34.8 dB). בתפריט של GoS זה פחות מבתפריטים של CoO ‏(41–49 dB), ולא נבדק למה.

### Chains of Olympus לא נשבר

ה-build של CoO נבנה מחדש בתיקייה נפרדת (`build/cmake_coo_p10`), כי סשן אחר השתמש באותו זמן ב-`build/cmake_full`. הוא הורץ עם התסריט של `build/rec_v9`, ב-renderer התוכנה ועם `ms0` ריק:

- ‏34/34 פריימים זהים פיקסל-אחר-פיקסל ל-`rec_v9` ‏(`tools/check_frames.py`).
- ה-WAV זהה בית-לבית (SHA-256 ‏`1c1ad676…`).
- אותו זמן וירטואלי (143.705 s), ואותן 36,386 החלפות thread.

## איך מריצים

```
tools\play_gos.cmd                                   משחק בחלון, --renderer gpu; שמירות ב-build\ms0_gos
tools\play_gos.cmd --renderer soft                   עם ה-renderer התוכנה
tools\replay.cmd build\sessions\<תאריך_שעה>          replay headless; ‏build.txt קובע איזה משחק
```

‏`play_gos.cmd` עובד כמו `play.cmd`: כל משחק מוקלט ב-`build\sessions\<תאריך_שעה>\`. השמירות של GoS נשמרות ב-`build\ms0_gos`, כדי שלא יגעו ב-`build\ms0` של CoO.

הבנייה (פירוט ב-[`recomp/README.md`](../recomp/README.md)):

```bash
python -I recomp/recomp.py --elf re/dumps/UCUS98737_EBOOT.BIN --ghidra re/out/recomp/UCUS98737_EBOOT.BIN.recomp.json --nids re/out/psp_nids_ppsspp.tsv --out build/gen/UCUS98737_full --difftest 0 --boot 0 --all
cmake -S . -B build/cmake_gos_full -G "Visual Studio 18 2026" -A x64 -DGAME=UCUS98737_full
cmake --build build/cmake_gos_full --config Release --parallel
```

ריצת הייחוס, headless:

```bash
build/cmake_gos_full/Release/gow_rt_UCUS98737_full.exe --image build/gen/UCUS98737_full/image.bin --boot --disc "God of War - Ghost of Sparta (USA) (En,Fr,Es).chd" --ms0 build/gos/ref_v1/ms0_empty --cpu-ms 900000 --wall-ms 900000 --renderer gpu --input build/gos/ref_v1/input.txt --frames-dir build/gos/ref_v1 --frames-every 150 --max-frames 9000 --wav build/gos/ref_v1/mix.wav --perf-csv build/gos/ref_v1/perf.csv
```

## מה עוד לא נבדק

- **רק תחילת השלב הראשון.** ריצות התסריט הגיעו עד פריים 10,800, בקרב שאחרי ה-tutorial של Launch. שאר המשחק לא שוחק.
- **שמירות:** רק ה-AUTOLOAD של ה-boot, בלי שמירה. נקודת השמירה הראשונה לא הושגה.
- **‏`__sceSasSetVoicePCM`** ממומש, אבל אף פעם לא נקרא.
- **ה-renderer התוכנה** עובד ב-GoS, אבל איטי: ‏36 FPS ב-headless בלוגואים ובתפריט. ‏`play_gos.cmd` משתמש ב-GPU.
- **הכותרת של החלון** היא "God of War (PSP native port)" בשני המשחקים.
- **‏2,911 דגימות נחתכות** במיקס של 203 שניות, בעיקר ברעמים של התפריט ובקרב. ערוצי ה-sceAudio מחוברים ונחתכים ל-16 ביט, כמו ב-PPSSPP. לא נבדק מה ה-PSP עושה בדיוק.
