// Chromatron - version, update check against the GitHub releases and the About dialog.

#pragma once

#include <functional>
#include <string>

namespace chromatron {

// Plug-in version, from the CMake project version (e.g. "1.2.0").
const char* version();

// The release page users download updates from.
const char* releasesUrl();

// Extracts the version from a GitHub "latest release" JSON reply ("tag_name": "v1.3.0" -> "1.3.0").
bool parseReleaseVersion(const std::string& json, std::string& version);

// Compares dotted versions numerically: <0 if a is older than b, 0 if equal, >0 if newer.
int compareVersions(const std::string& a, const std::string& b);

// Asks GitHub for the latest release (blocking, about 10 s at most). Only the request itself
// reaches GitHub; nothing about the project or the system is sent.
bool fetchLatestVersion(std::string& latest, std::string& error);

// Opens the releases page in the default browser.
void openReleasesPage();

// A two-button question: returns 1 for the first button, 2 for the second, 0 if dismissed.
// button2 may be null for a single OK-style button.
using AskFunction = std::function<int(const std::string& message, const char* button1, const char* button2)>;

// About dialog: the version and a "Check for Updates" button that offers the download page when a
// newer release exists. Uses a native dialog on Windows and macOS; elsewhere `fallback` (e.g. the
// host's message suite) asks instead. Call on the host's UI thread.
void showAboutDialog(const AskFunction& fallback = nullptr);

}  // namespace chromatron
