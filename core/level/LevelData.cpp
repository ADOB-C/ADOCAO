#include "LevelData.hpp"
#include "JsonCleaner.hpp"
#include "core/util/Logger.hpp"
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

// Fast parser: extract angleData float array from JSON without DOM allocation.
// Returns the parsed array and writes the end position (after ']') to `outArrayEnd`.
static std::vector<double> parseAngleDataFast(const char* json, size_t len, size_t& outArrayEnd) {
    const char* key = "\"angleData\"";
    const char* pos = (const char*)std::memchr(json, '"', len);
    while (pos) {
        size_t remain = len - (pos - json);
        if (remain >= 11 && std::memcmp(pos, key, 11) == 0) {
            pos += 11;
            while (pos < json + len && (*pos == ' ' || *pos == ':' || *pos == '\t' || *pos == '\n'))
                pos++;
            if (*pos != '[') return {};
            pos++; // skip '['
            break;
        }
        pos++;
        pos = (const char*)std::memchr(pos, '"', json + len - pos);
    }
    if (!pos) return {};

    std::vector<double> result;
    result.reserve((len - (pos - json)) / 30);
    while (pos < json + len) {
        while (pos < json + len && (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r'))
            pos++;
        if (pos >= json + len) break;
        if (*pos == ']') { outArrayEnd = pos - json + 1; break; }
        if (*pos == ',') { pos++; continue; }
        char* end;
        double val = strtod(pos, &end);
        if (end == pos) { pos++; continue; }
        result.push_back(val);
        pos = end;
    }
    return result;
}

// Fast streaming parser for actions — avoids nlohmann DOM for huge action arrays.
static std::string readFileUtf8(const std::string& filepath) {
#ifdef _WIN32
    // Convert UTF-8 path to wide for Windows API
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                   filepath.c_str(), -1, nullptr, 0);
    std::wstring wpath;
    if (wlen > 0) {
        wpath.resize(wlen);
        MultiByteToWideChar(CP_UTF8, 0, filepath.c_str(), -1, &wpath[0], wlen);
    } else {
        wlen = MultiByteToWideChar(CP_ACP, 0, filepath.c_str(), -1, nullptr, 0);
        if (wlen <= 0) return {};
        wpath.resize(wlen);
        MultiByteToWideChar(CP_ACP, 0, filepath.c_str(), -1, &wpath[0], wlen);
    }

    // Memory-mapped file: avoids OS buffer copy for large files
    HANDLE hFile = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return {};

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0) {
        CloseHandle(hFile); return {};
    }

    HANDLE hMapping = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMapping) { CloseHandle(hFile); return {}; }

    const char* data = (const char*)MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
    if (!data) { CloseHandle(hMapping); CloseHandle(hFile); return {}; }

    size_t size = (size_t)fileSize.QuadPart;
    // Skip UTF-8 BOM if present
    size_t offset = (size >= 3 && (unsigned char)data[0] == 0xEF
                     && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) ? 3 : 0;

    std::string content(data + offset, size - offset);

    UnmapViewOfFile(data);
    CloseHandle(hMapping);
    CloseHandle(hFile);
    return content;
#else
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return {};
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    // Skip UTF-8 BOM if present (RapidJSON rejects it at offset 0)
    if (content.size() >= 3 && (unsigned char)content[0] == 0xEF
        && (unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF)
        content.erase(0, 3);
    return content;
#endif
}

bool LevelData::loadFromFile(const std::string& filepath, ProgressCb onProgress, bool exportOnly) {
    if (onProgress) onProgress(0.05f, "Reading file...");
    std::string content = readFileUtf8(filepath);
    if (content.empty()) {
        LOG_E("Cannot open level file: %s", filepath.c_str());
        return false;
    }
    return loadFromString(cleanJson(content), onProgress, exportOnly);
}

