// C ABI wrapper over LibreOfficeKit for Windows (API: include/lok_viewer.h).
//
// - LibreOfficeKit is not safe for concurrent calls, so every LOK call runs under g_mutex.
// - Document handles are opaque ids, not LOK pointers: a stale or double-closed handle
//   is rejected instead of crashing the process.
// - No C++ exception may cross the C ABI: every export runs inside guarded().

#define LOK_VIEWER_EXPORTS
// getParts, paintTile, getDocumentSize, ... are declared only with this flag.
#define LOK_USE_UNSTABLE_API

#include "lok_viewer.h"
#include "LibreOfficeKit/LibreOfficeKit.h"

#ifndef _WIN32
  #error "lok_viewer.cpp supports Windows only (it loads mergedlo.dll)."
#endif

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

// LOK reports the document size as C 'long'; the public API exposes it as int32_t.
static_assert(sizeof(long) == sizeof(int32_t), "C 'long' must be 32-bit (Windows LLP64).");

namespace {

constexpr wchar_t kDefaultInstallPath[] = L"C:\\Program Files\\LibreOffice\\program";
// Same rule as LibreOfficeKitInit.h: a sofficeapp.dll this small is the text stub of a merged build.
constexpr ULONGLONG kSofficeAppStubMaxSize = 1000;
constexpr char kFileUrlScheme[] = "file:";
constexpr int kBytesPerPixel = 4;
constexpr char kInvalidHandle[] = "Invalid or closed document handle.";
constexpr wchar_t kProfileEnvVar[] = L"LOK_VIEWER_PROFILE";
constexpr wchar_t kDefaultProfileSubdir[] = L"\\LokViewer\\lo-profile";

using LokHookFunction = LibreOfficeKit* (*)(const char* install_path);
using LokHookFunction2 = LibreOfficeKit* (*)(const char* install_path, const char* user_profile_url);

// Carries a LokResult code together with the text for lok_get_last_error().
class LokError : public std::runtime_error {
public:
    LokError(int code, const std::string& message) : std::runtime_error(message), code_(code) {}
    int code() const noexcept { return code_; }

private:
    int code_;
};

struct LokState {
    LibreOfficeKit* office = nullptr;
    bool started = false;  // the LOK hook ran once; LibreOffice cannot be started again in this process
    std::uintptr_t next_handle = 1;
    std::unordered_map<std::uintptr_t, LibreOfficeKitDocument*> docs;
};

std::mutex g_mutex;  // guards g_state and serializes all LOK calls
LokState g_state;

thread_local std::string t_last_error;

void set_error(const char* message) noexcept {
    try {
        t_last_error = (message && *message) ? message : "Unknown error.";
    } catch (const std::bad_alloc&) {
        t_last_error.clear();  // no memory for the text; the caller still gets the error code
    }
}

// Runs fn and turns any exception into a LokResult code plus a thread-local error message.
template <typename Fn>
int guarded(Fn&& fn) noexcept {
    try {
        return fn();
    } catch (const LokError& e) {
        set_error(e.what());
        return e.code();
    } catch (const std::bad_alloc&) {
        set_error("Out of memory.");
    } catch (const std::exception& e) {
        set_error(e.what());
    } catch (...) {
        set_error("Unexpected internal error.");
    }
    return LOK_E_INTERNAL;
}

std::wstring utf8_to_wide(const char* text) {
    const int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (len <= 0)
        throw LokError(LOK_E_INVALID_ARG, "String is not valid UTF-8.");
    std::wstring wide(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide.data(), len);
    wide.resize(static_cast<size_t>(len) - 1);  // drop the terminating null
    return wide;
}

std::string wide_to_utf8(const std::wstring& wide) {
    if (wide.empty())
        return {};
    if (wide.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        throw LokError(LOK_E_INVALID_ARG, "String is too long.");
    const int wide_len = static_cast<int>(wide.size());
    const int len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wide_len,
                                        nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        throw LokError(LOK_E_INVALID_ARG, "String cannot be converted to UTF-8.");
    std::string utf8(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wide_len,
                        utf8.data(), len, nullptr, nullptr);
    return utf8;
}

std::string win32_error_text(DWORD code) {
    std::string text = "Win32 error " + std::to_string(code);
    wchar_t* buffer = nullptr;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::unique_ptr<wchar_t, decltype(&LocalFree)> buffer_owner(buffer, &LocalFree);
    if (len > 0 && buffer) {
        std::wstring message(buffer, len);
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' ||
                                    message.back() == L' ' || message.back() == L'.'))
            message.pop_back();
        text += ": " + wide_to_utf8(message);
    }
    return text;
}

