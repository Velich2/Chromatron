// Chromatron - platform layer of About.cpp (Windows / Linux in About.cpp, macOS in AboutApple.mm).

#pragma once

#include <string>

namespace chromatron {
namespace platform {

// HTTPS GET; body and HTTP status on success, a short reason on failure.
bool httpsGet(const char* url, const char* userAgent, std::string& body, int& status, std::string& error);

void openUrl(const char* url);

// Native two-button dialog. Returns false when the platform has none (the caller falls back).
bool ask(const std::string& title, const std::string& message, const char* button1, const char* button2,
         int& result);

}  // namespace platform
}  // namespace chromatron