bool LevelData::loadFromString(const std::string& jsonStr, ProgressCb onProgress, bool exportOnly) {
    try {
        if (onProgress) onProgress(0.10f, "Parsing angleData...");

        // Fast path: parse angleData directly without JSON DOM allocation
        size_t angleDataEnd = 0;
        angleData = parseAngleDataFast(jsonStr.c_str(), jsonStr.size(), angleDataEnd);

        // Helper: strip a JSON array value from a string, replacing it with [].
        // Returns the stripped string. Prevents nlohmann from parsing huge unused arrays.
        auto stripArray = [](const std::string& src, const char* key) -> std::string {
            const char* p = std::strstr(src.c_str(), key);
            if (!p) return src;
            size_t keyStart = p - src.c_str();
            size_t arrStart = keyStart + strlen(key);
            while (arrStart < src.size() && (src[arrStart] == ' ' || src[arrStart] == ':'
                   || src[arrStart] == '\t' || src[arrStart] == '\n'))
                arrStart++;
            if (arrStart >= src.size() || src[arrStart] != '[') return src;
            // Count brackets to find matching ]
            int depth = 1;
            size_t arrEnd = arrStart + 1;
            bool inString = false;
            for (; arrEnd < src.size() && depth > 0; arrEnd++) {
                char c = src[arrEnd];
                if (c == '"' && (arrEnd == 0 || src[arrEnd-1] != '\\')) inString = !inString;
                if (inString) continue;
                if (c == '[') depth++;
                else if (c == ']') depth--;
            }
            std::string out;
            out.reserve(keyStart + 2 + (src.size() - arrEnd));
            out.append(src, 0, arrStart);
            out += "[]";
            out.append(src, arrEnd, std::string::npos);
            return out;
        };

        // RapidJSON: parse full JSON (angleData + decorations stripped)
        // This handles both actions and settings in one DOM
        std::string stripped = jsonStr;
        if (angleDataEnd > 0) {
            stripped = stripArray(jsonStr, "\"angleData\"");
        }
        stripped = stripArray(stripped, "\"decorations\"");

        if (onProgress) onProgress(0.12f, "Parsing JSON...");
        rapidjson::Document root;
        root.Parse<rapidjson::kParseTrailingCommasFlag>(stripped.c_str());
        if (root.HasParseError()) {
            LOG_E("RapidJSON parse error at offset %zu, code %d",
                  root.GetErrorOffset(), (int)root.GetParseError());
            return false;
        }

        if (onProgress) onProgress(0.15f, "Extracting level data...");

        // Actions
        if (root.HasMember("actions") && root["actions"].IsArray()) {
            auto& arr = root["actions"];
            for (rapidjson::SizeType i = 0; i < arr.Size(); i++) {
                auto& a = arr[i];
                if (!a.IsObject() || !a.HasMember("floor") || !a.HasMember("eventType")) continue;
                FastAction act;
                act.floor = a["floor"].GetInt();
                std::string et = a["eventType"].GetString();
                if (et == "Twirl") act.type = FastAction::Twirl;
                else if (et == "SetSpeed") act.type = FastAction::SetSpeed;
                else if (et == "PositionTrack") act.type = FastAction::PositionTrack;
                else if (et == "SetHitsound") act.type = FastAction::SetHitsound;
                else if (et == "Bookmark") act.type = FastAction::Bookmark;
                else if (et == "Pause") act.type = FastAction::Pause;
                else if (et == "AnimateTrack") act.type = FastAction::AnimateTrack;
                else continue;
                if (act.type == FastAction::SetSpeed) {
                    if (a.HasMember("speedType") && std::string(a["speedType"].GetString()) == "Multiplier") {
                        act.flag = true; act.val1 = a.HasMember("bpmMultiplier") ? a["bpmMultiplier"].GetFloat() : 1.0f;
                    } else { act.val1 = a.HasMember("beatsPerMinute") ? a["beatsPerMinute"].GetFloat() : 0.0f; }
                } else if (act.type == FastAction::Pause) {
                    act.val1 = a.HasMember("duration") ? a["duration"].GetFloat() : 0.0f;
                } else if (act.type == FastAction::PositionTrack) {
                    if (a.HasMember("positionOffset") && a["positionOffset"].IsArray() && a["positionOffset"].Size() >= 2) {
                        act.val1 = a["positionOffset"][0].GetFloat(); act.val2 = a["positionOffset"][1].GetFloat();
                    }
                    if (a.HasMember("justThisTile")) {
                        if (a["justThisTile"].IsBool()) act.flag = a["justThisTile"].GetBool();
                        else if (a["justThisTile"].IsInt()) act.flag = a["justThisTile"].GetInt() != 0;
                        else if (a["justThisTile"].IsString()) {
                            std::string v = a["justThisTile"].GetString();
                            act.flag = (v == "Enabled" || v == "true" || v == "True");
                        }
                    }
                } else if (act.type == FastAction::SetHitsound) {
                    act.str = a.HasMember("hitsound") ? a["hitsound"].GetString() : "";
                    act.val1 = a.HasMember("hitsoundVolume") ? a["hitsoundVolume"].GetFloat() : 0.0f;
                } else if (act.type == FastAction::AnimateTrack) {
                    act.val1 = -1.0f; act.val2 = -1.0f; // sentinel: not set
                    if (a.HasMember("trackDisappearAnimation")) act.str = a["trackDisappearAnimation"].GetString();
                    if (a.HasMember("trackAnimation"))  act.flag = true; // flag2: has trackAnimation
                    if (a.HasMember("beatsBehind")) act.val1 = a["beatsBehind"].GetFloat();
                    if (a.HasMember("beatsAhead"))  act.val2 = a["beatsAhead"].GetFloat();
                }
                actions.push_back(act);
            }
        }

        // Settings
        if (root.HasMember("settings") && root["settings"].IsObject()) {
            auto& s = root["settings"];
            auto getF = [&](const char* k, float d) { return s.HasMember(k) ? s[k].GetFloat() : d; };
            auto getI = [&](const char* k, int d) { return s.HasMember(k) ? s[k].GetInt() : d; };
            auto getS = [&](const char* k, const char* d) -> std::string {
                return (s.HasMember(k) && s[k].IsString()) ? s[k].GetString() : d;
            };
            settings.bpm             = getF("bpm", 100.0f);
            settings.offset          = getF("offset", 0.0f);
            settings.countdownTicks  = getI("countdownTicks", 4);
            settings.zoom            = getF("zoom", 100.0f);
            settings.rotation        = getF("rotation", 0.0f);
            settings.relativeTo      = getS("relativeTo", "Player");
            settings.hitsound        = getS("hitsound", "Kick");
            settings.hitsoundVolume  = getF("hitsoundVolume", 100.0f);
            settings.trackColor      = getS("trackColor", "debb7b");
            settings.secondaryTrackColor = getS("secondaryTrackColor", "ffffff");
            settings.backgroundColor = getS("backgroundColor", "000000");
            settings.planetEase      = getS("planetEase", "Linear");
            settings.trackDisappearAnimation = getS("trackDisappearAnimation", "None");
            settings.trackAnimation  = getS("trackAnimation", "None");
            settings.beatsBehind     = getF("beatsBehind", 4.0f);
            settings.beatsAhead      = getF("beatsAhead", 3.0f);
            if (s.HasMember("stickToFloors")) {
                if (s["stickToFloors"].IsBool()) settings.stickToFloors = s["stickToFloors"].GetBool();
                else if (s["stickToFloors"].IsString()) {
                    std::string v = s["stickToFloors"].GetString();
                    settings.stickToFloors = (v == "Enabled" || v == "true" || v == "True");
                }
            }
            if (s.HasMember("position") && s["position"].IsArray() && s["position"].Size() >= 2)
                settings.position = {s["position"][0].GetFloat(), s["position"][1].GetFloat()};
        }

        // pathData
        if (root.HasMember("pathData") && root["pathData"].IsString())
            pathData = root["pathData"].GetString();

        // actions already parsed via fast path above (with nlohmann fallback)
        // decorations: not used, skip parsing entirely

        if (onProgress) onProgress(0.20f, "Processing level data...");

        // Convert pathData → angleData if needed
        if (!pathData.empty() && angleData.empty()) {
            convertPathToAngles();
        }

        if (!exportOnly) {
            if (onProgress) onProgress(0.30f, "Calculating tile positions...");
            calculateTilePositions();
        }
        if (onProgress) onProgress(0.40f, "Processing actions...");
        processActions();
        if (!exportOnly) {
            applyPositionTrackOffsets();
        }
        return true;
    } catch (const std::exception& e) {
        LOG_E("JSON parse error: %s", e.what());
        return false;
    }
}

