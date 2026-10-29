// Chromatron - version, update check and About dialog (Windows and Linux platform layers;
// macOS is in AboutApple.mm).

#include "core/About.h"
#include "core/AboutPlatform.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <vector>

#ifndef CHROMATRON_VERSION_STRING
#define CHROMATRON_VERSION_STRING "0.0.0"
#endif

namespace chromatron {

namespace {

const char* const kLatestReleaseApi = "https://api.github.com/repos/Velich2/Chromatron/releases/latest";

std::vector<long> versionParts(const std::string& v)
{
    std::vector<long> parts;
    size_t i = 0;
    while (i < v.size() && !std::isdigit((unsigned char)v[i])) ++i;  // "v1.2.0" -> "1.2.0"
    while (i < v.size() && std::isdigit((unsigned char)v[i])) {
        long n = 0;
        while (i < v.size() && std::isdigit((unsigned char)v[i])) {
            if (n < 100000000) n = n * 10 + (v[i] - '0');  // saturate instead of overflowing
            ++i;
        }
        parts.push_back(n);
        if (i < v.size() && v[i] == '.') ++i;
        else break;
    }
    return parts;
}

int ask(const AskFunction& fallback, const std::string& message, const char* button1, const char* button2)
{
    int result = 0;
    if (platform::ask("Chromatron", message, button1, button2, result)) return result;
    return fallback ? fallback(message, button1, button2) : 0;
}

}  // namespace

const char* version() { return CHROMATRON_VERSION_STRING; }

const char* releasesUrl() { return "https://github.com/Velich2/Chromatron/releases/latest"; }

bool parseReleaseVersion(const std::string& json, std::string& out)
{
    const std::string key = "\"tag_name\"";
    size_t p = json.find(key);
    if (p == std::string::npos) return false;
    p = json.find(':', p + key.size());
    if (p == std::string::npos) return false;
    p = json.find('"', p);
    if (p == std::string::npos) return false;
    const size_t end = json.find('"', p + 1);
    if (end == std::string::npos) return false;
    std::string tag = json.substr(p + 1, end - p - 1);
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
    if (versionParts(tag).empty()) return false;
    out = tag;
    return true;
}

int compareVersions(const std::string& a, const std::string& b)
{
    const std::vector<long> x = versionParts(a), y = versionParts(b);
    for (size_t i = 0; i < x.size() || i < y.size(); ++i) {
        const long u = i < x.size() ? x[i] : 0, v = i < y.size() ? y[i] : 0;
        if (u != v) return u < v ? -1 : 1;
    }
    return 0;
}

bool fetchLatestVersion(std::string& latest, std::string& error)
{
    std::string body;
    int status = 0;
    const std::string agent = std::string("Chromatron/") + version();
    if (!platform::httpsGet(kLatestReleaseApi, agent.c_str(), body, status, error)) return false;
    if (status != 200) {
        error = "GitHub answered with HTTP " + std::to_string(status);
        return false;
    }
    if (!parseReleaseVersion(body, latest)) {
        error = "unexpected reply from GitHub";
        return false;
    }
    return true;
}

void openReleasesPage() { platform::openUrl(releasesUrl()); }

void showAboutDialog(const AskFunction& fallback)
{
    const std::string installed = version();
    const std::string about = "Chromatron " + installed +
                              "\n\nOptical chromatic aberration for After Effects, Premiere Pro and DaVinci Resolve.";
    if (ask(fallback, about, "Check for Updates", "Close") != 1) return;

    std::string latest, error;
    if (!fetchLatestVersion(latest, error)) {
        if (ask(fallback, "Could not check for updates: " + error + ".\n\nOpen the releases page instead?",
                "Open Releases Page", "Close") == 1)
            openReleasesPage();
        return;
    }
    if (compareVersions(latest, installed) > 0) {
        if (ask(fallback, "Chromatron " + latest + " is available. You have " + installed + ".",
                "Open Download Page", "Later") == 1)
            openReleasesPage();
    } else {
        ask(fallback, "You have the latest version, Chromatron " + installed + ".", "OK", nullptr);
    }
}

}  // namespace chromatron

// ---------------------------------------------------------------------------
// Windows: WinHTTP, ShellExecute, TaskDialog (MessageBox where common controls v6 are missing).
// ---------------------------------------------------------------------------
#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winhttp.h>

namespace chromatron {
namespace platform {

namespace {

std::wstring widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle()
    {
        if (h) WinHttpCloseHandle(h);
    }
};

std::string lastError(const char* what) { return std::string(what) + " failed (" + std::to_string(GetLastError()) + ")"; }

}  // namespace

bool httpsGet(const char* url, const char* userAgent, std::string& body, int& status, std::string& error)
{
    const std::wstring wurl = widen(url);
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[1024] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
        error = lastError("WinHttpCrackUrl");
        return false;
    }

