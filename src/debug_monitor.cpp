#include "debug_monitor.h"
namespace sfr {
AbsentDebugMonitor::AbsentDebugMonitor(GuestMemory& memory) {
    // KeDebugMonitorData is a pointer variable: the import references this
    // readable cell, whose null value means no Xbox monitor/callback is installed.
    memory.map(address, sizeof(uint32_t));
    memory.add_read_only_word(address, [] { return 0u; }, GuestMemory::ProviderAccess::concurrent);
}
}
