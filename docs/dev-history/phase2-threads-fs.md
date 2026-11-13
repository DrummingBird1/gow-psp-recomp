# שלב 2: מתזמן threads ומערכת קבצים

**תאריך:** 2026-10-16 · **branch:** `phase2/threads-and-fs` · **קוד:** `runtime/`

## השורה התחתונה

שני המשחקים עוברים עכשיו את האתחול שלהם עם threads אמיתיים ועם הדיסק עצמו (CHD):
- `module_start` רץ ב-thread משלו ויוצר את `user_main`.
- נוצר ה-heap של המנוע (`radheap`).
- המשחק סורק את כל התיקיות בדיסק.
- `DiscSpinnerThread` פותח את ארכיון הנתונים הראשי לפי מספר סקטור.
- נוצר `PowerControlThread`, ואז נקבע ה-framebuffer הראשון.

הריצה נעצרת בפונקציה הראשונה שמשתמשת ב-**VFPU**: ‏`0x0881920C` ב-CoO ו-`0x08819F18` ב-GoS. זו כנראה אותה פונקציה של המנוע בשני המשחקים. תרגום ה-VFPU הוא החסם הבא.

| | Chains of Olympus | Ghost of Sparta |
|---|---|---|
| דיסק | CHD, ‏1,531MB | CHD, ‏1,667MB |
| heap של המנוע (`radheap`, FPL) | ‏0x138F400 (19.6MB) ב-`08B70C00` | ‏0x12A0C00 (18.6MB) ב-`08C5F400` |
| threads שנוצרו | module_start, user_main, ExitCallbackThread, DiscSpinnerThread, PowerControlThread | זהה |
| ארכיון שהספינר פותח | `sce_lbn0xb55c0_size0x5062000` (84MB) | `sce_lbn0xb8e20_size0x219e000` (35MB) |
| framebuffer ראשון | `04000000`, רוחב 512, פורמט 8888 | זהה |
| נעצר ב | פונקציית VFPU ‏`0881920C` | פונקציית VFPU ‏`08819F18` |

## מה נבנה

### מתזמן threads (`rt_thread.c`, `rt_fiber.c`)
- **fiber לכל thread:** כל thread של ה-PSP רץ על fiber משלו (Windows Fibers, או `ucontext` ב-POSIX), עם מחסנית C של 16MB שמוקצית וירטואלית. קריאה חוסמת עוברת ל-fiber של המתזמן.
- **סמנטיקה של PSP:**
  - רץ ה-thread המוכן עם העדיפות המספרית הנמוכה ביותר, ובעדיפות שווה לפי סדר FIFO.
  - thread שמעיר thread בעדיפות גבוהה יותר מוותר מיד על המעבד, וחוזר ראשון מבין בני העדיפות שלו.
  - `SuspendDispatchThread` חוסם החלפות.
- **זמן וירטואלי:** הזמן נמדד במיקרו-שניות ומתקדם רק כשכל ה-threads ממתינים, וקופץ ישר להתעוררות הבאה. בזכות זה ריצות דטרמיניסטיות, וניתן לשחזר אותן.
- **אובייקטים:**
  - threads, כולל create/start/exit, terminate/delete, wait-end, refer-status ו-priority.
  - semaphores, ‏event flags (AND/OR, CLEAR/CLEARALL, MULTI) ו-FPL.
  - callbacks: אפשר ליצור אותם, אבל הם עוד לא נקראים.
  - delay ו-sleep, ושעון מערכת.
- **זיכרון:** allocator מסוג first-fit על ה-user partition, עם הקצאה מלמטה, מלמעלה או עם alignment. כולל VolatileMem של 4MB ב-`0x08400000`.

