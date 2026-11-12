#include "sound_heap.h"

namespace sfr {
SoundHeapRegion sound_heap_region(uint32_t buffer, uint32_t size, const std::function<uint32_t(uint32_t)>& allocate) {
    if (buffer != sound_heap_console_buffer || size != sound_heap_console_size || !allocate) return {buffer, size, false};
    const uint32_t block = allocate(sound_heap_size);
    if (!block) return {buffer, size, false};
    return {block, sound_heap_size, true};
}
}
