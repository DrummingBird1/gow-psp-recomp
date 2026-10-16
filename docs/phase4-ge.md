# GE: מפרש רשימות תצוגה ו-renderer תוכנה

**תאריך:** 2026-10-16 · **branch:** `phase2/threads-and-fs` · **קוד:** `runtime/rt_ge.c`, `runtime/rt_gesoft.c`, `runtime/rt_frames.c`, `runtime/rt_prof.c`

## השורה התחתונה

- **התפריט הראשי של Chains of Olympus מצויר על המסך.** הוא כולל את Kratos בתלת-ממד עם תאורה ו-skinning, את העיר הבוערת עם עשן ואש, את הלוגו ואת התפריט.
- **מה עובר בדרך:** מסך הזכויות של Sony ו-Ready At Dawn, כולל ה-fade-out. אחריו מסך שחור במקום סרטון הפתיחה, כי sceMpeg עוד לא ממומש.
- **ביצועים** (i5-14500, ‏20 threads):
  - התפריט עולה מ-3.6 ל-26–28 פריימים לשנייה כשאין הקלטה, ול-24 בזמן הקלטה.
  - ריצת boot עד התפריט, 22.5 שניות של זמן משחק, לוקחת 22 שניות במקום 140.
- **התוצאה זהה פיקסל-אחר-פיקסל** בין גרסת ה-thread היחיד לגרסה המרובה. זה נבדק על פריימים 360, 840, 960 ו-1080.

![התפריט הראשי](../media/screenshots/2026-10-16_05_coo_main_menu.png)

## מבנה

| קובץ | תפקיד |
|---|---|
| `rt_ge.c` | מפרש רשימות התצוגה (ראו פירוט מתחת לטבלה). |
| `rt_gesoft.c` | ה-renderer: פענוח vertices, ‏transform ותאורה, ‏clipping, ‏rasterization, טקסטורות והשלבים לכל פיקסל. |
| `rt_frames.c` | שמירת פריימים ל-PNG, הקלטה ל-MP4 דרך ffmpeg, ויומן זמני פריים (CSV). |
| `rt_prof.c` | profiler שדוגם את ה-thread הראשי פעם במילישנייה ומתרגם את הדגימות לשמות פונקציות דרך ה-PDB ‏(`--profile`). |

מה `rt_ge.c` עושה:
- **בקרת זרימה:** ‏JUMP, ‏CALL, ‏RET, ‏BASE, ‏OFFSET ו-ORIGIN.
- **מצב:** המצב, המטריצות וה-CLUT.
- **העתקות בלוקים:** ‏block transfers.
- **סטטיסטיקות.**

הסמנטיקה לקוחה מה-renderer התוכנה של PPSSPP ‏(GPU/Software, ‏v1.20.4). הקוד עצמו הוא מימוש C עצמאי.

### Pipeline

1. **פענוח vertex:**
   - כל הפורמטים: weights, ‏UV, צבע, נורמל ומיקום.
   - יישור הרכיבים כמו בחומרה.
   - morphing.
   - skinning עם עד 8 עצמות.
2. **Transform:**
   - world, ‏view ו-projection.
   - viewport, ו-offset בדיוק של 1/16 פיקסל.
   - through mode.
   - fog לפי עומק ב-view space.
   - יצירת קואורדינטות טקסטורה: UV עם scale/offset, הטלה דרך מטריצת tgen, ו-environment map.
3. **תאורה:**
   - 4 אורות: directional, ‏point ו-spot.
   - ambient, ‏diffuse, ‏specular ו-powered diffuse.
   - material update מצבע ה-vertex.
   - צבע משני.
   - החשבון בשלמים, בעיגול של החומרה (‏"×2+1" ו-512).
4. **Primitives:**
   - triangles, ‏strips, ‏fans, ‏sprites, ‏lines ו-points.
   - culling לפי כיוון, כולל היפוך כיוון בתוך strip.
   - צבע provoking ב-flat shading.
5. **Clipping:** רק מול ה-near plane, כמו ב-PSP.
   - אם יש vertex מחוץ לטווח המסך, הפרימיטיב נזרק.
   - אם יש vertex מחוץ לטווח העומק בלי depth clamp, הפרימיטיב נזרק.
6. **Rasterization:**
   - edge functions בדיוק של 1/16 פיקסל, עם דגימה במרכז הפיקסל (`+7`) וכלל top-left.
   - צבע ועומק באינטרפולציה לינארית במסך.
   - טקסטורות עם תיקון פרספקטיבה.
7. **טקסטורות:**
   - פורמטים: 565, ‏5551, ‏4444, ‏8888, ‏CLUT4/8/16/32 ו-DXT1/3/5.
   - swizzle.
   - wrap/clamp.
   - nearest ו-bilinear.
   - mipmaps עם LOD אוטומטי, slope ו-const.
   - texture functions: ‏modulate, ‏decal, ‏blend, ‏replace ו-add, עם color doubling.
