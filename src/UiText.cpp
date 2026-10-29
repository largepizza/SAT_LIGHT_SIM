#include "UiText.h"

namespace
{
const char *const kTextEn[] = {
#define UI_TEXT(id, s) s,
#include "text/ui_text_en.inc"
#undef UI_TEXT
};
static_assert(sizeof(kTextEn) / sizeof(kTextEn[0]) == kTxtCount, "ui_text_en.inc: one string per id");

const char *const kTextNames[] = {
#define UI_TEXT(id, s) #id,
#include "text/ui_text_en.inc"
#undef UI_TEXT
};
} // namespace

const char *tx(Txt id)
{
    const size_t i = (size_t)id;
    return i < kTxtCount ? kTextEn[i] : "";
}

const char *txName(Txt id)
{
    const size_t i = (size_t)id;
    return i < kTxtCount ? kTextNames[i] : "";
}
