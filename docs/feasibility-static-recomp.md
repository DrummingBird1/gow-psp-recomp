# God of War PSP – דו"ח היתכנות ל-Static Recompilation

**תאריך:** 2026-10-15
**משחקים:** God of War: Chains of Olympus (UCUS98653) · God of War: Ghost of Sparta (UCUS98737)
**כלים:** Ghidra 12.1.3, ghidra-allegrex v21.4, PPSSPP 1.20.4 (לפענוח), סקריפטים ב-`re/scripts/`
**עדכון 2026-10-16:** שלב 1 (אב-טיפוס של ה-recompiler) הושלם. התוצאות ב-[phase1-prototype.md](phase1-prototype.md).

---

## השורה התחתונה

**זה אפשרי, ובצד של ה-CPU המצב טוב באופן חריג.** הקושי האמיתי הוא ה-runtime שמחליף את מערכת ההפעלה והחומרה של ה-PSP, ובראשו מנוע הגרפיקה (GE).

| תחום | הערכה | למה |
|---|---|---|
| גילוי קוד | 🟢 מצוין | 99.88% מ-`.text` פורק, 0 שגיאות פענוח. ה-relocations של ה-PRX נותנים רשימה מלאה של מצביעים לקוד |
| זרימת בקרה עקיפה | 🟢 טוב | 317/319 ו-172/174 קפיצות מחושבות נפתרו (נשארו רק 2 בכל משחק) |
| VFPU | 🟢 קל | רק כ-1,200 עד 1,400 פקודות ב-81 עד 109 פונקציות, ובעיקר מתמטיקה בסיסית |
| קוד שמשנה את עצמו או overlays | 🟢 לא נמצא | אין import של icache invalidation. המודולים שנטענים הם codecs של Sony בלבד |
| שכבת OS (HLE) | 🟡 בינוני | 199 ו-206 פונקציות. ל-PPSSPP יש מימוש לכל אחת מהן, אז יש מקור התנהגות |
| גרפיקה (GE) | 🔴 הקשה ביותר | המנוע בונה display lists של GE בעצמו, וצריך renderer מלא |
| אודיו ווידאו | 🟡 בינוני | SAS, ATRAC3+ ו-MPEG/H.264. יש מימושים קיימים (PPSSPP, FFmpeg) |
| threads ותזמון | 🟡 בינוני | מתזמן single-core מבוסס עדיפויות. צריך fibers ולא threads אמיתיים |

**הערכת היקף (ניחוש מושכל, אי-ודאות גבוהה):** בערך 6 עד 12 חודשי עבודה עקבית עד משחק ראשון שאפשר לשחק בו מתחילתו ועד סופו. המשחק השני יעלה הרבה פחות, כי שניהם רצים על אותו מנוע ואותו runtime.

---

## מה נבדק ואיך

1. **פענוח:** PPSSPP 1.20.4 עם `DumpFileTypes = 3` שמר את ה-EBOOT המפוענח של כל משחק. שני הקבצים הם ELF32 LE מסוג `SCE_PRX` (0xFFA0), עם flags `0x10a23001`, ושם המודול בשניהם הוא `dax`.
2. **ניתוח:** auto-analysis מלא ב-Ghidra עם Allegrex. אחר כך הוספתי פונקציות בכל יעד של relocation מתוך data שלא זוהה (2,175 ב-CoO ו-1,504 ב-GoS) והרצתי ניתוח חוזר.
3. **שמות ל-imports:** בניתי טבלת NID מקוד ה-HLE של PPSSPP (1,611 NIDs). שמות כל ה-imports נקבעו בפרויקט.
4. **מדידות:** סיווג פקודות, פונקציות, קפיצות עקיפות, מצביעים מ-data, דגימת decompiler ו-fingerprints להשוואה בין המשחקים.
5. **REA:** לא שמיש למשחקים האלה. ב-Windows המסלול של REA ל-Ghidra מקבל רק קבצי PE, ואין לו מסלול MIPS ל-IDA. ב-Linux הוא מקבל רק ET_EXEC של homebrew. בדקתי את זה ישירות עם `open_binary`.

---

## מספרים מרכזיים

