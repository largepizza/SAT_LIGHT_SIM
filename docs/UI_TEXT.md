# UI text: where it lives and how to add it

Every string the player reads (window titles, buttons, tooltips, settings labels and notes, toasts, the tutorial,
the boot chooser, card and panel text) belongs in one table: **`src/text/ui_text_en.inc`**, one line per string:

```cpp
UI_TEXT(BtnTrack, "Track")
UI_TEXT(FmtFollowDistKm, "%.2f km away")
```

Code asks for it by id:

```cpp
#include "UiText.h"                 // SatelliteSim.h already includes it
uiButton(inp, ui, "TrackBtn", 0, tx(Txt::BtnTrack), tx(Txt::TipTrack));
snprintf(buf, sizeof(buf), tx(Txt::FmtFollowDistKm), d / 1000.0);
const char *s = uiKitStr(tx(Txt::FmtSatCount), n);     // per-frame ring, safe for Clay
CLAY_TEXT(txc(Txt::NoteSkipTutorial), ...);            // UiTextClay.h: a Clay_String
```

There is one language (English) for now. The table exists so a later pass can add a language: a second `.inc` with
the same ids, a lookup that falls back to English, and a setting to pick it. Nothing else in the code should need to
change then.

## Rules

- **Ids** say what kind of text it is, then a few words of it: `Btn` button, `Tip` tooltip, `Lbl` a row's label,
  `Val` a value or a note in a readout row, `Sec` a section heading, `Ttl` a window title, `Opt` one choice of a
  list, `Sld` a slider's label, `Note` a free line of text, `Toast` a toast or banner, `Fmt` a printf format,
  `Tut` tutorial text. An id never changes when its text is reworded (search the code for the id, not the words).
  Identical text shares one id.
- **Formats** keep printf placeholders in the table. A translation may move words around a placeholder but must
  keep every placeholder in the same order (`%1$s` is not portable in MSVC's printf). Key names come in as `%s`
  from `keyDisplayName(...)`, never written into the text.
- **Pointers**: `tx()` returns static storage that never moves, so it can go straight to Clay, which keeps the
  pointer until the frame is recorded. Formatted text needs a buffer that outlives the frame's layout: `uiKitStr`
  (a per-frame ring), a member buffer, or the window's own string deque. Never a stack buffer.
- **Settings search** matches the label text the row is drawn with (`searchRow(id, label)`), so a label from the
  table is searchable as it is. A hand-built row must still call `searchRow` with its label.
- **Settings keys, harness commands, log lines and JSON keys are not UI text**: they stay in the code. So do
  keybinding action names (they are persisted in settings.json) and data (constellation, planet, category names).
- **Plain ASCII** (the font atlas is ASCII 32-126): no em dashes, curly quotes, degree signs or arrows. Use `-`,
  `'`, `deg`, `->` or words.
- **Length**: test at 1024x768 (`python tools/harness/ui_lowres.py run`). Buttons and card lines do not wrap;
  if a line does not fit, say less rather than shrink the font.

## Style

- Say what it does for the player, not how it is implemented ("Keeps it centred", not "aim lock on the follow
  basis"). Implementation detail belongs in the code comments and the wiki.
- No history or migration notes ("moved to ...", "was X before", "now ..."): the player only ever sees this build.
- One name per thing everywhere: Go to (not Follow), Track, Watch, Info, the Missions / Weather / Sky events
  windows, the Sun's Fury, Issue reports, Bookmarks, Cinematics.
- Sentence case for buttons, labels and tooltips; small caps (all upper case) for section headings only.
- Short labels; a tooltip says the rest in one or two sentences. No exclamation marks, no marketing.
- Units: SI with the HUD's unit choice where it applies; times in UTC as `YYYY-MM-DD HH:MM`.

## Tools

- `python tools/ui_text_migrate.py --report FILE...` counts the user-facing literals left at known UI call sites.
- `python tools/ui_text_migrate.py [--snprintf] FILE...` moves them into the table (review the diff: it knows call
  shapes, not meaning; it leaves keys, ids, file names and data alone).
- `python tools/ui_text_migrate.py --list FILE...` prints what it would move.