8. **לכל פיקסל:**
   - טווח עומק.
   - alpha test.
   - fog.
   - color test.
   - stencil ו-depth.
   - blending: כל ה-factors וכל המשוואות.
   - logic ops.
   - write masks.
   - clear mode.

### ריבוי ליבות

פרימיטיבים לא מצוירים מיד. הם נכנסים לתור, יחד עם snapshot של מצב ה-rasterizer, ‏palette מורחבת מראש ל-CLUT4/CLUT8 וגרסת ה-CLUT.

בזמן flush, השורות מחולקות לפסים של 4 שורות:
- כל thread לוקח את הפס הפנוי הבא, באופן דינמי. זה חשוב במעבד היברידי עם ליבות P ו-E.
- הוא מצייר בפס הזה את כל הפרימיטיבים שבתור, לפי הסדר.
- לכן כל פיקסל רואה בדיוק את אותו סדר ציור כמו ב-thread יחיד.

מתי מתבצע flush:
- בסוף כל רשימת תצוגה.
- לפני block transfer.
- לפני טעינת CLUT מ-VRAM.
- לפני ציור עם טקסטורה שנמצאת ב-VRAM, למשל render target מוקדם יותר.

flush קטן, פחות מ-16K פיקסלים, מצויר ב-thread הקורא בלבד ובלי להעיר את שאר ה-threads.

## אימות

- **השוואת פריימים:** ‏`tools/check_frames.py` משווה פריימים בין ריצות. המשחק דטרמיניסטי בזמן וירטואלי, ולכן כל שינוי ביצועים חייב לתת תמונה זהה. כל שלב באופטימיזציה נבדק כך:
  1. thread יחיד.
  2. פסים סטטיים.
  3. פסים דינמיים.
  4. palette מורחבת.
  5. shifts ו-`/GS-`.
- **מונה פיקסלים:** מספר הפיקסלים המוצללים זהה בדיוק בין v1 ל-v2: ‏2,428,867,349.
- **עמידה בחומרה:** ‏`vfpu-selftest` ‏66/66 אחרי השינויים, וכל ארבעת ה-builds עוברים קומפילציה.

## ביצועים

![זמני פריים](../media/screenshots/2026-10-16_06_perf_frame_times_v1_v2.png)

| גרסה | התפריט: ms לפריים | FPS בתפריט | boot עד פריים 1343 |
|---|---|---|---|
| v1: ‏thread יחיד | 276.7 | 3.6 | ‏140.2 שניות |
| v2: ‏20 threads, בזמן הקלטה | 41.3 | 24.2 | ‏22.1 שניות |
| v2: ‏20 threads, בלי הקלטה | 36.2 | 27.6 | — |

מה עולה בתפריט:
- **עשן ואש:** ‏77 quads גדולים עם blending וטקסטורת CLUT8 בגודל 64×64, שמכסים כ-2.9 מיליון פיקסלים בכל פריים.
- **סה"כ:** כ-4.3 מיליון פיקסלים לפריים.
- ב-PSP זה עניין רגיל.
- בתוכנה על המעבד זה העומס העיקרי: לפי ה-profiler, ‏`put_pixel` ‏33%, ‏`texture` ‏25% ו-`raster_tri` ‏14%.

סרטונים ב-`media/videos/`:
- `2026-10-16_coo_boot_to_menu_v1_gameclock.mp4`: לפי שעון המשחק. ניגון במהירות האמיתית של המשחק.
- `2026-10-16_coo_boot_to_menu_v1_hostclock_1thread.mp4`
- `2026-10-16_coo_boot_to_menu_v2_hostclock_20threads.mp4`

  השניים האלה לפי שעון המחשב, כך שרואים כמה חלקה הריצה באמת.
- `2026-10-16_coo_perf_v1_vs_v2_hostclock.mp4`: השוואה זו לצד זו, עם מונה FPS חי (`tools/perf_report.py`).

## מה עוד חסר

- **sceMpeg:** ‏(סרטון הפתיחה), ‏ATRAC3+ ו-SAS (סאונד).
- **קלט:** אין עדיין.
- **חלון:** אין עדיין. הפריימים נשמרים לקבצים בלבד.
- **ב-renderer עוד לא ממומשים:**
  - dithering.
  - patches מסוג Bezier ו-spline, שלא בשימוש ב-CoO.
  - bounding box test. כרגע הוא תמיד מחזיר "גלוי".
- **ביצועים:** הצעדים הבאים האפשריים:
  - לולאות פיקסל מותאמות למצבים הנפוצים.
  - פחות flushes, באמצעות בדיקת חפיפה בין טקסטורה ל-render target.
  - חפיפה בין קוד המשחק לציור.
  - renderer חומרה (D3D11/Vulkan) ל-port הסופי.
- **Ghost of Sparta:** עדיין נתקע בפענוח PGD, לפני ציור כלשהו.
