#pragma once

// Native file open / folder selection dialogs (Win32 COM on Windows,
// tinyfiledialogs elsewhere) plus sidecar-music auto-detection.

#include <string>

namespace wizard {

// Opens a file chooser with the given title and filter string (e.g. "*.adofai").
// Returns an empty string when the user cancels.
std::string openFileDialog(const char* title, const char* filterStr);

// Folder picker, seeded with initialDir. Empty when cancelled.
std::string selectFolderDialog(const char* title, const std::string& initialDir);

// Auto-detect the music file: same-name audio next to the .adofai
// (.ogg/.mp3/.wav/.flac/.m4a). Returns "" if nothing found.
std::string detectMusicFile(const std::string& levelPath);

} // namespace wizard