bool starts_with(const std::wstring& text, const wchar_t* prefix) {
    return text.compare(0, std::wcslen(prefix), prefix) == 0;
}

std::wstring full_path(const std::wstring& path) {
    DWORD len = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (len == 0) {
        const DWORD err = GetLastError();
        throw LokError(LOK_E_INVALID_ARG, "Invalid path: " + win32_error_text(err));
    }
    std::wstring full(len, L'\0');
    len = GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (len == 0 || len >= full.size()) {
        const DWORD err = GetLastError();
        throw LokError(LOK_E_INVALID_ARG, "Invalid path: " + win32_error_text(err));
    }
    full.resize(len);
    return full;
}

// RFC 3986 path characters that may stay unencoded ('%' is always encoded).
bool is_url_path_char(unsigned char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        return true;
    return c != '\0' && std::strchr("-._~!$&'()*+,;=:@/", c) != nullptr;
}

// Converts a path to a percent-encoded UTF-8 "file:" URL for LibreOffice.
// A raw Windows path is not safe as LOK input: "C:" can be read as a URL scheme,
// and '#', '%' or spaces in file names break an unencoded URL.
std::string to_file_url(const char* path) {
    if (_strnicmp(path, kFileUrlScheme, sizeof(kFileUrlScheme) - 1) == 0)
        return path;  // already a URL

    std::wstring full = full_path(utf8_to_wide(path));
    // Drop the long-path prefix: \\?\C:\x -> C:\x, \\?\UNC\srv\share -> \\srv\share
    if (starts_with(full, L"\\\\?\\UNC\\"))
        full.replace(0, std::wcslen(L"\\\\?\\UNC\\"), L"\\\\");
    else if (starts_with(full, L"\\\\?\\"))
        full.erase(0, std::wcslen(L"\\\\?\\"));

    const std::string utf8 = wide_to_utf8(full);
    std::string url = "file://";
    size_t start = 0;
    if (utf8.compare(0, 2, "\\\\") == 0)
        start = 2;    // UNC: \\server\share\x -> file://server/share/x
    else
        url += '/';   // drive: C:\x -> file:///C:/x

    static constexpr char kHex[] = "0123456789ABCDEF";
    for (size_t i = start; i < utf8.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c == '\\') {
            url += '/';
        } else if (is_url_path_char(c)) {
            url += static_cast<char>(c);
        } else {
            url += '%';
            url += kHex[c >> 4];
            url += kHex[c & 0x0F];
        }
    }
    return url;
}

std::wstring get_env(const wchar_t* name) {
    DWORD len = GetEnvironmentVariableW(name, nullptr, 0);
    if (len == 0)
        return {};
    std::wstring value(len, L'\0');
    len = GetEnvironmentVariableW(name, value.data(), len);
    value.resize(len);
    return value;
}

// LibreOfficeKit must not use the desktop LibreOffice profile: saved UI state there (e.g. the
// sidebar window state) makes documentLoad deadlock on Windows, and a running LibreOffice can lock it.
// Uses %LOK_VIEWER_PROFILE% (folder or file: URL) if set, else %LOCALAPPDATA%\LokViewer\lo-profile.
std::string lok_profile_url() {
    std::wstring dir = get_env(kProfileEnvVar);
    if (dir.empty()) {
        std::wstring base = get_env(L"LOCALAPPDATA");
        if (base.empty())
            base = get_env(L"TEMP");
        if (base.empty())
            throw LokError(LOK_E_INIT_FAILED, "No folder for the LibreOfficeKit profile: set LOK_VIEWER_PROFILE.");
        dir = base + kDefaultProfileSubdir;
    }
    return to_file_url(wide_to_utf8(dir).c_str());
}