| מדד | Chains of Olympus | Ghost of Sparta |
|---|---:|---:|
| גרסת SDK | 3.70 | 6.03 עד 6.05 |
| גודל `.text` | 3.15MB | 4.14MB |
| פקודות שפורקו | 787,079 | 1,033,953 |
| כיסוי `.text` | 99.88% | 99.88% |
| פונקציות | 15,872 | 20,249 |
| חציון / מקסימום פקודות לפונקציה | 25 / 2,225 | 25 / 2,256 |
| פקודות FPU (single precision) | 109,448 (13.9%) | 147,023 (14.2%) |
| פקודות VFPU / פונקציות עם VFPU | 1,193 / 81 | 1,355 / 109 |
| VFPU prefixes (`vpfx*`) | 19 ב-4 פונקציות | 19 ב-4 פונקציות |
| branch-likely | 8,406 | 11,671 |
| קפיצות מחושבות (`jr` שאינו `ra`) / נפתרו | 319 / 317 | 174 / 172 |
| קריאות עקיפות (`jalr`) | 3,376 | 5,191 |
| relocations ב-`.text` | 100,272 | 117,296 |
| מילים ב-data שמצביעות לקוד / יעדים שונים | 17,060 / 10,162 | 24,410 / 11,282 |
| imports / ספריות | 199 / 21 | 206 / 22 |
| imports ש-PPSSPP מממש | 199 (100%) | 206 (100%) |
| דגימת decompile שהצליחה | 623/623 + 81/81 VFPU | 605/605 + 109/109 VFPU |

---

## ממצאים לפי תחום

### 1. גילוי קוד: מצוין
- אין אף שגיאת פענוח (bookmark של ERROR) באף משחק, כך ש-Allegrex מזהה את כל הפקודות.
- זה היתרון הגדול של קובץ PRX: כל מילה ב-data שמצביעה לקוד מסומנת ב-relocation. לכן יש רשימה מדויקת של vtables, callbacks ו-jump tables, בלי לנחש. אחרי ההוספה, כל 10,162 ו-11,282 היעדים הם תחילת פונקציה או case בתוך switch.
- נשארו 3,800 ו-4,884 בייטים לא מפורקים, ב-691 ו-933 רווחים קטנים של כ-5 בייטים בממוצע. כנראה padding בין פונקציות, אבל עוד לא אימתתי את זה.

### 2. זרימת בקרה עקיפה: טוב
- **jump tables:** כמעט כולן נפתרו סטטית. נשארו 2 קפיצות לא פתורות בכל משחק, ואותן צריך לבדוק ידנית.
- **`jalr`:** אלה בעיקר קריאות וירטואליות של C++ ומצביעי פונקציה. ב-recompilation פותרים אותן עם טבלת lookup בזמן ריצה מכתובת לפונקציה, כמו ב-N64Recomp. בזכות ה-relocations, רשימת היעדים האפשריים כמעט מלאה.

### 3. delay slots ו-branch-likely
אלה דפוסים סטנדרטיים של MIPS שכל recompiler ל-MIPS יודע לטפל בהם. ל-branch-likely יש 8 עד 12 אלף מקרים, וה-delay slot שלהם מבוטל כשהענף לא נלקח. אין כאן סיכון מיוחד.

### 4. FPU
כ-14% מהקוד הוא FPU ב-single precision, והוא מתורגם ישירות ל-`float` ב-C. צריך לשים לב לכמה הבדלים מ-IEEE: flush-to-zero, מצבי עיגול (FCR31), ההתנהגות של `cvt.w.s` והשוואות עם NaN. PPSSPP כבר מטפל בכולם, אז אפשר להעתיק משם את הסמנטיקה.

### 5. VFPU: קל מהצפוי
- כמעט כל השימוש הוא בפעולות בסיסיות: `lv.s/lv.q/sv.s/sv.q`, `vdot`, `vscl`, `vsin`, `vcos`, `vmov`.
- יש רק 19 פקודות prefix, ב-4 פונקציות בכל משחק. אפשר לממש אותן ידנית, אחת אחת.
- `vsin` ו-`vcos` הם קירובים של החומרה. כדי לקבל התנהגות זהה, אפשר להעתיק את הטבלאות של PPSSPP.

