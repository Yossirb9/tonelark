# Tonelark (beta)

<div dir="rtl">

**תוכנת עריכה וניהול לתמונות RAW, חינמית ובקוד פתוח, עם ממשק בסגנון Lightroom.** היא פותחת את קטלוגי ה-Lightroom הקיימים שלך, ויש בה כלי AI שעובדים עם המנוי שכבר יש לך ל-Claude או ל-Codex. התוכנה מבוססת על המנוע של darktable ורצה על Windows 10/11 של 64 ביט.

[English below](#english)

</div>

![הספרייה עם פאנל ה-AI](docs/screenshots/library.jpg)
![מסך העריכה](docs/screenshots/develop.jpg)

<div dir="rtl">

## הורדה והתקנה

1. בעמוד [Releases](../../releases) מורידים את הגרסה האחרונה: המתקין (`…-win64-setup.exe`) או הגרסה הניידת (`…-win64-portable.zip`), שעובדת בלי התקנה.
2. המתקין עדיין לא חתום דיגיטלית, ולכן Windows יציג "Windows protected your PC". לוחצים **More info** ואז **Run anyway**.
3. אפשר להתקין רק למשתמש הנוכחי, בלי הרשאות מנהל.

זו גרסת בטא. היא נבדקה עד עכשיו על מחשב אחד, אז כדאי לשמור גיבוי של התמונות ושל הקטלוג. תקלות והצעות מדווחים בעמוד [Issues](../../issues).

## מה יש בה

- **Library ו-Develop בסגנון Lightroom:** פאנל Basic עם אותם סליידרים (Temp, ‏Tint, ‏Exposure עד Vibrance), ‏HSL, ‏Color Grading, ‏Tone Curve, מסכות (כולל מסכת AI לאובייקט), חיתוך, ריפוי, לפני/אחרי, Snapshots, ‏Presets וייצוא.
- **פתיחת קטלוגים של Lightroom Classic (.lrcat):**
  - **עובר:** תיקיות, כוכבים, Pick ו-Reject, תוויות צבע, מילות מפתח, Collections, ‏Virtual Copies, ‏Stacks, כותרות ו-GPS.
  - **עריכות שעוברות:** White Balance, הסליידרים של Basic, ‏Tone Curve, ‏HSL, ‏Color Grading, חיתוך, Vignette, ‏Grain, חידוד, הפחתת רעש, תיקון עדשה והסרת כתמים.
  - **לא עובר:** התאמות מקומיות (מברשות ומסננים) ופרופילים של Adobe.
  - הקטלוג נפתח לקריאה בלבד ולא משתנה.
- **כלי AI:**
  - **דירוג ומיון (Culling):** מציאת הצילום הטוב בכל רצף, בדיקת חדות, עיניים פתוחות וחיוכים. זה רץ על המחשב, בלי שירות חיצוני.
  - **Best Take:** בתמונה קבוצתית, כל אחד מקבל את הפנים הכי טובות שלו מתוך הרצף.
  - **עריכה במילים:** כותבים למשל "golden hour חם ורך", וה-AI מזיז את הסליידרים. התוצאה היא עריכה רגילה שאפשר לשנות או לבטל.
  - **התאמת מראה לסשן:** כל התמונות מקבלות את המראה של תמונת רפרנס, והחשיפה והאיזון הלבן מותאמים לכל תמונה.
  - **הצעות חיתוך:** בכל יחס (למשל 4:5 או 9:16), כולל יישור אופק.
  - **מילות מפתח, כותרות ותיאורים:** בכל שפה, גם בעברית.
  - **עבודה מתוך צ'אט (MCP):** צ'אט של Claude Code או Codex יכול לחפש, לראות, לדרג, לערוך ולייצא תמונות ב-Tonelark.

כלי ה-AI החיצוניים עובדים דרך Claude Code, ‏Codex או Gemini CLI המותקנים במחשב, עם המנוי שלך ובלי מפתח API. בלי כלים כאלה, כל שאר התוכנה עובדת כרגיל. בכלים האלה נשלחים עותקים מוקטנים של התמונות לשירות שבחרת, תחת החשבון שלך. המדריך המלא בעברית מותקן עם התוכנה, ונמצא בתפריט התחל.

</div>

<a name="english"></a>
## English

**A free, open-source raw photo editor and organizer for Windows with a Lightroom-style workflow.** It opens your Lightroom Classic catalogs and has AI tools that use the Claude or Codex subscription you already have. It is built on the darktable engine.

### Download

Get the installer or the portable zip from [Releases](../../releases). The installer is not code-signed yet: when Windows shows "Windows protected your PC", click **More info**, then **Run anyway**. This is a **beta**, tested on one computer so far: keep backups of your photos and catalog, and report problems in [Issues](../../issues).

### Features

- **Lightroom-style Library and Develop.**
  - Develop tools: the Basic panel with the same sliders, HSL, Color Grading, Tone Curve, masks (including an AI object mask), crop, healing and before/after.
  - Workflow: snapshots, presets and export.
- **Opens Lightroom Classic catalogs (.lrcat)**, read-only.
  - What transfers: folders, ratings, flags, color labels, keywords, collections, virtual copies, stacks and titles.
  - Develop settings: white balance and basic tone, tone curve, HSL, color grading, crop, vignette, grain, sharpening, noise reduction, lens corrections and spot removal.
  - Not converted: local adjustments and Adobe profiles.
- **AI tools.**
  - Culling of bursts (sharpness, eyes open, smiles) and a group-photo *Best Take*, both local, with no external service.
  - *Auto Edit* with words: the AI sets the sliders, and the result is a normal, undoable edit.
  - *Match Look* across a shoot, crop suggestions in any aspect ratio, and keywords and captions in any language.
  - An **MCP server**, so a Claude Code or Codex chat can find, view, rate, edit and export your photos.
  - The external tools run through Claude Code, Codex or Gemini CLI with your own subscription, and need no API key. They send reduced copies of your photos to the service you choose.

### Building

See [TONELARK.md](TONELARK.md). It is a regular darktable build (MSYS2 UCRT64, CMake, Ninja) plus the Windows packaging in `packaging/windows`.

### License and credits

Tonelark is free software under the [GNU GPL v3](LICENSE), a fork of [darktable](https://www.darktable.org) by the darktable developers (see [README.darktable.md](README.darktable.md) and [AUTHORS](AUTHORS)). The Windows package contains third-party libraries and models under their own licenses, listed in `THIRD_PARTY_NOTICES.txt` in the installation folder (generated by `tools/lightspeed/third_party.py`).

Tonelark is not affiliated with, endorsed by or sponsored by Adobe. Adobe and Lightroom are trademarks of Adobe Inc., used here only to describe compatibility.