// Loads the LibreOffice core library the same way LibreOfficeKitInit.h does:
// a real sofficeapp.dll first, otherwise mergedlo.dll.
HMODULE load_lok_library(const std::wstring& program_dir) {
    std::wstring library = program_dir + L"\\sofficeapp.dll";
    WIN32_FILE_ATTRIBUTE_DATA info{};
    const bool real_sofficeapp =
        GetFileAttributesExW(library.c_str(), GetFileExInfoStandard, &info) &&
        ((static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow) > kSofficeAppStubMaxSize;
    if (!real_sofficeapp)
        library = program_dir + L"\\mergedlo.dll";

    // LOAD_WITH_ALTERED_SEARCH_PATH: resolve the library's imports from its own folder.
    HMODULE module = LoadLibraryExW(library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        const DWORD err = GetLastError();
        throw LokError(LOK_E_INIT_FAILED,
                       "Failed to load " + wide_to_utf8(library) + ": " + win32_error_text(err));
    }
    return module;
}

// --- helpers below require g_mutex to be held ---

LibreOfficeKit* require_office() {
    if (!g_state.office)
        throw LokError(LOK_E_NOT_INITIALIZED, "LibreOfficeKit is not initialized. Call lok_viewer_init() first.");
    return g_state.office;
}

LibreOfficeKitDocument* require_doc(LokDocHandle handle) {
    if (!handle)
        throw LokError(LOK_E_INVALID_ARG, "Document handle is null.");
    const auto it = g_state.docs.find(reinterpret_cast<std::uintptr_t>(handle));
    if (it == g_state.docs.end())
        throw LokError(LOK_E_INVALID_ARG, kInvalidHandle);
    return it->second;
}

// Returns LOK's last error text and releases it with LOK's own allocator (different CRT).
std::string take_office_error(const char* fallback) {
    LibreOfficeKit* office = g_state.office;
    char* raw = office ? office->pClass->getError(office) : nullptr;
    if (!raw)
        return fallback;
    // freeError exists since LibreOffice 5.2; without it the text cannot be freed safely and leaks.
    auto free_error = [office](char* p) {
        if (LIBREOFFICEKIT_HAS(office, freeError))
            office->pClass->freeError(p);
    };
    std::unique_ptr<char, decltype(free_error)> raw_owner(raw, free_error);
    return *raw ? std::string(raw) : std::string(fallback);
}

} // namespace

extern "C" {

LOK_API const char* lok_get_last_error(void) {
    return t_last_error.empty() ? nullptr : t_last_error.c_str();
}

LOK_API int lok_viewer_init(const char* install_path) {
    return guarded([&]() -> int {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_state.office)
            return LOK_OK;  // already initialized
        if (g_state.started)
            throw LokError(LOK_E_INIT_FAILED,
                           "LibreOfficeKit was already started once in this process (destroyed or failed). "
                           "Restart the process.");

        std::wstring program_dir = (install_path && *install_path)
            ? utf8_to_wide(install_path)
            : std::wstring(kDefaultInstallPath);
        while (!program_dir.empty() && (program_dir.back() == L'\\' || program_dir.back() == L'/'))
            program_dir.pop_back();

        const DWORD attributes = GetFileAttributesW(program_dir.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
            throw LokError(LOK_E_INIT_FAILED, "LibreOffice program folder not found: " + wide_to_utf8(program_dir));

        // LibreOffice keeps loading DLLs from its program folder at run time.
        if (!SetDllDirectoryW(program_dir.c_str())) {
            const DWORD err = GetLastError();
            throw LokError(LOK_E_INIT_FAILED, "SetDllDirectoryW failed: " + win32_error_text(err));
        }

        // The module is never freed: LibreOffice cannot be unloaded from a running process.
        HMODULE module = load_lok_library(program_dir);
        auto hook2 = reinterpret_cast<LokHookFunction2>(GetProcAddress(module, "libreofficekit_hook_2"));
        auto hook = reinterpret_cast<LokHookFunction>(GetProcAddress(module, "libreofficekit_hook"));
        if (!hook2 && !hook)
            throw LokError(LOK_E_INIT_FAILED, "Neither libreofficekit_hook_2 nor libreofficekit_hook was found.");

        const std::string program_dir_utf8 = wide_to_utf8(program_dir);
        const std::string profile_url = lok_profile_url();
        g_state.started = true;
        // Very old LibreOffice versions only have libreofficekit_hook, which always uses the default profile.
        LibreOfficeKit* office = hook2 ? hook2(program_dir_utf8.c_str(), profile_url.c_str())
                                       : hook(program_dir_utf8.c_str());
        if (!office)
            throw LokError(LOK_E_INIT_FAILED, "LibreOfficeKit failed to start (hook returned NULL).");

        g_state.office = office;
        return LOK_OK;
    });
}

LOK_API void lok_viewer_destroy(void) {
    guarded([&]() -> int {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_state.office)
            return LOK_OK;

        // Documents must not outlive the office instance.
        for (auto& entry : g_state.docs)
            entry.second->pClass->destroy(entry.second);
        g_state.docs.clear();

        LibreOfficeKit* office = std::exchange(g_state.office, nullptr);
        office->pClass->destroy(office);
        return LOK_OK;
    });
}