### 6. אין קוד שמשנה את עצמו ואין overlays
- אין import של `sceKernelIcacheInvalidate*`, רק `sceKernelDcacheWriteback*` שקשור ל-DMA של ה-GE.
- CoO קורא ל-`sceKernelLoadModule` במקום אחד, וזה בשביל 6 ה-PRX של Sony שנשמרו (audiocodec, libatrac3plus, mpeg, mpegbase, sc_sascore, videocodec). GoS משתמש ב-`sceUtilityLoadModule`. בשני המקרים נממש את המודולים ב-HLE ולא נקמפל אותם.
- לכן, ככל שאפשר לראות סטטית, כל קוד המשחק נמצא ב-EBOOT אחד.

### 7. שכבת ה-OS (HLE)
מה המשחקים צריכים מה-runtime, לפי ה-imports שקיבלו שמות:

| תחום | ספריות | פונקציות (CoO/GoS) | הערות |
|---|---|---:|---|
| threads וסנכרון | ThreadManForUser, Kernel_Library | 37 / 40 | 6 עד 7 מקומות שיוצרים thread. semaphores, event flags, FPL, callbacks |
| קבצים | IoFileMgrForUser | 21 / 22 | כולל IO אסינכרוני. ה-runtime יקרא ישירות מה-CHD או ה-ISO |
| זיכרון | SysMemUserForUser, sceSuspendForUser | 12 / 12 | כולל VolatileMem, ה-4MB הנוספים |
| גרפיקה | sceGe_user, sceDisplay | 11 / 11 | `sceGeListEnQueue` נקרא מ-9 מקומות, `sceDisplayWaitVblankStart` מ-8 |
| אודיו | sceAudio, sceSasCore, sceAtrac3plus | 54 / 55 | mixer של SAS ופענוח ATRAC3+ |
| וידאו | sceMpeg | 25 / 25 | סרטוני PMF (H.264 ו-ATRAC3+) |
| UI של המערכת | sceUtility, sceImpose | 10 / 12 | חלונות שמירה ו-MsgDialog. ה-runtime צריך לצייר אותם |
| DRM | scePspNpDrm_user | 0 / 2 | רק ב-GoS: קבצי `.edat` מוצפנים. PPSSPP מפענח אותם |
| שונות | LoadExec, ModuleMgr, Stdio, Utils, Power, Rtc, Umd, Ctrl | 29 / 27 | |

ל-PPSSPP יש מימוש לכל 405 הפונקציות, כך שיש מקור התנהגות מלא. אם נעתיק ממנו קוד, ה-runtime יהיה תחת רישיון GPL-2.0, וזה בסדר לפרויקט אישי.

### 8. גרפיקה: החלק הקשה
- המנוע (`sgl.base.psp.cpp` לפי מחרוזות ה-assert) בונה display lists גולמיים של GE ושולח אותם עם `sceGeListEnQueue`. לכן ה-runtime צריך מפרש של פקודות GE ו-renderer מודרני (Vulkan, D3D12 או OpenGL), עם אמולציה של 2MB VRAM, framebuffers, render-to-texture ו-readbacks.
- **אפשרות א':** להשתמש בשכבת ה-GPU של PPSSPP. זה הכי מהיר, אבל קשור לארכיטקטורה של PPSSPP.
- **אפשרות ב':** לכתוב renderer משלנו ל-GE. יותר עבודה, אבל יותר שליטה.
- **אפשרות ג', בהמשך:** hooks על פונקציות של `sgl` ברמת המנוע, בשביל widescreen, רזולוציה גבוהה ושיפורים אחרים.

### 9. threads ותזמון
ה-PSP הוא single-core, עם תזמון preemptive לפי עדיפויות, וההחלפה בין threads קורית בעיקר בקריאות חוסמות. לכן נריץ כל thread כ-fiber ונחליף ביניהם בקריאות HLE, כמו ש-PPSSPP עושה. הסיכון: race conditions שמסתמכים על סדר תזמון מסוים. להגיע ל-60FPS ידרוש שינויי לוגיקה, כי כנראה המשחק קשור ל-VBlank. את זה עוד לא בדקתי.