### מערכת קבצים (`rt_io.c`)
- **הדיסק:** CHD נקרא דרך **libchdr**, עם FetchContent שננעל על commit מסוים (BSD-3). אפשר גם קובץ ISO רגיל. הדיסקים של המשחקים הם CHD v5 (zstd/zlib/huff/flac) עם hunks של 2048 בייטים.
- **ISO 9660:** חיפוש נתיבים בלי תלות באותיות גדולות או קטנות, ורשימות תיקייה.
- **התקנים:**
  - `disc0:`/`umd0:` קוראים מהדיסק, כולל נתיבי `sce_lbn0x…_size0x…` שהמשחקים משתמשים בהם כדי לקרוא ישירות לפי סקטור.
  - `ms0:` ממופה לתיקייה מקומית (`--ms0`).
  - יש תמיכה בנתיב העבודה (`sceIoChdir`) ובנתיבים יחסיים.
- **`IoFileMgrForUser`:** ‏open, read, write, lseek (64-bit), close, getstat, dopen/dread/dclose, mkdir ו-remove. הגרסאות האסינכרוניות (OpenAsync, ReadAsync, LseekAsync, PollAsync, WaitAsync) מסתיימות מיד, וכמו ב-PPSSPP, ‏OpenAsync מחזיר fd גם כשהקובץ חסר.

### שכבת HLE (`rt_hle.c`)
- **dispatcher:** מתרגם NID לפונקציה לפי שם, ושומר את התוצאה ב-cache. כל פונקציה לא ממומשת נרשמת בלוג בפעם הראשונה שקוראים לה.
- **display:** ‏`WaitVblankStart` ממתין בזמן הווירטואלי עד ה-vblank הבא (59.94Hz).
- **GE:** רשימות התצוגה מתקבלות ונספרות, אבל עוד לא מצויר כלום.
- **שאר המודולים:**
  - בקר: אין לחיצות.
  - אודיו: פלט חוסם שמקדם את הזמן לפי מספר הדגימות.
  - utility: חלון הודעה מסתיים מיד, ושמירה מחזירה "אין נתונים".
  - power, ‏RTC, ‏impose ו-modules: מימוש מינימלי.

## בדיקת רגרסיה

הבדיקה הדיפרנציאלית מול Unicorn רצה שוב אחרי שינוי ה-runtime (16 מקרים לכל אחד מ-300 שורשים):

| | CoO | GoS |
|---|---:|---:|
| התאמה מלאה | 1,671 | 1,837 |
| **אי-התאמות** | **0** | **0** |
| פונקציות שהתאימו בכל המקרים | 174 | 183 |

## איך מריצים

```bash
build/cmake_full/Release/gow_rt_UCUS98653_full.exe --image build/gen/UCUS98653_full/image.bin --boot --disc "God of War - Chains of Olympus (USA).chd" --ms0 build/ms0
```

אפשרויות:
- `--trace-hle`: כל קריאת HLE נרשמת בלוג.
- `--cpu-ms`: מגבלת זמן מעבד למתזמן.
- `--wall-ms`: watchdog לפי זמן אמיתי.
- `--max-hle`: מגבלה על מספר קריאות ה-HLE.

## מה עוד לא קיים

- **callbacks ופסיקות:** לא נקראים. אין vblank interrupt, ‏exit callback או UMD callback.
- **גרפיקה:** אין מפרש GE ואין ציור.
- **אודיו:** שקט. ‏SAS, ATRAC3+ ו-MPEG לא ממומשים.
- **שמירות:** לא נשמרות.
- **קבצים מוצפנים:** ‏`sceIoIoctl` מחזיר 0 בלבד. קבצי EDAT של GoS לא מפוענחים.
- **I/O אסינכרוני:** לא מדמה זמן קריאה.

## הצעדים הבאים

1. **תרגום VFPU:** זה החסם עכשיו. מדובר ב-81 ו-109 פונקציות, כ-1,200 ו-1,400 פקודות. המיפוי של הרגיסטרים והסמנטיקה מגיעים מ-PPSSPP.
2. **reference מבוסס PPSSPP** לבדיקה הדיפרנציאלית, כולל VFPU ופקודות Allegrex-only.
3. **מפרש GE ו-renderer.**
