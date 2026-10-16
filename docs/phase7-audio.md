# שלב 7: סאונד (sceAudio, ‏SAS ו-ATRAC3+)

**תאריך:** 2026-10-16 · **branch:** `phase2/threads-and-fs` · **קוד:** `runtime/rt_audio.c`, `runtime/rt_sas.c`, `runtime/rt_atrac.c`, `runtime/rt_at3dec.cpp`, `runtime/at3shim/`, החיבור ב-`runtime/rt_host.c`, `CMakeLists.txt`, `tools/audio_report.py`

## השורה התחתונה

- **ל-Chains of Olympus יש סאונד.**
  - **מוזיקה:** מוזיקת התפריט (`MAINMENU.AT3`), הקטע של תחילת המשחק (`GAMESTRT.AT3`) ומוזיקת השלב הראשון (`PERCOM2L.AT3`) מתנגנות דרך sceAtrac3plus.
  - **אפקטים:** הקרב והסביבה מתנגנים דרך sceSasCore. בריצה של 85 שניות, ‏58 מהן בשלב הראשון, היו 1,098 key-ons של קולות.
- **ה-decoder של ATRAC3+ מדויק:**
  - הפענוח זהה לזה של FFmpeg עד 1–2 LSB ‏(83–85 dB).
  - הוא מיושר לדגימה הראשונה של השיר.
  - הלולאה של מוזיקת התפריט חלקה עד רמת הדגימה הבודדת.
- **בחלון** (`--window --renderer gpu`):
  - הסאונד מתנגן בזמן אמת דרך SDL3, עם תור של ‏~51 ms בהתקן.
  - המשחק נשאר ב-60 FPS: ממוצע 59.97, ‏p99 של 18.7 ms.
  - אין אף underrun ב-85 שניות.
  - ה-WAV של ריצת החלון זהה בית-לבית לזה של ריצת headless.
- **headless נשאר דטרמיניסטי:** שתי ריצות זהות של 4,000 פריימים נתנו קובץ WAV זהה ופריימים זהים (SHA-256).
- **`--wav out.wav`** כותב את כל המיקס בזמן המשחק. כך אפשר לבדוק סאונד בלי לשמוע אותו, ולצרף אותו להקלטת `--record`.

![קרב בחלון, עם סאונד](../media/screenshots/2026-10-16_21_window_combat_with_sound.png)

![המיקס של ריצת התסריט: מוזיקת התפריט, תחילת המשחק, השלב והקרב](../media/screenshots/2026-10-16_22_audio_spectrogram_gameplay.png)

## איך המשחק משמיע סאונד

המשחק משתמש במנוע הסאונד של Sony, ‏Scream (‏`snd_stream v1.10.1`). הוא טוען את ה-PRX של Sony ‏(`sc_sascore`, ‏`libatrac3plus`, ‏`audiocodec`) דרך `sceKernelLoadModule`, וה-runtime מספק את הפונקציות שלהם ב-HLE.

| מה | איך |
|---|---|
| ערוצים | באתחול שומרים ארבעה ערוצי sceAudio: ‏7 ‏(448 דגימות), ו-6, 5, 4 ‏(256). |
| אפקטים | ה-thread ‏`snd_tick_timer_thread` מעדכן קולות (`SetVoice`, ‏pitch, ווליום, ‏ADSR, ‏KeyOn/KeyOff). הוא קורא ל-`__sceSasCore` שמונה פעמים, ב-grain של 256 דגימות, ושולח את 2,048 הדגימות ב-`sceAudioOutputPannedBlocking`: לערוץ 4 בתפריט ולערוץ 6 בשלב. |
| reverb | ‏`__sceSasRevType(-1)` ו-`__sceSasRevVON(1, 0)`: רק הצליל היבש. עד השלב הראשון המשחק לא משתמש ב-reverb. |
| מוזיקה | ה-thread ‏`snd_stream_service_thread_0` קורא 160 KB מקובץ ה-AT3 ל-buffer, קורא ל-`sceAtracSetDataAndGetID` (מצב streaming) ול-`sceAtracSetLoopNum(0x7FFFFFFF)`. אחר כך הוא חוזר על: `sceAtracDecodeData`, ‏`GetStreamDataInfo`, קריאה מהקובץ ו-`AddStreamData`. את הדגימות הוא אוסף ב-ring משלו ומשמיע בבלוקים של 2,048 בערוץ 5, עם fade-in בווליום (`0x0AF4` עד `0x423C`). |

