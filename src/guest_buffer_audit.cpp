#include "ppc_recomp_shared.h"
#include "diagnostic_hooks.h"
#include "guest_buffer_audit.h"
#include "guest_graphics.h"
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <unordered_map>

// Plain forwards deliberately retain the original entry classification. An
// audit must not turn a buffer allocation or lock wait into a graphics hook.
PPC_FUNC_IMPL(__imp__sub_824F0F18);
PPC_FUNC_IMPL(__imp__sub_824F0FF0);
PPC_FUNC_IMPL(__imp__sub_824F1FD0);
PPC_FUNC_IMPL(__imp__sub_824F2020);
PPC_FUNC_IMPL(__imp__sub_824F0FE0);
PPC_FUNC_IMPL(__imp__sub_824F10A0);
PPC_FUNC_IMPL(__imp__sub_824F1F50);
PPC_FUNC_IMPL(__imp__sub_827FBF08);
PPC_FUNC_IMPL(__imp__sub_827FBF38);
PPC_FUNC_IMPL(__imp__sub_827FBFB8);

namespace sfr {
static GuestBufferState* managed_buffers() {
    return active_guest_graphics ? active_guest_graphics->buffers() : nullptr;
}
bool buffer_audit_enabled() {
    static const bool enabled=[] { const char* p=std::getenv("SFR_BUFFER_AUDIT"); return p && *p!='0'; }();
    return enabled;
}
namespace {
struct Descriptor {
    uint32_t kind, data, size, format;
    bool operator==(const Descriptor&) const = default;
};
Descriptor descriptor(GuestMemory& memory,uint32_t resource) {
    const uint32_t common=memory.load<uint32_t>(resource);
    const uint32_t kind=common&15;
    const uint32_t data=memory.load<uint32_t>(uint64_t(resource)+24);
    const uint32_t size=memory.load<uint32_t>(uint64_t(resource)+28);
    return {kind,kind==1?data&~3u:data,kind==1?size&0x03fffffcu:size,common&0x80000000u};
}
struct Resource {
    Descriptor description;
    uint64_t generation;
    uint64_t locks=0, unlocks=0;
    bool created=false;
    bool reported_without_unlock=false;
};
std::mutex audit_mutex;
std::unordered_map<uint32_t,Resource> resources;
uint64_t generation=0;
struct Counters {
    uint64_t created=0, discovered=0, replaced=0, released=0, locks=0, unlocks=0, nested_unlocks=0;
    uint64_t vb_draws=0, ib_draws=0, up_draws=0, created_binds=0, unlocked_binds=0, locked_binds=0;
} totals;
// Caller holds audit_mutex. Imported/serialized headers also get identities;
// seeing one never makes it eligible for managed rendering.
Resource& observe(GuestMemory& memory,uint32_t resource,bool created=false) {
    const Descriptor info=descriptor(memory,resource);
    auto found=resources.find(resource);
    if(found==resources.end() || created || found->second.description!=info) {
        if(found!=resources.end()) ++totals.replaced;
        if(created) ++totals.created; else ++totals.discovered;
        resources.insert_or_assign(resource,Resource{info,++generation,0,0,created});
    }
    return resources.at(resource);
}
void created(PPCContext& ctx) {
    if(!ctx.r3.u32) return;
    std::lock_guard guard(audit_mutex);
    observe(*active_memory,ctx.r3.u32,true);
}
void locked(PPCContext& ctx,uint32_t resource) {
    if(!ctx.r3.u32) return;
    std::lock_guard guard(audit_mutex);
    auto& entry=observe(*active_memory,resource);
    ++entry.locks; ++totals.locks;
}
void unlocked(uint32_t resource,uint32_t depth) {
    std::lock_guard guard(audit_mutex);
    auto& entry=observe(*active_memory,resource);
    ++totals.unlocks;
    if(depth==0x100) ++entry.unlocks;
    else ++totals.nested_unlocks;
}
void bind(GuestMemory& memory,uint32_t resource,bool index) {
    if(!resource) return;
    auto& entry=observe(memory,resource);
    ++(index?totals.ib_draws:totals.vb_draws);
    totals.created_binds+=entry.created;
    totals.unlocked_binds+=entry.unlocks!=0;
    totals.locked_binds+=(memory.load<uint32_t>(resource)&0xf00)!=0;
    if(!entry.unlocks && !entry.reported_without_unlock) {
        entry.reported_without_unlock=true;
        std::cerr << "BUFFER_WITHOUT_UNLOCK handle=0x" << std::hex << resource
            << " common=0x" << memory.load<uint32_t>(resource)
            << " backing=0x" << entry.description.data << std::dec
            << " bytes=" << entry.description.size << " kind=" << entry.description.kind
            << " generation=" << entry.generation << " locks=" << entry.locks
            << " created=" << entry.created << '\n';
    }
}
}
void audit_buffer_draw(GuestMemory& memory,uint32_t device,bool indexed,bool up) {
    std::lock_guard guard(audit_mutex);
    if(up) { ++totals.up_draws; return; }
    bind(memory,memory.load<uint32_t>(uint64_t(device)+12636),false);
    if(indexed) bind(memory,memory.load<uint32_t>(uint64_t(device)+12612),true);
}
void audit_buffer_frame(uint32_t frame) {
    if(frame%120) return;
    std::lock_guard guard(audit_mutex);
    std::cerr << "BUFFER_AUDIT frame=" << frame << " live=" << resources.size()
        << " created=" << totals.created << " discovered=" << totals.discovered
        << " replaced=" << totals.replaced << " released=" << totals.released
        << " locks=" << totals.locks << " unlocks=" << totals.unlocks
        << " nested_unlocks=" << totals.nested_unlocks
        << " vb_draws=" << totals.vb_draws << " ib_draws=" << totals.ib_draws << " up_draws=" << totals.up_draws
        << " created_binds=" << totals.created_binds << " unlocked_binds=" << totals.unlocked_binds
        << " locked_binds=" << totals.locked_binds << '\n';
}
}