void LevelData::calculateTilePositions() {
    tiles.clear();
    if (angleData.empty()) return;

    int n = static_cast<int>(angleData.size());

    // Build "floats" array: 999 = midspin (previous + 180)
    std::vector<float> floats(n);
    for (int i = 0; i < n; i++) {
        if (angleData[i] == 999.0) {
            floats[i] = (i > 0 ? floats[i - 1] : 0.0f) + 180.0f;
        } else {
            floats[i] = angleData[i];
        }
    }

    tiles.resize(n);
    double curX = 0.0, curY = 0.0;  // double for precision

    for (int i = 0; i < n; i++) {
        tiles[i].index = i;
        tiles[i].position = {curX, curY};
        tiles[i].direction = floats[i];

        double rad = (double)floats[i] * 3.14159265358979323846 / 180.0;
        curX += std::cos(rad);
        curY += std::sin(rad);
    }

    // Append extra tile (infinite rotation reference)
    if (n > 0) {
        Tile extra;
        extra.index = n;
        double dir = 0.0, length = 1.0;
        if (n > 1) {
            double dx = (double)tiles[n-1].position[0] - (double)tiles[n-2].position[0];
            double dy = (double)tiles[n-1].position[1] - (double)tiles[n-2].position[1];
            length = std::sqrt(dx*dx + dy*dy);
            if (length > 0.01) dir = std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;
            if (length < 0.01) length = 1.0;
        }
        double rad = dir * 3.14159265358979323846 / 180.0;
        extra.position = {
            (float)(tiles[n-1].position[0] + std::cos(rad) * length),
            (float)(tiles[n-1].position[1] + std::sin(rad) * length)
        };
        extra.angle = 180.0f;
        extra.direction = (float)dir;
        tiles.push_back(extra);
    }
}