לפני השלב הזה כל הפונקציות של SAS ו-ATRAC החזירו 0. המשחק המשיך לרוץ בשקט: אחרי `sceAtracGetSecondBufferInfo` הוא שחרר את ה-ATRAC מיד, וה-mix של SAS לא כתב כלום.

## sceAudio: ערוצים על השעון הווירטואלי (`rt_audio.c`)

ב-PSP, שמונת ערוצי sceAudio וערוץ Output2 מעורבבים בחומרה ב-44.1 kHz.

**המודל:**
- לכל ערוץ נשמר המיקום, ביחידות של דגימות 44.1 kHz של זמן וירטואלי, שבו נגמרים הנתונים שבתור.
- קריאת output מערבבת את הבלוק ל-ring buffer. הבלוק מתחיל ב-max(עכשיו, סוף התור של הערוץ).
- **קריאה חוסמת:** אם יש עוד נתונים בתור, היא ממתינה עד שהם מתנגנים, כלומר עד שהבלוק שלה מתחיל. זה מה שהחומרה עושה, וזה מה ש-PPSSPP עושה ב-`__AudioEnqueue`. המשחק נשאר בלוק אחד לפני "החומרה".
- **קריאה לא חוסמת** (`sceAudioOutputPanned`) מחזירה `CHANNEL_BUSY` כשיש עוד נתונים בתור.
- **ווליום:** ‏`0x8000` הוא 1.0. בערוצים הרגילים מותר עד `0xFFFF`, וב-Output2 עד `0xFFFFF`.
- **ערוץ mono** (`0x10`) משוכפל לשני הצדדים.
- **הקצאת ערוץ פנוי** (`sceAudioChReserve(-1)`) מתחילה מהערוץ הגבוה ולעולם לא נותנת את 0, כמו ב-PPSSPP.

**הפלט:** דגימות שלפני הזמן הווירטואלי הנוכחי כבר לא ישתנו. ‏`rt_audio_advance` ממיר אותן ל-16 ביט (עם clamp) ושולח אותן ליעדים:
- קובץ WAV (`--wav`).
- בחלון, stream של SDL3.

**דטרמיניזם:** כל התזמון מגיע מהשעון הווירטואלי. שום דבר בצד המחשב, לא התקן הסאונד ולא מצב התור שלו, לא חוזר למשחק. לכן ריצת headless נשארת דטרמיניסטית, וה-WAV שלה זהה בין ריצות.

**הפריימים השתנו לעומת v3.** ה-threads של הסאונד ממתינים עכשיו כמו בחומרה:
- sceAudio חוסם עד שהבלוק הקודם התנגן.
- SAS ו-ATRAC לוקחים זמן, כמו ב-PPSSPP.

לכן התזמון של המשחק השתנה. בתפריט זו אותה סצנה. במשחק, המצב בפריים נתון שונה: למשל, בפריים 4,000 מופיע עכשיו מסך ה-tutorial על ה-red orbs. פריימי הייחוס להשוואת renderers עודכנו: ‏`build/rec_v7` מחליף את `build/rec_v3`. הוא כולל renderer תוכנה, פריים כל 200, ‏`perf.csv` ו-`mix.wav`.

### בחלון: סנכרון לזמן אמת

בחלון, הזמן הווירטואלי צמוד לשעון האמיתי (`rt_host_idle`, ‏[שלב 5](phase5-window-input.md)), ולכן ה-stream מקבל סאונד בקצב שבו הוא מנגן אותו.

1. **לפני השינה:** כשכל ה-threads ממתינים עד זמן `next`, אף אחד לא יכול להוסיף סאונד לפני `next`. לכן הסאונד עד `next` נשלח להתקן לפני שה-runtime ישן עד שהשעון האמיתי מגיע לשם.
2. **אחרי השינה:** ‏`rt_audio_host_sync` מודד כמה סאונד יש בתור של ההתקן. זו נקודת המינימום, ובמצב יציב היא שווה ל-latency.
   - **מתחת ל-10 ms** (המחשב פיגר, למשל בטעינה): התור מתמלא בשקט עד 50 ms. בלי זה כל בלוק היה מגיע בדיוק בזמן, והסאונד היה מקרטע.
   - **drift:** בקר איטי מכוון את קצב הנגינה של ה-stream (`SDL_SetAudioStreamFrequencyRatio`, לכל היותר ±0.5%). כך הוא סופג את ההפרש בין השעון של התקן הסאונד לשעון של המחשב, והתור נשאר סביב 50 ms לאורך זמן.

