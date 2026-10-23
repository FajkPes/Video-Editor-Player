#include "settings.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <algorithm>
#include <fstream>
#include <sstream>

#include "json.h"

namespace vp {

std::string toUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr,
                                         nullptr);
    if (need <= 1) return {};
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &out[0], need, nullptr, nullptr);
    out.resize(need - 1);
    return out;
}

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (need <= 1) return {};
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], need);
    out.resize(need - 1);
    return out;
}

std::wstring Settings::fileLocation() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path = exe;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) path.resize(slash + 1);
    return path + L"vp_settings.json";
}

namespace {

AudioPreset readAudioPreset(const json::Value& v) {
    AudioPreset p;
    p.name = v["name"].asString();
    p.normalise = v["normalise"].asBool();
    p.target = static_cast<float>(v["target"].asNumber(-14.0));
    for (const json::Value& t : v["tracks"].asArray()) {
        AudioPresetTrack track;
        track.gain = static_cast<float>(t["gain"].asNumber(1.0));
        track.eqLow = static_cast<float>(t["low"].asNumber());
        track.eqMid = static_cast<float>(t["mid"].asNumber());
        track.eqHigh = static_cast<float>(t["high"].asNumber());
        track.enabled = t["enabled"].asBool(true);
        track.normalise = t["normalise"].asBool(true);
        track.pitch = static_cast<float>(t["pitch"].asNumber(0.0));
        p.tracks.push_back(track);
    }
    return p;
}

json::Value writeAudioPreset(const AudioPreset& p) {
    json::Value v;
    v.set("name", p.name);
    v.set("normalise", p.normalise);
    v.set("target", p.target);
    json::Value tracks;
    for (const AudioPresetTrack& t : p.tracks) {
        json::Value item;
        item.set("gain", t.gain);
        item.set("low", t.eqLow);
        item.set("mid", t.eqMid);
        item.set("high", t.eqHigh);
        item.set("enabled", t.enabled);
        item.set("normalise", t.normalise);
        item.set("pitch", t.pitch);
        tracks.array().push_back(item);
    }
    v.set("tracks", tracks);
    return v;
}

VideoPreset readVideoPreset(const json::Value& v) {
    VideoPreset p;
    p.name = v["name"].asString();
    p.contrast = v["contrast"].asInt();
    p.brightness = v["brightness"].asInt();
    p.saturation = v["saturation"].asInt();
    p.gamma = v["gamma"].asInt();
    p.hue = v["hue"].asInt();
    p.sharpen = v["sharpen"].asInt();
    p.denoise = v["denoise"].asInt();
    return p;
}

json::Value writeVideoPreset(const VideoPreset& p) {
    json::Value v;
    v.set("name", p.name);
    v.set("contrast", p.contrast);
    v.set("brightness", p.brightness);
    v.set("saturation", p.saturation);
    v.set("gamma", p.gamma);
    v.set("hue", p.hue);
    v.set("sharpen", p.sharpen);
    v.set("denoise", p.denoise);
    return v;
}

RenderPreset readRenderPreset(const json::Value& v) {
    RenderPreset p;
    p.name = v["name"].asString();
    p.encoder = std::max(0, std::min(4, v["encoder"].asInt()));
    p.bitrate = v["bitrate"].asInt(30000);
    p.useQuality = v["useQuality"].asBool();
    p.quality = v["quality"].asInt(23);
    p.fps = v["fps"].asInt();
    p.speedPreset = v["speedPreset"].asInt(3);
    p.audioBitrate = v["audioBitrate"].asInt(192);
    p.speed = v["speed"].asNumber(1.0);
    p.keepPitch = v["keepPitch"].asBool(true);
    p.hardwareDecode = v["hardwareDecode"].asBool(true);
    p.halfSize = v["halfSize"].asBool();
    p.orientation = std::max(0, std::min(2, v["orientation"].asInt()));
    p.outWidth = std::max(0, std::min(16384, v["outWidth"].asInt()));
    p.outHeight = std::max(0, std::min(16384, v["outHeight"].asInt()));
    return p;
}

json::Value writeRenderPreset(const RenderPreset& p) {
    json::Value v;
    v.set("name", p.name);
    v.set("encoder", p.encoder);
    v.set("bitrate", p.bitrate);
    v.set("useQuality", p.useQuality);
    v.set("quality", p.quality);
    v.set("fps", p.fps);
    v.set("speedPreset", p.speedPreset);
    v.set("audioBitrate", p.audioBitrate);
    v.set("speed", p.speed);
    v.set("keepPitch", p.keepPitch);
    v.set("hardwareDecode", p.hardwareDecode);
    v.set("halfSize", p.halfSize);
    v.set("orientation", p.orientation);
    v.set("outWidth", p.outWidth);
    v.set("outHeight", p.outHeight);
    return v;
}

}  // namespace

