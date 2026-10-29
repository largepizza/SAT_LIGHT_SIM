#pragma once
// ─── UI text table ──────────────────────────────────────────────────────────
// Every user-facing string the app shows lives in src/text/ui_text_en.inc, one line per string:
//
//     UI_TEXT(BtnTrack, "Track")
//     UI_TEXT(ToastBookmarkSaved, "Bookmark saved: %s")
//
// Code asks for a string by its id: tx(Txt::BtnTrack). Formatted strings keep their printf placeholders in the
// table and are formatted by the caller (snprintf(buf, n, tx(Txt::X), ...) or uiKitStr(tx(Txt::X), ...)), so a
// translation may move the words around a placeholder but must keep the placeholders and their order.
//
// tx() returns a pointer to static storage that never moves, so it is safe to hand straight to Clay (which keeps
// the pointer until the frame is recorded) and to static tables. There is one language (English) for now; a later
// pass adds a table per language with the same ids (a missing entry falls back to English) and a setting to pick it.
// See docs/UI_TEXT.md for the rules (where text goes, ids, length limits, search).
#include <cstddef>
#include <cstdint>

enum class Txt : uint16_t
{
#define UI_TEXT(id, s) id,
#include "text/ui_text_en.inc"
#undef UI_TEXT
    Count
};

// The string for id in the current language.
const char *tx(Txt id);

// Number of entries (for tools / tests).
constexpr size_t kTxtCount = (size_t)Txt::Count;

// The id's name ("BtnTrack"), for logs and the harness.
const char *txName(Txt id);