**התוצאה:** ריצה של 5,000 פריימים בחלון, עם תסריט הקלט ו-`--renderer gpu`.

| מדד | ערך |
|---|---|
| זמני פריים במשחק (זמן וירטואלי מעל 30 s) | ממוצע 59.97 FPS, ‏p50 ‏16.7 ms, ‏p99 ‏18.7 ms, ‏max ‏24.4 ms |
| underruns בהתקן | 0 |
| גודל התור הממוצע | 51 ms |
| קצב הנגינה בסוף | 1.0001, כמעט בלי תיקון |
| פריימים מעל 25 ms | 6: בהתחלה, במעברי התפריט ובטעינת השלב (פריים אחד של 1.25 s) |

**דטרמיניזם גם בחלון:** ה-WAV של ריצת החלון זהה בית-לבית ל-WAV של ריצת headless עם אותו קלט, גם עם ה-renderer התוכנה וגם עם ה-GPU. הסנכרון לזמן אמת וה-renderer לא משפיעים על המשחק.

![סיכום ריצת החלון](../media/screenshots/2026-10-16_23_window_audio_summary.png)

## ATRAC3+ ‏(`rt_atrac.c`, ‏`rt_at3dec.cpp`)

### בחירת ה-decoder

| | FFmpeg ‏(libavcodec) | ‏`at3_standalone` של PPSSPP |
|---|---|---|
| קוד | כל libavcodec, או build מותאם שלו | ‏~8,400 שורות, כולל הטבלאות, בלי תלויות |
| מקור ה-decoder | ‏`atrac3plus.c` של FFmpeg | אותו decoder, מחולץ מ-FFmpeg כדי שייבנה לבד |
| בנייה | build של FFmpeg או DLLs חיצוניים | עוד static library ב-CMake |
| רישיון | ‏LGPL-2.1+ | ‏LGPL-2.1+ (הקבצים שומרים את כותרות FFmpeg) |

**הבחירה: ‏`at3_standalone`.** זה אותו decoder של FFmpeg, בלי כל השאר, ו-PPSSPP משתמש בו עם המשחקים האלה כל יום.
- **הורדה:** הקבצים לא נכנסים לגיט. CMake מוריד אותם מ-tag ‏`v1.20.4` של PPSSPP ובודק כל קובץ מול SHA-256, כמו ש-SDL3 ו-libchdr מגיעים דרך FetchContent.
- **בלי שינויים:** הקבצים נבנים כמו שהם. התיקייה `runtime/at3shim/` מספקת במקום PPSSPP את שלוש הכותרות שהם מבקשים: לוג, הקצאה מיושרת ובדיקות ארכיטקטורה.
- **ממשק C:** ‏`rt_at3dec.cpp` עוטף את ה-decoder בממשק C, כמו `Atrac3Audio` של PPSSPP: המרה מ-float ל-16 ביט, ושכפול mono לסטריאו.
- **ATRAC3 (לא פלוס):** נבנה גם הוא. ל-sceAtrac יש מזהים לשני ה-codecs, אבל כל המוזיקה של CoO היא ATRAC3+ ‏(96 kbps, frames של 560 בתים).

### מכונת המצבים

ה-API של sceAtrac הוא בעיקר ניהול buffer שהמשחק ממלא בעצמו: כמה נתונים יש, מאיפה לקרוא את ההמשך, ואיך לחזור לנקודת הלולאה. ‏`rt_atrac.c` הוא port ל-C של `Atrac2` של PPSSPP ‏(`AtracCtx2.cpp`, v1.20.4), שנכתב מול בדיקות על חומרה אמיתית:
- **מצבי ה-buffer:** כל הקובץ בזיכרון, חצי buffer, ו-streaming בלי לולאה, עם לולאה בסוף או עם trailer ו-buffer שני.
- **frames לדילוג:** אחרי `SetData`, אחרי seek ובכל לולאה מפענחים frame אחד או שניים ומשליכים אותם, כדי ש-decoder יגיע למצב הנכון.
- **ניהול ה-streaming:** ‏`GetStreamDataInfo`, ‏`AddStreamData` ו-`GetRemainFrame`, כולל עטיפה בסוף ה-buffer ובנקודת הלולאה.
- **seek:** ‏`GetBufferInfoForResetting` ו-`ResetPlayPosition`.
- **השהיות:** כמו ב-HLE של PPSSPP. פענוח frame לוקח ל-thread הקורא 2.3 ms של זמן וירטואלי, ו-`SetData` לוקח 0.1 ms.