void Settings::load() {
    std::ifstream in(fileLocation(), std::ios::binary);
    if (!in) return;
    std::ostringstream text;
    text << in.rdbuf();
    const json::Value root = json::parse(text.str());
    if (!root.isObject()) return;

    volume = static_cast<float>(root["volume"].asNumber(1.0));
    normalise = root["normalise"].asBool();
    normaliseTarget = static_cast<float>(root["normaliseTarget"].asNumber(-14.0));
    editorByDefault = root["editorByDefault"].asBool();
    waveMode = root["waveMode"].asString("editor");
    thumbMode = root["thumbMode"].asString("editor");
    rememberPosition = root["rememberPosition"].asBool(true);
    startFullscreen = root["startFullscreen"].asBool(false);
    alwaysOnTop = root["alwaysOnTop"].asBool(false);
    keepPitch = root["keepPitch"].asBool(true);
    volumeStep = root["volumeStep"].asInt(5);
    uiFps = std::max(10, std::min(144, root["uiFps"].asInt(30)));
    seekSmall = root["seekSmall"].asInt(10);
    seekLarge = root["seekLarge"].asInt(60);
    avOffset = root["avOffset"].asNumber(0.0);
    fitMode = root["fitMode"].asInt();
    contrast = root["contrast"].asInt();
    brightness = root["brightness"].asInt();
    saturation = root["saturation"].asInt();
    gamma = root["gamma"].asInt();
    hue = root["hue"].asInt();
    sharpen = root["sharpen"].asInt();
    denoise = root["denoise"].asInt();
    lastFolder = root["lastFolder"].asString();
    language = root["language"].asString("en");

    audioPresets.clear();
    for (const json::Value& v : root["audioPresets"].asArray()) {
        audioPresets.push_back(readAudioPreset(v));
    }
    videoPresets.clear();
    for (const json::Value& v : root["videoPresets"].asArray()) {
        videoPresets.push_back(readVideoPreset(v));
    }
    renderPresets.clear();
    for (const json::Value& v : root["renderPresets"].asArray()) {
        renderPresets.push_back(readRenderPreset(v));
    }

    positions.clear();
    for (const auto& [path, seconds] : root["positions"].asObject()) {
        positions[path] = seconds.asNumber();
    }
}

void Settings::save() const {
    json::Value root;
    root.set("volume", volume);
    root.set("normalise", normalise);
    root.set("normaliseTarget", normaliseTarget);
    root.set("editorByDefault", editorByDefault);
    root.set("waveMode", waveMode);
    root.set("thumbMode", thumbMode);
    root.set("rememberPosition", rememberPosition);
    root.set("startFullscreen", startFullscreen);
    root.set("alwaysOnTop", alwaysOnTop);
    root.set("keepPitch", keepPitch);
    root.set("volumeStep", volumeStep);
    root.set("uiFps", uiFps);
    root.set("seekSmall", seekSmall);
    root.set("seekLarge", seekLarge);
    root.set("avOffset", avOffset);
    root.set("fitMode", fitMode);
    root.set("contrast", contrast);
    root.set("brightness", brightness);
    root.set("saturation", saturation);
    root.set("gamma", gamma);
    root.set("hue", hue);
    root.set("sharpen", sharpen);
    root.set("denoise", denoise);
    root.set("lastFolder", lastFolder);
    root.set("language", language);

    json::Value audio, video, render;
    // Made into arrays before anything is added, so an empty list is written as
    // [] rather than null. It reads back the same either way, but this file is
    // meant to survive being opened in an editor.
    audio.array();
    video.array();
    render.array();
    for (const AudioPreset& p : audioPresets) audio.array().push_back(writeAudioPreset(p));
    for (const VideoPreset& p : videoPresets) video.array().push_back(writeVideoPreset(p));
    for (const RenderPreset& p : renderPresets) render.array().push_back(writeRenderPreset(p));
    root.set("audioPresets", audio);
    root.set("videoPresets", video);
    root.set("renderPresets", render);

    json::Value where;
    where.object();
    for (const auto& [path, seconds] : positions) where.set(path, seconds);
    root.set("positions", where);

    // Written beside the real file and moved over it: a crash or a full disk
    // half way through then costs nothing, where writing in place would lose
    // every preference at once.
    const std::wstring target = fileLocation();
    const std::wstring temp = target + L".new";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << root.dump(2) << "\n";
        out.close();
        if (!out) { DeleteFileW(temp.c_str()); return; }
    }
    MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void Settings::rememberPositionFor(const std::wstring& path, double seconds) {
    if (!rememberPosition || path.empty()) return;
    const std::string key = toUtf8(path);
    if (seconds < 20.0) {
        positions.erase(key);   // near the start is the same as not started
        return;
    }
    positions[key] = seconds;

    // A cap, so the file does not grow without limit. The oldest entries are
    // not tracked, so this drops an arbitrary one - which is the right trade
    // for a convenience that costs nothing when it misses.
    while (positions.size() > 400) positions.erase(positions.begin());
}

double Settings::positionFor(const std::wstring& path) const {
    if (!rememberPosition) return 0.0;
    auto it = positions.find(toUtf8(path));
    return it == positions.end() ? 0.0 : it->second;
}

Settings& settings() {
    static Settings instance;
    return instance;
}

}  // namespace vp
