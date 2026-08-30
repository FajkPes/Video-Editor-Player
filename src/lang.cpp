#include "lang.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "settings.h"

namespace vp {

namespace {

// std::map rather than unordered_map on purpose: tr() hands out a pointer into
// the stored string, and a node-based container keeps that pointer good for as
// long as the entry lives, whatever else is inserted afterwards.
std::map<std::wstring, std::wstring> table;

// Every line the program has actually asked for. That is what a translator
// wants - the strings that exist, rather than whatever a search of the source
// happened to turn up.
std::set<std::wstring> asked;

std::string language = "en";

std::wstring folder() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path = exe;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) path.resize(slash + 1);
    return path + L"lang\\";
}

std::wstring trimmed(const std::wstring& s) {
    const size_t from = s.find_first_not_of(L" \t\r\n");
    if (from == std::wstring::npos) return {};
    const size_t to = s.find_last_not_of(L" \t\r\n");
    return s.substr(from, to - from + 1);
}

// A backslash-n in the file becomes a real line break, a doubled backslash a
// single one. Nothing else is escaped: the file is UTF-8, so every other
// character can simply be typed.
std::wstring unescape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        const wchar_t next = s[++i];
        if (next == L'n') out += L'\n';
        else if (next == L't') out += L'\t';
        else if (next == L'\\') out += L'\\';
        else { out += L'\\'; out += next; }
    }
    return out;
}

std::wstring escape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        if (c == L'\n') out += L"\\n";
        else if (c == L'\t') out += L"\\t";
        else if (c == L'\\') out += L"\\\\";
        else out += c;
    }
    return out;
}

std::vector<std::wstring> readLines(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream all;
    all << in.rdbuf();
    std::string text = all.str();

    // A byte-order mark is what a text editor on Windows leaves behind, so it
    // has to be stepped over rather than parsed as part of the first key.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }

    std::vector<std::wstring> lines;
    const std::wstring wide = fromUtf8(text);
    size_t at = 0;
    while (at <= wide.size()) {
        const size_t end = wide.find(L'\n', at);
        lines.push_back(wide.substr(at, end == std::wstring::npos ? end : end - at));
        if (end == std::wstring::npos) break;
        at = end + 1;
    }
    return lines;
}

}  // namespace

void loadLanguage(const std::string& code) {
    table.clear();
    language = code.empty() ? "en" : code;
    if (language == "en") return;

    const std::wstring path = folder() + fromUtf8(language) + L".lang";
    for (const std::wstring& raw : readLines(path)) {
        const std::wstring line = trimmed(raw);
        if (line.empty() || line[0] == L'#') continue;
        const size_t split = line.find(L'=');
        if (split == std::wstring::npos) continue;
        const std::wstring key = unescape(trimmed(line.substr(0, split)));
        const std::wstring value = unescape(trimmed(line.substr(split + 1)));
        // An empty right-hand side means "not translated yet", which is not the
        // same as "translate this to nothing".
        if (!key.empty() && !value.empty()) table[key] = value;
    }
}

const std::string& currentLanguage() { return language; }

std::wstring languageName(const std::string& code) {
    if (code == "en") return L"English";
    const std::wstring path = folder() + fromUtf8(code) + L".lang";
    for (const std::wstring& raw : readLines(path)) {
        const std::wstring line = trimmed(raw);
        if (line.empty()) continue;
        if (line[0] != L'#') break;            // past the header, stop looking
        const size_t at = line.find(L"name:");
        if (at != std::wstring::npos) return trimmed(line.substr(at + 5));
    }
    return fromUtf8(code);
}

std::vector<LanguageInfo> availableLanguages() {
    std::vector<LanguageInfo> found;
    found.push_back({"en", L"English"});

    WIN32_FIND_DATAW entry{};
    const std::wstring pattern = folder() + L"*.lang";
    HANDLE search = FindFirstFileW(pattern.c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) return found;
    do {
        std::wstring name = entry.cFileName;
        const size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos) name.resize(dot);
        // The template is written by the program, not meant to be chosen.
        if (name.empty() || name[0] == L'_' || name == L"en") continue;
        found.push_back({toUtf8(name), languageName(toUtf8(name))});
    } while (FindNextFileW(search, &entry));
    FindClose(search);

    std::sort(found.begin() + 1, found.end(),
              [](const LanguageInfo& a, const LanguageInfo& b) {
                  return a.code < b.code;
              });
    return found;
}

const wchar_t* tr(const wchar_t* english) {
    if (!english) return L"";
    const std::wstring key = english;
    asked.insert(key);
    if (table.empty()) return english;
    const auto found = table.find(key);
    return found == table.end() ? english : found->second.c_str();
}

void writeLanguageTemplate() {
    if (asked.empty()) return;
    const std::wstring dir = folder();
    CreateDirectoryW(dir.c_str(), nullptr);

    // tr() only sees what has actually been drawn, and a run where no file was
    // ever opened never draws the track rows or the editor. So the template is
    // merged rather than replaced: with the keys already in it, and with the
    // keys of every translation beside it, the list only ever grows and a
    // translator is not handed whichever third of the program was on screen.
    std::set<std::wstring> keys = asked;
    auto absorb = [&keys](const std::wstring& path) {
        for (const std::wstring& raw : readLines(path)) {
            const std::wstring line = trimmed(raw);
            if (line.empty() || line[0] == L'#') continue;
            const size_t split = line.find(L'=');
            if (split == std::wstring::npos) continue;
            const std::wstring key = unescape(trimmed(line.substr(0, split)));
            if (!key.empty()) keys.insert(key);
        }
    };
    absorb(dir + L"_template.lang");
    WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW((dir + L"*.lang").c_str(), &entry);
    if (search != INVALID_HANDLE_VALUE) {
        do { absorb(dir + entry.cFileName); } while (FindNextFileW(search, &entry));
        FindClose(search);
    }

    std::ofstream out(dir + L"_template.lang", std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << "\xEF\xBB\xBF";                     // so an editor opens it as UTF-8
    out << "# name: My language\n";
    out << "# author: \n";
    out << "#\n";
    out << "# Every line this program can show. Rename this file to the code of\n";
    out << "# your language (cs.lang, de.lang, pl.lang), put your own words on the\n";
    out << "# right of each equals sign, and it appears in Settings on the next\n";
    out << "# run. Leave a line out, or leave its right side empty, and the English\n";
    out << "# is used for that one line. Keep %d, %s and the like where they are.\n";
    out << "#\n";
    for (const std::wstring& key : keys) {
        const std::string line = toUtf8(escape(key));
        out << line << " = " << line << "\n";
    }
}

}  // namespace vp
