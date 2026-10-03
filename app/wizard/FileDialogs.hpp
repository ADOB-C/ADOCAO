#pragma once

// Native file open / folder selection dialogs (Win32 COM on Windows,
// tinyfiledialogs elsewhere) plus sidecar-music auto-detection.

#include <string>
#include <vector>

namespace wizard {

// Opens a file chooser. `patterns` are real filter patterns (e.g. {"*.adofai",
// "*.adofai.xz"}); previously the single string was passed to tinyfiledialogs as the
// filter *description* with zero patterns, so the dialog filtered nothing at all.
// Returns an empty string when the user cancels.
std::string openFileDialog(const char* title, const std::vector<std::string>& patterns,
                           const char* description);

// Folder picker, seeded with initialDir. Empty when cancelled.
std::string selectFolderDialog(const char* title, const std::string& initialDir);

// Auto-detect the music file: same-name audio next to the level file
// (.ogg/.mp3/.wav/.flac/.m4a). ".adofai.xz"/".adofai.zst" are stripped first.
// Returns "" if nothing found.
std::string detectMusicFile(const std::string& levelPath);

} // namespace wizard