    Handle session, connection, request;
    // Automatic proxy needs Windows 8.1; older systems use the WinHTTP default proxy.
    session.h = WinHttpOpen(widen(userAgent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h)
        session.h = WinHttpOpen(widen(userAgent).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) {
        error = lastError("WinHttpOpen");
        return false;
    }
    WinHttpSetTimeouts(session.h, 5000, 5000, 8000, 8000);
    connection.h = WinHttpConnect(session.h, host, parts.nPort, 0);
    if (!connection.h) {
        error = lastError("connecting to GitHub");
        return false;
    }
    request.h = WinHttpOpenRequest(connection.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request.h) {
        error = lastError("WinHttpOpenRequest");
        return false;
    }
    const wchar_t* headers = L"Accept: application/vnd.github+json\r\n";
    if (!WinHttpSendRequest(request.h, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        error = lastError("the request to GitHub");
        return false;
    }
    DWORD code = 0, size = sizeof(code);
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX)) {
        error = lastError("reading the reply status");
        return false;
    }
    status = (int)code;
    body.clear();
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) {
            error = lastError("reading the reply");
            return false;
        }
        if (available == 0) break;
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request.h, &chunk[0], available, &read)) {
            error = lastError("reading the reply");
            return false;
        }
        body.append(chunk, 0, read);
        if (body.size() > (1u << 20)) break;  // a release reply is a few kB
    }
    return true;
}

void openUrl(const char* url) { ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

bool ask(const std::string& title, const std::string& message, const char* button1, const char* button2, int& result)
{
    const std::wstring wtitle = widen(title), wmessage = widen(message);
    const std::wstring b1 = widen(button1 ? button1 : "OK"), b2 = widen(button2 ? button2 : "");
    HWND owner = GetActiveWindow();

    // TaskDialogIndirect lives in common controls v6, which the hosts load; resolve it at run
    // time so the plug-in still loads (and falls back to MessageBox) where it is missing.
    using TaskDialogFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    HMODULE comctl = GetModuleHandleW(L"comctl32.dll");
    if (!comctl) comctl = LoadLibraryW(L"comctl32.dll");
    const auto taskDialog = comctl ? (TaskDialogFn)GetProcAddress(comctl, "TaskDialogIndirect") : nullptr;
    if (taskDialog) {
        TASKDIALOG_BUTTON buttons[2] = {{1001, b1.c_str()}, {1002, b2.c_str()}};
        TASKDIALOGCONFIG config = {};
        config.cbSize = sizeof(config);
        config.hwndParent = owner;
        config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        config.pszWindowTitle = wtitle.c_str();
        config.pszContent = wmessage.c_str();
        config.cButtons = button2 ? 2 : 1;
        config.pButtons = buttons;
        config.nDefaultButton = 1001;
        int pressed = 0;
        if (SUCCEEDED(taskDialog(&config, &pressed, nullptr, nullptr))) {
            result = pressed == 1001 ? 1 : pressed == 1002 ? 2 : 0;
            return true;
        }
    }
    if (!button2) {
        MessageBoxW(owner, wmessage.c_str(), wtitle.c_str(), MB_OK | MB_ICONINFORMATION);
        result = 1;
        return true;
    }
    const std::wstring text = wmessage + L"\n\nOK: " + b1 + L"    Cancel: " + b2;
    result = MessageBoxW(owner, text.c_str(), wtitle.c_str(), MB_OKCANCEL | MB_ICONINFORMATION) == IDOK ? 1 : 2;
    return true;
}

}  // namespace platform
}  // namespace chromatron

// ---------------------------------------------------------------------------
// Linux (Resolve): curl and xdg-open; no native dialog, the OFX host's messages are used.
// ---------------------------------------------------------------------------
#elif !defined(__APPLE__)

namespace chromatron {
namespace platform {

bool httpsGet(const char* url, const char* userAgent, std::string& body, int& status, std::string& error)
{
    // Fixed arguments only: url and agent are compile-time constants of this file.
    const std::string command = std::string("curl -sS --max-time 10 -w '\\n%{http_code}' -A '") + userAgent +
                                "' -H 'Accept: application/vnd.github+json' '" + url + "' 2>&1";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        error = "curl is not available";
        return false;
    }
    std::string out;
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) out.append(buffer, n);
    const int rc = pclose(pipe);
    // A host that ignores SIGCHLD makes pclose fail with ECHILD although curl ran: trust the output then.
    const bool exited = rc == 0 || (rc == -1 && errno == ECHILD);
    const size_t nl = out.rfind('\n');
    if (!exited || nl == std::string::npos) {
        error = out.empty() ? "curl failed" : out.substr(0, out.find('\n'));
        return false;
    }
    status = std::atoi(out.c_str() + nl + 1);
    body = out.substr(0, nl);
    if (status == 0) {  // no HTTP reply (only reachable when the exit status was lost): curl's message
        error = body.empty() ? "curl failed" : body.substr(0, body.find('\n'));
        return false;
    }
    return true;
}

void openUrl(const char* url)
{
    const std::string command = std::string("xdg-open '") + url + "' >/dev/null 2>&1 &";
    if (std::system(command.c_str()) != 0) {
        // Nothing else to try; the URL is shown in the dialog text.
    }
}

bool ask(const std::string&, const std::string&, const char*, const char*, int&) { return false; }

}  // namespace platform
}  // namespace chromatron

#endif