**הבדל מכוון אחד מ-PPSSPP:** ה-frame הראשון אחרי התחלה, seek או לולאה מתחיל באמצע frame. ‏PPSSPP מחזיר את *תחילת* ה-frame המפוענח. כאן מחזירים את הדגימות *ממיקום הפענוח והלאה*. הבדיקה מול FFmpeg בהמשך מראה שכך הנגינה רציפה: בהתחלה היא מתחילה בדגימה הנכונה, ובלולאה אין קפיצה.

### אימות מול FFmpeg

‏`--extract` מעתיק קובץ מהדיסק. ‏FFmpeg מפענח אותו, ומשווים לסאונד שהמשחק שולח לערוץ המוזיקה:

| קובץ | התאמה | שגיאה מקסימלית | יישור |
|---|---|---|---|
| ‏`MAINMENU.AT3` | ‏83–85 dB | ‏1–2 LSB | הדגימה הראשונה שלנו היא דגימה 2,710 של FFmpeg: ה-delay של ה-encoder (‏`0x926`) ועוד 368 של ה-codec |
| ‏`PERCOM2L.AT3` | ‏84–86 dB | ‏1–2 LSB | רציף לאורך כל הקטע |

**הלולאה:** בתפריט (בלי קלט, 65 שניות) המוזיקה עוברת את סוף השיר (37.5 שניות). ההיסט בין הפלט שלנו ל-FFmpeg גדל בדיוק ב-1,654,122 דגימות, שזה בדיוק אורך השיר. כלומר הלולאה חלקה עד הדגימה הבודדת.

ההבדל של 1–2 LSB הוא ההמרה מ-float: ‏PPSSPP כופל ב-32767 וחותך, ו-FFmpeg כופל ב-32768 ומעגל.

## SAS ‏(`rt_sas.c`)

‏`__sceSasCore` מערבב grain של דגימות לבאפר סטריאו שהמשחק משמיע דרך sceAudio. ‏`rt_sas.c` הוא port של `SasAudio.cpp`, ‏`SasReverb.cpp` ו-`sceSas.cpp` של PPSSPP:
- **VAG** (ה-ADPCM של Sony): בלוקים של 16 בתים ל-28 דגימות, חמישה פילטרים ועוד הערכים שנמדדו על חומרה, ודגלי לולאה וסוף.
- **ADSR:** ארבעת השלבים, שש העקומות (לינארית, עם "שבירה", אקספוננציאלית ו-direct), ופענוח `SetSimpleADSR`. אחרי key-on הקול שותק 32 דגימות.
- **pitch:** ‏`0x1000` הוא 1.0. ה-resampling לינארי עם שתי דגימות היסטוריה.
- **ווליום:** שמאל וימין, ו-send ל-reverb.
- **זמן:** כל mix לוקח ל-thread הקורא 20 µs, ועוד 68 µs לכל קול פעיל ועוד 0.6 µs לכל דגימה ב-grain, עד 1.2 ms (הנוסחה של PPSSPP).

**שני הבדלים מכוונים מ-PPSSPP:**
- **רעש** (`__sceSasSetNoise`): ב-PPSSPP קולות רעש שותקים. כאן הם מקבלים את מחולל הרעש של ה-SPU של ה-PSX: ‏LFSR של 16 ביט. התדר בן 6 הביטים מתפרק ל-shift ול-step של ה-SPU.
- **reverb:** האלגוריתם והפריסטים של ה-SPU של ה-PSX, ב-22.05 kHz. ‏PPSSPP כותב כל דגימה שנייה ב-44.1 kHz ומשאיר אפס בין לבין. כאן הפערים מתמלאים באינטרפולציה, באותה רמה, בלי ה-image ליד 22 kHz. המשחק כרגע לא מפעיל reverb, אבל הוא ממומש.

