#include "native_pipeline_cache.h"
#include "pipeline_cache_file.h"
#include "plume_vulkan.h"
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>

namespace sfr {
struct NativePipelineCache::Impl {
    plume::VulkanDevice& device;
    std::filesystem::path path;
    uint64_t saved_generation = 0;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::thread worker;

    explicit Impl(plume::VulkanDevice& d) : device(d) {
        const char* override_path = std::getenv("SFR_PIPELINE_CACHE_PATH");
        path = override_path && *override_path ? std::filesystem::u8path(override_path) :
            std::filesystem::path("pipeline-cache") / "vulkan.bin";
        auto data = load_pipeline_cache_file(path);
        const auto& properties = device.physicalDeviceProperties;
        PipelineCacheIdentity identity{properties.vendorID, properties.deviceID, {}};
        std::memcpy(identity.uuid.data(), properties.pipelineCacheUUID, identity.uuid.size());
        if (!compatible_pipeline_cache(data, identity)) data.clear();
        VkPipelineCacheCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        info.initialDataSize = data.size();
        info.pInitialData = data.empty() ? nullptr : data.data();
        // Default flags provide internal synchronization with pipeline creation.
        auto result = vkCreatePipelineCache(device.vk, &info, nullptr, &device.pipelineCache);
        if (result != VK_SUCCESS && !data.empty()) {
            data.clear();
            info.initialDataSize = 0;
            info.pInitialData = nullptr;
            result = vkCreatePipelineCache(device.vk, &info, nullptr, &device.pipelineCache);
        }
        if (result != VK_SUCCESS) device.pipelineCache = VK_NULL_HANDLE;
        std::ostringstream message;
        message << "NATIVE_PIPELINE_CACHE enabled=" << (device.pipelineCache != VK_NULL_HANDLE)
                << " loaded_bytes=" << data.size() << " result=" << result
                << " path=" << path.generic_string() << '\n';
        std::cerr << message.str();
        if (device.pipelineCache != VK_NULL_HANDLE) worker = std::thread([this] { run(); });
    }
    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_one();
        if (worker.joinable()) worker.join();
    }
    void save() {
        const auto generation = device.pipelineCacheGeneration.load(std::memory_order_relaxed);
        if (generation == saved_generation) return;
        const auto started = std::chrono::steady_clock::now();
        // Creation may grow the cache between the size query and copy. Retry a
        // bounded number of times; never persist a partial or failed snapshot.
        for (int attempt = 0; attempt < 3; ++attempt) {
            size_t size = 0;
            if (vkGetPipelineCacheData(device.vk, device.pipelineCache, &size, nullptr) != VK_SUCCESS ||
                !size || size > pipeline_cache_max_bytes) return;
            std::vector<uint8_t> data(size);
            const auto result = vkGetPipelineCacheData(device.vk, device.pipelineCache, &size, data.data());
            if (result == VK_INCOMPLETE) continue;
            if (result != VK_SUCCESS || size > data.size()) return;
            data.resize(size);
            const bool saved = save_pipeline_cache_file(path, data);
            if (saved) saved_generation = generation;
            std::ostringstream message;
            message << "NATIVE_PIPELINE_CACHE saved=" << saved << " bytes=" << size
                    << " generation=" << generation << " save_ms="
                    << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()
                    << '\n';
            std::cerr << message.str();
            return;
        }
    }
    void run() {
        for (;;) {
            bool stop;
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock, std::chrono::seconds(10), [this] { return stopping; });
                stop = stopping;
            }
            try { save(); }
            catch (const std::exception& e) {
                std::cerr << std::string("NATIVE_PIPELINE_CACHE save_failed=") + e.what() + '\n';
            }
            if (stop) return;
        }
    }
};
NativePipelineCache::NativePipelineCache(plume::VulkanDevice& device) : impl_(std::make_unique<Impl>(device)) {}
NativePipelineCache::~NativePipelineCache() = default;
}
