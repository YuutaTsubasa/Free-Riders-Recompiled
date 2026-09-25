#pragma once
#include <memory>

namespace plume { struct VulkanDevice; }
namespace sfr {
// Owns persistence, not the Vulkan cache handle. Must die before the device.
class NativePipelineCache {
public:
    explicit NativePipelineCache(plume::VulkanDevice& device);
    ~NativePipelineCache();
    NativePipelineCache(const NativePipelineCache&) = delete;
    NativePipelineCache& operator=(const NativePipelineCache&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
