#pragma once
// Language.
//
// The English text is the key. tr(L"Open\x2026") returns either a translation
// or, when there is none, the English it was handed - so the program is always
// readable, a missing line in a translation costs that one line and nothing
// else, and adding a language never means touching the source.
//
// A language is one UTF-8 text file in the `lang` folder beside the executable,
// named after its code: lang\cs.lang, lang\de.lang. Each line is
//
//     English text = translated text
//
// with `#` starting a comment and `\n` standing for a line break. The first two
// comments may name the language and its author:
//
//     # name: Cestina
//     # author: somebody
//
// Anyone can write one with a text editor. `lang\_template.lang` is rewritten
// every run with every line the program actually asked for, which is the list
// to start from.

#include <string>
#include <vector>

namespace vp {

struct LanguageInfo {
    std::string code;     // "en", "cs", the file's own name
    std::wstring name;    // what to show in the settings, from `# name:`
};

// Loads a language. "en" or an unknown code means the built-in English.
void loadLanguage(const std::string& code);
const std::string& currentLanguage();

// English first, then every lang\*.lang file that was found.
std::vector<LanguageInfo> availableLanguages();
std::wstring languageName(const std::string& code);

// The translation, or `english` itself. The returned pointer is stable until
// the next loadLanguage.
const wchar_t* tr(const wchar_t* english);

// Writes lang\_template.lang: every line the program has asked tr() for so far,
// each already filled in with the English, ready to be translated over.
void writeLanguageTemplate();

}  // namespace vp
