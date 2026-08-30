#pragma once
// The preferences window.
//
// Small on purpose: only the choices that change how the program behaves before
// anyone touches a control - what it opens with, what it computes on its own,
// and what it remembers. Everything else is already a control in the main
// window, and duplicating those here would only create two places to look.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace vp {

// Edits the shared settings in place. Returns true when something changed, in
// which case the caller saves and reacts.
bool runSettingsWindow(HWND owner, HINSTANCE instance);

}  // namespace vp
