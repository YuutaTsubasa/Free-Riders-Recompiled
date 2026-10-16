#pragma once

#if defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

namespace sfr {

// In FreeRidersRecompiled.app the Vulkan driver, MoltenVK, sits in
// Contents/Frameworks, where volk's dlopen("libMoltenVK.dylib") does not
// look. Loaded here by its full path first, that same dlopen returns it.
// Without a bundled copy, volk finds one through DYLD_LIBRARY_PATH as before.
inline void preload_bundled_vulkan() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0)
        return;
    path.resize(std::strlen(path.c_str()));
    std::error_code error;
    const auto directory = std::filesystem::weakly_canonical(path, error).parent_path();
    for (const auto& candidate : {directory / "../Frameworks/libMoltenVK.dylib", directory / "Frameworks/libMoltenVK.dylib",
                                   directory / "libMoltenVK.dylib"})
        if (std::filesystem::exists(candidate, error) && dlopen(candidate.c_str(), RTLD_NOW | RTLD_GLOBAL))
            return;
}

}  // namespace sfr
#endif
