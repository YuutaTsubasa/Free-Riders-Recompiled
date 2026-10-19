#pragma once

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace sfr {

// The bundled MoltenVK preload_bundled_vulkan() loaded, or null.
inline void*& bundled_vulkan_handle() {
    static void* handle = nullptr;
    return handle;
}

// In FreeRidersRecompiled.app the Vulkan driver, MoltenVK, sits in
// Contents/Frameworks, where volk's dlopen("libMoltenVK.dylib") does not
// look. It is loaded here by its full path, and Plume takes Vulkan from it
// directly (bundled_vulkan_get_instance_proc_addr). Without a bundled copy,
// volk finds a driver as before (DYLD_LIBRARY_PATH, a Vulkan SDK's loader).
inline void preload_bundled_vulkan() {
#if TARGET_OS_IPHONE
    // iOS refuses GPU work from an app in the background, and MoltenVK takes
    // that refused command buffer for a lost device, for good. With this it
    // carries on once the app is back in front (unless the player set it).
    setenv("MVK_CONFIG_RESUME_LOST_DEVICE", "1", 0);
#endif
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
        if (std::filesystem::exists(candidate, error))
            if (void* handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_GLOBAL)) {
                bundled_vulkan_handle() = handle;
                return;
            }
}

// The bundled MoltenVK's vkGetInstanceProcAddr (a PFN_vkGetInstanceProcAddr),
// or null. volk on its own tries a Vulkan loader first (libvulkan.dylib, then
// /usr/local/lib/libvulkan.dylib), so on a Mac with the Vulkan SDK installed
// the SDK's loader and MoltenVK ran the game, whatever their version: a
// MoltenVK before 1.3.0 counts the attributes of the zero-stride vertex
// binding (native_renderer.cpp) as needing buffers of their own, ran out of
// Metal buffer indexes, and every pipeline failed to compile (Issue #207).
inline void* bundled_vulkan_get_instance_proc_addr() {
    void* handle = bundled_vulkan_handle();
    return handle ? dlsym(handle, "vkGetInstanceProcAddr") : nullptr;
}

}  // namespace sfr
#endif