LOK_API LokDocHandle lok_doc_open(const char* file_path) {
    LokDocHandle handle = nullptr;
    guarded([&]() -> int {
        if (!file_path || !*file_path)
            throw LokError(LOK_E_INVALID_ARG, "File path is empty.");
        const std::string url = to_file_url(file_path);

        std::lock_guard<std::mutex> lock(g_mutex);
        LibreOfficeKit* office = require_office();
        LibreOfficeKitDocument* doc = office->pClass->documentLoad(office, url.c_str());
        if (!doc)
            throw LokError(LOK_E_OPERATION_FAILED, "Failed to load document: " + take_office_error("unknown error"));

        try {
            // Required once before any tiled rendering call (paintTile, getDocumentSize, ...).
            doc->pClass->initializeForRendering(doc, "");
            const std::uintptr_t id = g_state.next_handle++;
            g_state.docs.emplace(id, doc);
            handle = reinterpret_cast<LokDocHandle>(id);
        } catch (...) {
            doc->pClass->destroy(doc);
            throw;
        }
        return LOK_OK;
    });
    return handle;
}

LOK_API void lok_doc_close(LokDocHandle handle) {
    if (!handle)
        return;
    guarded([&]() -> int {
        std::lock_guard<std::mutex> lock(g_mutex);
        LibreOfficeKitDocument* doc = require_doc(handle);
        g_state.docs.erase(reinterpret_cast<std::uintptr_t>(handle));
        doc->pClass->destroy(doc);
        return LOK_OK;
    });
}

LOK_API int lok_doc_get_parts(LokDocHandle handle) {
    int parts = 0;
    guarded([&]() -> int {
        std::lock_guard<std::mutex> lock(g_mutex);
        LibreOfficeKitDocument* doc = require_doc(handle);
        parts = doc->pClass->getParts(doc);
        return LOK_OK;
    });
    return parts;
}

LOK_API void lok_doc_set_part(LokDocHandle handle, int part) {
    guarded([&]() -> int {
        std::lock_guard<std::mutex> lock(g_mutex);
        LibreOfficeKitDocument* doc = require_doc(handle);
        doc->pClass->setPart(doc, part);
        return LOK_OK;
    });
}

LOK_API int lok_doc_get_size_twips(LokDocHandle handle, int32_t* out_width, int32_t* out_height) {
    return guarded([&]() -> int {
        if (!out_width || !out_height)
            throw LokError(LOK_E_INVALID_ARG, "Output pointer is null.");

        std::lock_guard<std::mutex> lock(g_mutex);
        LibreOfficeKitDocument* doc = require_doc(handle);
        long width = 0;
        long height = 0;
        doc->pClass->getDocumentSize(doc, &width, &height);
        *out_width = static_cast<int32_t>(width);
        *out_height = static_cast<int32_t>(height);
        return LOK_OK;
    });
}

LOK_API int lok_doc_paint_tile(
    LokDocHandle handle,
    unsigned char* buffer,
    int canvas_width,
    int canvas_height,
    int tile_pos_x,
    int tile_pos_y,
    int tile_width,
    int tile_height
) {
    return guarded([&]() -> int {
        if (!buffer)
            throw LokError(LOK_E_INVALID_ARG, "Buffer is null.");
        if (canvas_width <= 0 || canvas_height <= 0 || tile_width <= 0 || tile_height <= 0)
            throw LokError(LOK_E_INVALID_ARG, "Canvas and tile sizes must be positive.");
        // Sizes travel as int on both sides of this API; a tile buffer of 2 GB or more is never valid.
        if (static_cast<int64_t>(canvas_width) * canvas_height * kBytesPerPixel > std::numeric_limits<int32_t>::max())
            throw LokError(LOK_E_INVALID_ARG, "Canvas is too large.");

        std::lock_guard<std::mutex> lock(g_mutex);
        LibreOfficeKitDocument* doc = require_doc(handle);
        doc->pClass->paintTile(doc, buffer, canvas_width, canvas_height,
                               tile_pos_x, tile_pos_y, tile_width, tile_height);
        return LOK_OK;
    });
}

LOK_API int lok_doc_save_as(LokDocHandle handle, const char* out_path, const char* format, const char* filter_options) {
    return guarded([&]() -> int {
        if (!out_path || !*out_path)
            throw LokError(LOK_E_INVALID_ARG, "Output path is empty.");
        const std::string url = to_file_url(out_path);

        std::lock_guard<std::mutex> lock(g_mutex);
        LibreOfficeKitDocument* doc = require_doc(handle);
        if (!doc->pClass->saveAs(doc, url.c_str(), format, filter_options))
            throw LokError(LOK_E_OPERATION_FAILED, "Failed to save document: " + take_office_error("unknown error"));
        return LOK_OK;
    });
}

} // extern "C"
