#include "installer.h"

#include <windows.h>

#include <shlobj.h>

namespace flipfps {
namespace installer {
namespace {

constexpr const char* kRegKey = "Software\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
constexpr const char* kDllName = "XrApiLayer_flipfps.dll";
constexpr const char* kManifestName = "XrApiLayer_flipfps.json";

std::string exe_dir() {
    char buf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, buf, sizeof(buf));
    std::string path{buf};
    const auto slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

std::string install_dir() {
    char buf[MAX_PATH]{};
    if (GetEnvironmentVariableA("LOCALAPPDATA", buf, sizeof(buf)) == 0) {
        return {};
    }
    return std::string{buf} + "\\FlipFPS";
}

bool file_exists(const std::string& path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Short stamp identifying a build: size plus write time is enough to tell two
// builds apart without hashing megabytes on every launch.
std::string content_tag(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) {
        return "unknown";
    }
    const unsigned long long stamp =
        ((unsigned long long)data.ftLastWriteTime.dwHighDateTime << 32) ^
        data.ftLastWriteTime.dwLowDateTime ^ ((unsigned long long)data.nFileSizeLow << 16);
    char buf[24]{};
    sprintf_s(buf, "%011llx", stamp & 0xFFFFFFFFFFFull);
    return buf;
}

// The OpenXR loader silently skips a manifest that starts with a BOM, so this
// writes plain UTF-8, and via temp+move so a reader never sees a partial file.
bool write_manifest(const std::string& path, const std::string& library_name) {
    const std::string temp = path + ".tmp";
    FILE* f = nullptr;
    if (fopen_s(&f, temp.c_str(), "wb") != 0 || f == nullptr) {
        return false;
    }
    fprintf(f,
            "{\n"
            "    \"file_format_version\": \"1.0.0\",\n"
            "    \"api_layer\": {\n"
            "        \"name\": \"XR_APILAYER_ELLIOTTTATE_flipfps\",\n"
            "        \"library_path\": \".\\\\%s\",\n"
            "        \"api_version\": \"1.0\",\n"
            "        \"implementation_version\": \"1\",\n"
            "        \"description\": \"FlipFPS: turn a controller upside down to see an FPS/frametime "
            "HUD in any OpenXR app\",\n"
            "        \"disable_environment\": \"FLIPFPS_DISABLE\"\n"
            "    }\n"
            "}\n",
            library_name.c_str());
    fclose(f);
    return MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// Old versioned DLLs linger only until nothing has them mapped; delete quietly
// and leave the ones still in use for the next run.
void prune_old_layer_dlls(const std::string& dir, const std::string& keep) {
    WIN32_FIND_DATAA find{};
    HANDLE h = FindFirstFileA((dir + "\\XrApiLayer_flipfps.*.dll").c_str(), &find);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (keep != find.cFileName) {
            DeleteFileA((dir + "\\" + find.cFileName).c_str());
        }
    } while (FindNextFileA(h, &find));
    FindClose(h);
}

} // namespace

Status query() {
    Status status{};
    status.install_dir = install_dir();
    status.manifest_path = status.install_dir + "\\" + kManifestName;

    WIN32_FIND_DATAA find{};
    HANDLE h = FindFirstFileA((status.install_dir + "\\XrApiLayer_flipfps*.dll").c_str(), &find);
    const bool any_dll = h != INVALID_HANDLE_VALUE;
    if (any_dll) {
        FindClose(h);
    }
    status.files_present = file_exists(status.manifest_path) && any_dll;

    HKEY key{};
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD type = 0;
        DWORD value = 0;
        DWORD size = sizeof(value);
        // Present and zero means enabled; the loader treats non-zero as disabled.
        const LSTATUS r =
            RegQueryValueExA(key, status.manifest_path.c_str(), nullptr, &type, (LPBYTE)&value, &size);
        status.installed = r == ERROR_SUCCESS && value == 0;
        if (r == ERROR_SUCCESS && value != 0) {
            status.detail = "registered but disabled by its registry value";
        }
        RegCloseKey(key);
    }

    if (status.installed && !status.files_present) {
        status.detail = "registered, but the layer files are missing";
    }
    return status;
}

bool install(std::string* error, const std::string& source_dir) {
    const std::string src = source_dir.empty() ? exe_dir() : source_dir;
    const std::string dst = install_dir();
    if (dst.empty()) {
        if (error != nullptr) {
            *error = "cannot resolve %LOCALAPPDATA%";
        }
        return false;
    }
    CreateDirectoryA(dst.c_str(), nullptr);

    const std::string src_dll = src + "\\" + kDllName;
    const std::string src_manifest = src + "\\" + kManifestName;
    if (!file_exists(src_dll) || !file_exists(src_manifest)) {
        if (error != nullptr) {
            *error = "layer files not found next to the Control Center (" + src + ")";
        }
        return false;
    }

    // A game that is running right now holds its layer DLL open, so installing
    // over the same filename would fail. Each build is installed under a
    // content-derived name and the manifest points at that one; the running
    // game keeps its old file, and the next launch picks up the new one.
    const std::string versioned = "XrApiLayer_flipfps." + content_tag(src_dll) + ".dll";
    const std::string dst_dll = dst + "\\" + versioned;
    const std::string dst_manifest = dst + "\\" + kManifestName;

    if (!file_exists(dst_dll) && CopyFileA(src_dll.c_str(), dst_dll.c_str(), FALSE) == 0) {
        if (error != nullptr) {
            *error = "could not copy the layer into " + dst;
        }
        return false;
    }
    prune_old_layer_dlls(dst, versioned);

    if (!write_manifest(dst_manifest, versioned)) {
        if (error != nullptr) {
            *error = "could not write the layer manifest";
        }
        return false;
    }

    HKEY key{};
    if (RegCreateKeyExA(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = "could not open the OpenXR implicit-layer registry key";
        }
        return false;
    }
    const DWORD zero = 0;
    const LSTATUS r =
        RegSetValueExA(key, dst_manifest.c_str(), 0, REG_DWORD, (const BYTE*)&zero, sizeof(zero));
    RegCloseKey(key);

    if (r != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = "could not write the registry value";
        }
        return false;
    }
    return true;
}

bool uninstall(std::string* error) {
    const std::string dst = install_dir();
    const std::string manifest = dst + "\\" + kManifestName;

    HKEY key{};
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kRegKey, 0, KEY_WRITE, &key) == ERROR_SUCCESS) {
        RegDeleteValueA(key, manifest.c_str());
        RegCloseKey(key);
    }

    // Leave the files in place: a game running right now still has the DLL
    // mapped, and an orphaned copy on disk is harmless once unregistered.
    (void)error;
    return true;
}

} // namespace installer
} // namespace flipfps
