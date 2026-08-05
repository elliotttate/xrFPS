#pragma once

#include <string>

namespace flipfps {

// Registers/unregisters the layer as a per-user implicit OpenXR API layer.
// Everything lives under HKCU, so no elevation is ever required.
namespace installer {

struct Status {
    bool installed{false};
    bool files_present{false};
    std::string manifest_path;
    std::string install_dir;
    std::string detail;
};

Status query();

// Copies the layer DLL + manifest next to the Control Center's own install
// directory and adds the HKCU registry value. `source_dir` is where the built
// DLL/manifest live (defaults to the executable's directory).
bool install(std::string* error, const std::string& source_dir = {});
bool uninstall(std::string* error);

} // namespace installer
} // namespace flipfps