PPC_FUNC(sub_824F0F18) {
    __imp__sub_824F0F18(ctx,base);
    if(sfr::buffer_audit_enabled()) sfr::created(ctx);
    if(auto* buffers=sfr::managed_buffers()) buffers->created(ctx.r3.u32);
}
PPC_FUNC(sub_824F0FF0) {
    __imp__sub_824F0FF0(ctx,base);
    if(sfr::buffer_audit_enabled()) sfr::created(ctx);
}
// The title's 8 MiB ring stores its VB at 83E59B88. Allocation writes the
// returned guest pointer through r3, consumes r4 bytes and increments Common's
// lock depth. Finish decrements that depth directly, bypassing public Unlock.
PPC_FUNC(sub_827FBF08) {
    const uint32_t resource=ctx.r3.u32;
    __imp__sub_827FBF08(ctx,base);
    if(auto* buffers=sfr::managed_buffers()) buffers->created(resource);
}
PPC_FUNC(sub_827FBF38) {
    const uint32_t output=ctx.r3.u32, bytes=ctx.r4.u32;
    auto* buffers=sfr::managed_buffers();
    const uint32_t resource=buffers?sfr::active_memory->load<uint32_t>(0x83E59B88):0;
    __imp__sub_827FBF38(ctx,base);
    if(buffers) {
        const uint32_t address=sfr::active_memory->load<uint32_t>(output);
        const uint32_t physical=(address&0x1fffffffu)+(address>=0xe0000000u?0x1000u:0u);
        buffers->begin_range(resource,physical,bytes);
    }
}
PPC_FUNC(sub_827FBFB8) {
    auto* buffers=sfr::managed_buffers();
    const uint32_t resource=buffers?sfr::active_memory->load<uint32_t>(0x83E59B88):0;
    const uint32_t depth=buffers?sfr::active_memory->load<uint32_t>(resource)&0xf00:0;
    __imp__sub_827FBFB8(ctx,base);
    if(buffers) buffers->finish_ranges(resource,depth);
}
PPC_FUNC(sub_824F1FD0) {
    const uint32_t resource=ctx.r3.u32;
    if(auto* buffers=sfr::managed_buffers()) buffers->lock(resource);
    __imp__sub_824F1FD0(ctx,base);
    if(sfr::buffer_audit_enabled()) sfr::locked(ctx,resource);
}
PPC_FUNC(sub_824F2020) {
    const uint32_t resource=ctx.r3.u32;
    __imp__sub_824F2020(ctx,base);
    if(sfr::buffer_audit_enabled()) sfr::locked(ctx,resource);
}
PPC_FUNC(sub_824F0FE0) {
    const uint32_t resource=ctx.r3.u32;
    auto* buffers=sfr::managed_buffers();
    const uint32_t depth=(sfr::buffer_audit_enabled() || buffers)?sfr::active_memory->load<uint32_t>(resource)&0xf00:0;
    __imp__sub_824F0FE0(ctx,base);
    if(sfr::buffer_audit_enabled()) sfr::unlocked(resource,depth);
    if(buffers) buffers->unlock(resource,depth);
}
PPC_FUNC(sub_824F10A0) {
    const uint32_t resource=ctx.r3.u32;
    const uint32_t depth=sfr::buffer_audit_enabled()?sfr::active_memory->load<uint32_t>(resource)&0xf00:0;
    __imp__sub_824F10A0(ctx,base);
    if(sfr::buffer_audit_enabled()) sfr::unlocked(resource,depth);
}
PPC_FUNC(sub_824F1F50) {
    const uint32_t resource=ctx.r3.u32;
    auto* buffers=sfr::managed_buffers();
    const uint64_t managed_identity=buffers?buffers->identity(resource):0;
    uint64_t identity=0;
    if(sfr::buffer_audit_enabled()) {
        std::lock_guard guard(sfr::audit_mutex);
        const auto found=sfr::resources.find(resource);
        if(found!=sfr::resources.end()) identity=found->second.generation;
    }
    __imp__sub_824F1F50(ctx,base);
    if(managed_identity && ctx.r3.u32==0) buffers->released(resource,managed_identity);
    if(identity && ctx.r3.u32==0) {
        std::lock_guard guard(sfr::audit_mutex);
        const auto found=sfr::resources.find(resource);
        if(found!=sfr::resources.end() && found->second.generation==identity) {
            sfr::resources.erase(found); ++sfr::totals.released;
        }
    }
}