**אימות:** שמונה קטעי VAG מבנקי הסאונד שהמשחק טוען נלקחו מהקריאות ל-`__sceSasSetVoice`. הם נעטפו בכותרת VAG ופוענחו ב-FFmpeg ‏(`adpcm_psx`). ההתאמה ‏58–64 dB. ההפרש הוא עיגול: ה-SPU, ו-PPSSPP, מזיזים ב-`>>6`, ו-FFmpeg מחלק ב-64.

## ביצועים

- **פרופיילר:** בריצה של 1,500 פריימים עם ה-renderer התוכנה, אף פונקציה של סאונד לא מגיעה ל-0.1% מהדגימות. ה-mix של SAS, הפענוח של ATRAC3+ וה-ring של sceAudio זניחים.
- **GPU renderer, headless:** ‏5,000 פריימים ב-129–135 FPS.
- **thread switches:** ההשהיות של SAS ו-ATRAC מוסיפות כאלה: 26 אלף במקום 8.6 אלף ב-5,000 פריימים.

## רישוי

- **‏`rt_sas.c` ו-`rt_atrac.c`:** הם port של הקוד של PPSSPP ‏(GPL-2.0+), ו-`rt_audio.c` עוקב אחרי ההתנהגות שלו. זה תואם להחלטה ב-[דוח ההיתכנות](feasibility-static-recomp.md): ה-runtime תחת GPL.
- **ה-decoder:** ‏LGPL-2.1+, תואם ל-GPL. הוא לא נכנס לגיט, ו-CMake מוריד אותו.

## הרצה

```bash
build/cmake_full/Release/gow_rt_UCUS98653_full.exe --image build/gen/UCUS98653_full/image.bin --boot --disc "God of War - Chains of Olympus (USA).chd" --ms0 build/ms0 --window --renderer gpu
```

```bash
build/cmake_full/Release/gow_rt_UCUS98653_full.exe --image build/gen/UCUS98653_full/image.bin --boot --disc "God of War - Chains of Olympus (USA).chd" --ms0 build/ms0 --input "$(cat build/rec_v7/input.txt)" --max-frames 5000 --renderer gpu --wav build/audio/run.wav
```

| אפשרות | מה היא עושה |
|---|---|
| (עם `--window`) | סאונד בהתקן ברירת המחדל, ‏~50 ms latency |
| ‏`--mute` | בלי סאונד בחלון |
| ‏`--wav out.wav` | כל המיקס, בזמן המשחק, מזמן וירטואלי 0 |
| ‏`--disc game.chd --extract disc0:/path out` | מעתיק קובץ מהדיסק (לבדיקות מול FFmpeg) |

בסוף כל ריצה יש סיכום: כמה שניות עורבבו, כמה דגימות נחתכו (clip), כמה בלוקים היו בכל ערוץ, כמה grains של SAS ו-key-ons, כמה frames של ATRAC פוענחו, ובחלון גם underruns, גודל התור וקצב הנגינה.

הבדיקות נעשו עם `tools/audio_report.py`:

| פקודה | מה היא עושה |
|---|---|
| ‏`levels run.wav` | RMS ו-peak לכל שנייה: איפה יש סאונד |
| ‏`match ours.wav ref.wav --at SEC` | מוצאת איפה קטע מהפלט שלנו נמצא בפענוח ייחוס, ומחזירה היסט, gain והתאמה ב-dB |
| ‏`picture run.wav --png out.png --mark SEC:label` | ספקטרוגרמה, פס עוצמה ותוויות על ציר הזמן |

```bash
.venv/Scripts/python -I tools/audio_report.py match build/audio/menu_loop.wav build/audio/MAINMENU_ffmpeg.wav --at 52.6
```

## מה נשאר

- **הסאונד של הסרטונים:** ‏sceMpeg עדיין בלי decoder, כך שהסרטונים מדולגים בלי תמונה ובלי סאונד.
- **מצבים שלא נבדקו:** המסלולים של ATRAC עם buffer שני, חצי buffer ו-seek ממומשים לפי PPSSPP, אבל CoO לא משתמש בהם עד השלב הראשון.
- **‏Ghost of Sparta:** טוען את מודולי הסאונד דרך `sceUtilityLoadModule`. עוד לא נבדק עם סאונד.
