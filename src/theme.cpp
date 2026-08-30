#include "theme.h"

namespace vp::theme {

namespace {
HFONT make(int height, int weight, const wchar_t* face) {
    return CreateFontW(height, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       VARIABLE_PITCH, face);
}
}  // namespace

// The sizes of the version this replaces: 12 px everywhere, 11 px for the
// faintest line, 15 px semibold for a title, Consolas for figures.
void Fonts::create() {
    ui = make(-13, FW_NORMAL, L"Segoe UI");
    sub = make(-12, FW_NORMAL, L"Segoe UI");
    tiny = make(-11, FW_NORMAL, L"Segoe UI");
    semi = make(-13, FW_SEMIBOLD, L"Segoe UI");
    bold = make(-15, FW_SEMIBOLD, L"Segoe UI");
    mono = make(-12, FW_NORMAL, L"Consolas");
    icon = make(-15, FW_NORMAL, L"Segoe MDL2 Assets");
    osd = make(-20, FW_SEMIBOLD, L"Segoe UI");
}

void Fonts::destroy() {
    for (HFONT* f : {&ui, &sub, &tiny, &semi, &bold, &mono, &icon, &osd}) {
        if (*f) { DeleteObject(*f); *f = nullptr; }
    }
}

}  // namespace vp::theme