// ADOFAI pathData → angleData conversion
// Based on ADOFAI-JS official mapping table (src/pathdata/index.ts)

float LevelData::pathCharToAngle(char c) {
    switch (c) {
        case 'R': return 0;
        case 'p': return 15;
        case 'J': return 30;
        case 'E': return 45;
        case 'T': return 60;
        case 'o': return 75;
        case 'U': return 90;
        case 'q': return 105;
        case 'G': return 120;
        case 'Q': return 135;
        case 'H': return 150;
        case 'W': return 165;
        case 'L': return 180;
        case 'x': return 195;
        case 'N': return 210;
        case 'Z': return 225;
        case 'F': return 240;
        case 'V': return 255;
        case 'D': return 270;
        case 'Y': return 285;
        case 'B': return 300;
        case 'C': return 315;
        case 'M': return 330;
        case 'A': return 345;
        case '5': return 555;   // multi-hit stack 5
        case '6': return 666;   // multi-hit stack 6
        case '7': return 777;   // multi-hit stack 7
        case '8': return 888;   // multi-hit stack 8
        case '!': return 999;   // midspin
        default:  return 0;
    }
}

void LevelData::processActions() {
    int n = (int)angleData.size() + 1;  // +1 for tile 0 (tiles may be empty in export mode)
    tileBPMs.assign(n, settings.bpm);
    tileHasTwirl.assign(n, false);
    tileHasSetSpeed.assign(n, false);
    bookmarkFloors.clear();

    struct SS { float multiplier = 0.0f; float bpm = 0.0f; bool isMultiplier = false; };
    std::vector<SS> setSpeedByFloor(n);

    struct HSChange { int floor; std::string type; float volume; };
    std::vector<HSChange> hsChanges;

    for (auto& a : actions) {
        int floor = a.floor;
        if (floor < 0 || floor >= n) continue;
        switch (a.type) {
        case FastAction::Twirl:
            tileHasTwirl[floor] = true; break;
        case FastAction::SetSpeed:
            tileHasSetSpeed[floor] = true;
            { SS& ev = setSpeedByFloor[floor]; ev.isMultiplier = a.flag;
              if (a.flag) ev.multiplier = a.val1; else ev.bpm = a.val1; }
            break;
        case FastAction::PositionTrack:
            tilePositionOffsets[floor] = {a.val1, a.val2, a.flag}; break;
        case FastAction::SetHitsound:
            hsChanges.push_back({floor, a.str.empty() ? settings.hitsound : a.str,
                                 a.val1 > 0 ? a.val1 : settings.hitsoundVolume}); break;
        case FastAction::Bookmark:
            bookmarkFloors.push_back(floor); break;
        case FastAction::AnimateTrack:
            atStates[floor] = {a.str.empty() ? settings.trackDisappearAnimation : a.str,
                               settings.trackAnimation, // aa not parsed yet; use global
                               a.val1 >= 0 ? a.val1 : settings.beatsBehind,
                               a.val2 >= 0 ? a.val2 : settings.beatsAhead,
                               a.flag};
            break;
        default: break;
        }
    }

    float runningBPM = settings.bpm;
    for (int i = 0; i < n; i++) {
        if (setSpeedByFloor[i].isMultiplier) runningBPM *= setSpeedByFloor[i].multiplier;
        else if (setSpeedByFloor[i].bpm > 0.0f) runningBPM = setSpeedByFloor[i].bpm;
        tileBPMs[i] = runningBPM;
    }

    if (!hsChanges.empty()) {
        std::string curHS = settings.hitsound;
        float curVol = settings.hitsoundVolume;
        size_t ci = 0;
        for (int i = 0; i < n; i++) {
            while (ci < hsChanges.size() && hsChanges[ci].floor <= i) {
                curHS = hsChanges[ci].type; curVol = hsChanges[ci].volume; ci++;
            }
            if (curHS != settings.hitsound) tileHitsounds[i] = curHS;
            if (curVol != settings.hitsoundVolume) tileHitsoundVolumes[i] = curVol;
        }
    }

    std::sort(bookmarkFloors.begin(), bookmarkFloors.end());

    // Viewer convenience: tile 0 and the final tile are always jump targets,
    // even when the chart defines no Bookmark events (Ctrl+←/→ while stopped).
    bookmarkFloors.push_back(0);
    if (n > 1) bookmarkFloors.push_back(n - 1);
    std::sort(bookmarkFloors.begin(), bookmarkFloors.end());
    bookmarkFloors.erase(std::unique(bookmarkFloors.begin(), bookmarkFloors.end()),
                         bookmarkFloors.end());
}

