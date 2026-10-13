#pragma once
#include <cstdint>
namespace sfr {
class GuestMemory;
// Diagnostic census only. Never used as proof that direct writers are absent,
// and never changes the existing write watches or rendering path.
bool buffer_audit_enabled();
void audit_buffer_draw(GuestMemory& memory, uint32_t device, bool indexed, bool up);
void audit_buffer_frame(uint32_t frame);
}