### 10. מנוע משותף לשני המשחקים
- **שם המנוע:** `dax` בשני המשחקים.
- **נתיבי מקור במחרוזות:** ב-CoO `c:/work/daxx/src/libs/...`, וב-GoS `c:/work/coo2/dev/main/src/libs/...`. כנראה Ghost of Sparta נקרא בפנים "Chains of Olympus 2".
- **אותן ספריות:** animation, core, elements2d, elements3d, elementsxd, essentials, sgl, simulation, threaddata.
- **התאמה מדויקת:** 39.5% מהפונקציות של GoS עם 8 פקודות ומעלה (6,001 מתוך 15,184) זהות בצורה לפונקציה ב-CoO, וזה 21.7% מהפקודות. ההשוואה מחמירה: היא דורשת אותם registers ואותן פקודות, והקומפיילר השתנה בין SDK 3.70 ל-6.0x. לכן החפיפה האמיתית כנראה גבוהה יותר. אפשר לבדוק את זה עם Version Tracking או BSim.
- **המשמעות:** runtime אחד לשני המשחקים, ושמות והערות שאפשר להעביר ביניהם.

### 11. decompiler (לפאצ'ים, לא חובה ל-recompilation)
- **הצלחה:** 100% בדגימה של 1,228 פונקציות ו-190 פונקציות VFPU, עם ממוצע של 4 עד 5ms לפונקציה (19 עד 23ms לפונקציות VFPU).
- **אזהרות:** ב-2% עד 3% מהפונקציות שנדגמו.
- **VFPU:** ב-60% מפונקציות ה-VFPU מופיעים inputs לא ממודלים (`in_`), כי ghidra-allegrex לא ממדל את מצב ה-VFPU במלואו.
- **מסקנה:** מספיק טוב כדי לכתוב פאצ'ים ממוקדים כמו widescreen או רזולוציה. ה-recompiler עצמו מתרגם פקודות ולא את הפלט של ה-decompiler.

---

## ארכיטקטורה מוצעת

```
CHD/ISO של המשתמש ──► runtime (מקומפל, נייטיב ל-Windows)
                      ├─ זיכרון: arena של 32MB בכתובות PSP (כולל VolatileMem ו-2MB VRAM)
                      ├─ CPU context: 32 GPR, HI/LO, 32 FPR, 128 רגיסטרי VFPU ומצב prefix
                      ├─ קוד המשחק: C שנוצר מה-EBOOT (פונקציה ל-C לכל פונקציה)
                      ├─ טבלת lookup מכתובת לפונקציה (בשביל jalr ו-jr)
                      ├─ HLE: threads (fibers), IO, זיכרון, utility, NpDrm
                      ├─ GE: מפרש display lists ו-renderer (Vulkan/D3D12)
                      ├─ אודיו: SAS, ATRAC3+, sceAudio ל-SDL3
                      ├─ וידאו: PMF דרך FFmpeg
                      └─ קלט, חלון, הגדרות: SDL3
```

הקלט ל-recompiler הוא רשימת הפונקציות וה-relocations מפרויקט ה-Ghidra. הכתובת שבה נטען ה-PRX נקבעת מראש, למשל `0x08804000` כמו ב-PPSSPP, ואת ה-relocations מחילים בזמן ה-recompilation.

כדאי לבחון את N64Recomp (שגם הוא recompiler ל-MIPS) כבסיס או כהשראה. Allegrex פשוט יותר מה-VR4300, כי הוא 32-bit בלבד. צריך לבדוק את הרישיון ואת ההתאמה.

**בדיקות:** נשווה ריצה של הקוד המתורגם לריצה ב-PPSSPP, למשל עם ה-debugger שלו, ונשווה את מצב ה-registers בנקודות עצירה. זו הדרך לתפוס באגים בתרגום מוקדם.

---

## תוכנית בשלבים