void LevelData::applyPositionTrackOffsets() {
    if (tilePositionOffsets.empty()) return;

    double cumX = 0.0, cumY = 0.0;
    int n = (int)tiles.size();

    std::vector<std::pair<int, TilePositionOffset>> sorted(tilePositionOffsets.begin(), tilePositionOffsets.end());
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.first < b.first; });
    size_t oi = 0;

    for (int i = 0; i < n; i++) {
        while (oi < sorted.size() && sorted[oi].first == i) {
            cumX += sorted[oi].second.offsetX;
            cumY += sorted[oi].second.offsetY;
            oi++;
        }
        tiles[i].position[0] += cumX;
        tiles[i].position[1] += cumY;
    }
}

void LevelData::releaseMemory() {
    // Free arrays no longer needed after loading completes
    actions.clear(); actions.shrink_to_fit();
    tilePositionOffsets.clear();
    tileHitsounds.clear();
    tileHitsoundVolumes.clear();
    std::string().swap(pathData);
    // angleData kept: needed by TileMesh::build() for midspin detection
    // tileBPMs kept: needed by buildIcons() for SetSpeed icon coloring
    // tileHasTwirl/tileHasSetSpeed kept: needed by buildIcons()
    // bookmarkFloors kept: needed during gameplay for Ctrl+Left/Right navigation
}

void LevelData::convertPathToAngles() {
    if (pathData.empty()) return;

    angleData.clear();
    angleData.reserve(pathData.size());

    for (char c : pathData) {
        angleData.push_back(pathCharToAngle(c));
    }
}
