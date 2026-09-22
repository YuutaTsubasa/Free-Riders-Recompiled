#include "native_sync_objects.h"
#include "guest_memory.h"
#include <iostream>
#include <stdexcept>

static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> static void rejects(F f) {
    try { f(); } catch(const sfr::RuntimeStop&) { return; }
    throw std::runtime_error("wrong event capability accepted");
}
int main() {
    try {
        sfr::NativeSyncObjects native;
        const auto listener=native.create_notification_event();
        require(listener.status==0 && listener.handle && native.owns(listener.handle), "owned listener created");
        require(native.wait(listener.handle,0)==0x102, "listener starts genuinely nonsignaled");
        require(native.set_event(listener.handle)==0xc0000024 && native.reset_event(listener.handle)==0xc0000024,
                "ordinary event APIs cannot mutate notification state");
        require(native.release_semaphore(listener.handle,1).status==0xc0000024, "not a semaphore");
        rejects([&]{ native.retain_event(listener.handle); });
        const auto ordinary=native.create_event(true,false);
        rejects([&]{ native.retain_notification_event(ordinary.handle); });
        require(!native.retain_notification_event(0), "invalid listener has no capability");
        auto producer=native.retain_notification_event(listener.handle);
        auto waiter=native.retain_wait(listener.handle);
        producer->signal();
        require(waiter->wait(0).status==0 && waiter->wait(0).status==0, "manual event retains signal across waits");
        require(native.reset_event(listener.handle)==0xc0000024 && waiter->wait(0).status==0,
                "ordinary reset cannot hide queued work");
        producer->reset();
        require(waiter->wait(0).status==0x102, "queue owner clears signal");
        require(native.close(listener.handle)==0 && !native.owns(listener.handle), "guest close removes listener handle");
        require(!native.retain_notification_event(listener.handle), "closed handle cannot be reacquired");
        producer->signal();
        require(waiter->wait(0).status==0, "retained native capabilities survive guest close");
        producer.reset();
        require(waiter->wait(0).status==0, "retained waiter owns its native lifetime");
        std::cout << "Native notification event checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
