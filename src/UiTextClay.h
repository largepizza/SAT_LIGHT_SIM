#pragma once
// tx() as a Clay_String (include after clay.h): CLAY_TEXT(txc(Txt::X), ...). The table's strings are static.
#include "UiText.h"
#include <cstring>

inline Clay_String txc(Txt id)
{
    const char *p = tx(id);
    return Clay_String{true, (int32_t)std::strlen(p), p};
}