| שלב | מטרה | אבן דרך שמראה שהצלחנו | הערכה |
|---|---|---|---|
| 0 | ניתוח והיתכנות | הדו"ח הזה | ✅ הושלם |
| 1 | recompiler MVP | כל 15,872 הפונקציות מתורגמות ל-C ומתקמפלות. `module_start` רץ עד קריאת ה-HLE הראשונה | ✅ אב-טיפוס הושלם: 98% מתורגם, ה-boot מגיע עד thread הספינר |
| 2 | kernel HLE | המשחק עולה עד `sceGeListEnQueue` הראשון (בלוג) | 3 עד 6 שבועות |
| 3 | renderer ל-GE | הלוגו והתפריטים מוצגים | 1.5 עד 4 חודשים (תלוי באפשרות א' או ב') |
| 4 | אודיו, וידאו, שמירות | אפשר לשחק את הפתיחה עם סאונד וסרטונים | 1 עד 2 חודשים |
| 5 | יציבות ושיפורים | משחק מלא, GoS, לאנצ'ר, רזולוציה, widescreen ו-60FPS | פתוח |

**נקודת החלטה אחרי שלב 2:** אם ה-HLE של ה-threads מתגלה כשביר, שוקלים להשאיר את ה-PPSSPP הקיים לגרסה שאפשר לשחק בה.

## סיכונים

1. **GE:** אפקטים של framebuffer (render-to-texture וקריאות חוזרות מ-VRAM) הם המקור הנפוץ לבאגים ויזואליים.
2. **תזמון:** המשחק עלול להסתמך על סדר תזמון או על timing מסוים של ה-PSP.
3. **נומריקה:** הבדלים קטנים ב-FPU ובקירובים של VFPU עלולים לגרום לסטיות בפיזיקה.
4. **2 קפיצות לא פתורות בכל משחק:** צריך לבדוק אותן ידנית.
5. **היקף:** פרויקט ארוך שדורש התמדה.
6. **משפטי:** ה-dumps והקוד המתורגם מכילים קוד של Sony, ולכן הם לשימוש אישי בלבד. את ה-runtime אפשר להפיץ בנפרד, כמו במודל של N64Recomp שבו המשתמש מביא את המשחק שלו. אם נשתמש בקוד של PPSSPP, ה-runtime יהיה תחת GPL.

## מה כדאי לעשות עכשיו

1. לבדוק את 691 ו-933 הרווחים הקטנים שנשארו לא מפורקים, ואת 2 הקפיצות הלא פתורות בכל משחק.
2. לייצא מהפרויקט את קלט ה-recompiler: רשימת פונקציות, relocations ו-jump tables בפורמט JSON.
3. לבנות אב-טיפוס של recompiler על כמה מאות פונקציות, עם runtime מינימלי ובדיקה מול PPSSPP.

---

## נספח: קבצים ושחזור

| נתיב | תוכן |
|---|---|
| `re/ghidra/GoW_PSP.gpr` | פרויקט Ghidra: שתי התוכניות, אחרי ניתוח, עם שמות ל-imports והערות NID |
| `re/dumps/` | EBOOT ו-PRX מפוענחים (לשימוש אישי) |
| `re/out/*.metrics.json`, `*.functions.csv` | מדדים ורשימת פונקציות |
| `re/out/*.imports.json` | imports עם NID, שם, מספר קוראים וסטטוס ב-PPSSPP |
| `re/out/*.datapointers.json` | מצביעים מ-data לקוד, מתוך ה-relocations |
| `re/out/*.coverage.json`, `*.decompile.json/.csv` | בדיקת הכיסוי ודגימת ה-decompiler (מהמצב שלפני הוספת הפונקציות) |
| `re/out/*.fingerprints.csv` | fingerprints להשוואה בין המשחקים (מהמצב שלפני הוספת הפונקציות) |
| `re/out/psp_nids_ppsspp.json/.tsv` | טבלת NID מ-PPSSPP 1.20.4 |
| `re/scripts/` | הסקריפטים: `ExportRecompMetrics`, `NamePspImports`, `DataPointersIntoText`, `SeedFunctionsFromRelocations`, `ProbeCoverage`, `SampleDecompile`, `FunctionFingerprints`, `build_nid_db.mjs` |

**הרצה חוזרת של המדדים** (ממצב read-only):

```bash
"E:/AI/GoW_PSP/tools/ghidra_12.1.3_PUBLIC/support/analyzeHeadless.bat" E:/AI/GoW_PSP/repo/re/ghidra GoW_PSP -process -noanalysis -readOnly -scriptPath E:/AI/GoW_PSP/repo/re/scripts -postScript ExportRecompMetrics.java E:/AI/GoW_PSP/repo/re/out
```
