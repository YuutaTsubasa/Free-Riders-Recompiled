#include "guest_async_files.h"
#include "guest_memory.h"
#include "asset_files.h"
#include "native_sync_objects.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace sfr {
struct GuestAsyncFiles::Impl {
    struct Operation {
        enum class Dispatch { waiting, pending, cancel };
        GuestFiles::ReadRequest request;
        uint64_t offset;
        GuestMemory::WriteLease outputs;
        std::unique_ptr<NativeSyncObjects::RetainedEvent> event;
        std::unique_ptr<AssetFiles::AsyncRead> native;
        std::mutex mutex;
        std::condition_variable changed;
        Dispatch dispatch=Dispatch::waiting;
        std::atomic<bool> finished=false;
        // Destroy/join the worker before its native request, staging and pins.
        std::jthread worker;
        Operation(GuestFiles::ReadRequest r,uint64_t position,GuestMemory::WriteLease pin,
                  std::unique_ptr<NativeSyncObjects::RetainedEvent> retained,
                  std::unique_ptr<AssetFiles::AsyncRead> io)
            :request(r),offset(position),outputs(std::move(pin)),event(std::move(retained)),native(std::move(io)){}
        void wake(Dispatch value) {
            std::lock_guard lock(mutex);
            if(dispatch==Dispatch::waiting)dispatch=value;
            changed.notify_all();
        }
    };
    GuestMemory& memory;
    AssetFiles& files;
    NativeSyncObjects& sync;
    GuestExecution& execution;
    CompletionObserver observer;
    SubmissionObserver submission_observer;
    std::vector<std::unique_ptr<Operation>> operations;
    std::mutex calls_mutex;
    std::condition_variable calls_drained;
    size_t active_calls=0;
    bool accepting=true;
    std::mutex shutdown_mutex;
    bool stopped=false;
    struct Call {
        Impl& service;
        explicit Call(Impl& owner):service(owner){
            std::lock_guard lock(service.calls_mutex);
            if(!service.accepting)throw GuestExecutionCancelled();
            ++service.active_calls;
        }
        ~Call(){
            std::lock_guard lock(service.calls_mutex);
            --service.active_calls;
            service.calls_drained.notify_all();
        }
    };
    Impl(GuestMemory& m,AssetFiles& f,NativeSyncObjects& s,GuestExecution& e,CompletionObserver o,SubmissionObserver submit)
        :memory(m),files(f),sync(s),execution(e),observer(std::move(o)),submission_observer(std::move(submit)){}

    GuestFiles::ReadResult publish(Operation& operation) {
        const auto result=operation.native->result();
        const auto& request=operation.request;
        if(result.offset!=operation.offset || result.transferred>request.length ||
           result.bytes.size()!=result.transferred || (result.status && result.transferred))
            throw RuntimeStop("async-file-read",request.handle,"inconsistent native completion result");
        operation.outputs.check(); // Validate every retained output before the first byte changes.
        operation.outputs.write_bytes(request.buffer,std::span<const uint8_t>(result.bytes.data(),result.transferred));
        operation.outputs.store<uint32_t>(request.io_output,result.status);
        operation.outputs.store<uint32_t>(uint64_t(request.io_output)+4,result.transferred);
        const GuestFiles::ReadResult completed{result.status,result.transferred,result.offset};
        if(observer)observer(request,completed);
        // No more reads use the staging span. Release file request ownership
        // before waking code that may immediately close the guest file handle.
        operation.native.reset();
        operation.outputs.reset();
        operation.event->signal();
        return completed;
    }
    void run(Operation& operation,std::stop_token stop) noexcept {
        try {
            {
                std::stop_callback wake(stop,[&]{
                    std::lock_guard lock(operation.mutex);
                    operation.changed.notify_all();
                });
                std::unique_lock lock(operation.mutex);
                operation.changed.wait(lock,[&]{return stop.stop_requested() ||
                    operation.dispatch!=Operation::Dispatch::waiting;});
                if(stop.stop_requested() || operation.dispatch==Operation::Dispatch::cancel){
                    operation.finished.store(true,std::memory_order_release);
                    return;
                }
            }
            operation.native->wait(stop); // Native-only: private event, staging and OVERLAPPED.
            {
                auto permit=execution.enter_completion();
                publish(operation);
            }
        }catch(const GuestExecutionCancelled&){
            if(!execution.stopped())execution.fail(std::current_exception());
        }catch(...){execution.fail(std::current_exception());}
        // Reaping may join this thread under a guest lease only after the
        // completion lease above has been destroyed.
        operation.finished.store(true,std::memory_order_release);
    }
    GuestFiles::ReadResult read(const GuestFiles::ReadRequest& request,GuestExecution::Lease& permit) {
        permit.checkpoint(false);
        if(request.apc || !request.event || !request.io_output || !request.offset_pointer)
            throw RuntimeStop("async-file-read",request.handle,"requires an event, IO block, explicit offset and no guest APC");
        if(request.length)memory.check_write(request.buffer,request.length);
        memory.check_write(request.io_output,8);
        if(request.length && uint64_t(request.buffer)<uint64_t(request.io_output)+8 &&
           uint64_t(request.io_output)<uint64_t(request.buffer)+request.length)
            throw RuntimeStop("async-file-read",request.io_output,"read buffer overlaps the IO status block");
        const uint64_t offset=memory.load<uint64_t>(request.offset_pointer);
        if(!files.owns(request.handle))return {0xc0000008,0,offset};
        auto event=sync.retain_event(request.event);
        if(!event)return {0xc0000008,0,offset};
        auto native=files.prepare_async_read(request.handle,request.length,offset);
        const std::array ranges{GuestMemory::Range{request.buffer,request.length},
                                GuestMemory::Range{request.io_output,8}};
        auto outputs=memory.pin_writes(std::span(ranges).subspan(request.length?0:1));
        std::erase_if(operations,[](const auto& op){return op->finished.load(std::memory_order_acquire);});
        auto operation=std::make_unique<Operation>(request,offset,std::move(outputs),std::move(event),std::move(native));
        operations.reserve(operations.size()+1);
        auto* const active=operation.get();
        operations.push_back(std::move(operation));
        try {
            active->worker=std::jthread([this,active](std::stop_token stop){run(*active,stop);});
        }catch(...){operations.pop_back();throw;}
        AssetFiles::SubmitResult submitted;
        try {
            permit.run_blocking([&](std::stop_token stop){
                if(stop.stop_requested())throw GuestExecutionCancelled();
                if(submission_observer)submission_observer(stop);
                if(stop.stop_requested())throw GuestExecutionCancelled();
                submitted=active->native->submit();
            },[&]{
                // Scheduler admission allocations have also completed, and
                // the original caller still exclusively owns guest execution.
                active->outputs.check();
                active->event->reset();
                active->outputs.store<uint32_t>(request.io_output,0x103);
                active->outputs.store<uint32_t>(uint64_t(request.io_output)+4,0);
            });
        }catch(...){
            // Even if cancellation denied permit reacquisition, only touch the
            // host dispatch gate. Shutdown retains and drains any submitted I/O.
            active->wake(Operation::Dispatch::cancel);
            throw;
        }
        if(submitted==AssetFiles::SubmitResult::pending){
            active->wake(Operation::Dispatch::pending);
            return {0x103,0,offset};
        }
        active->wake(Operation::Dispatch::cancel);
        return publish(*active);
    }
};
GuestAsyncFiles::GuestAsyncFiles(GuestMemory& memory,AssetFiles& files,NativeSyncObjects& sync,
                               GuestExecution& execution,CompletionObserver observer,SubmissionObserver submission)
    :impl_(std::make_unique<Impl>(memory,files,sync,execution,std::move(observer),std::move(submission))){}
GuestAsyncFiles::~GuestAsyncFiles(){shutdown();}
GuestFiles::ReadResult GuestAsyncFiles::read(const GuestFiles::ReadRequest& request,GuestExecution::Lease& permit) {
    Impl::Call call(*impl_);
    try{return impl_->read(request,permit);}
    catch(const RuntimeStop&){throw;}
    catch(const GuestExecutionCancelled&){throw;}
    catch(const std::exception& error){throw RuntimeStop("async-file-read",request.handle,error.what());}
}
void GuestAsyncFiles::shutdown() noexcept {
    if(!impl_)return;
    std::lock_guard shutdown_lock(impl_->shutdown_mutex);
    if(impl_->stopped)return;
    {
        std::lock_guard lock(impl_->calls_mutex);
        impl_->accepting=false;
    }
    impl_->execution.stop_and_drain();
    {
        // A stopped scheduler can still have an issuing host inside submit or
        // unwinding after denied permit reacquisition. Its raw operation stays
        // owned until the complete public read call exits.
        std::unique_lock lock(impl_->calls_mutex);
        impl_->calls_drained.wait(lock,[&]{return impl_->active_calls==0;});
    }
    for(auto& operation:impl_->operations)operation->worker.request_stop();
    for(auto& operation:impl_->operations){
        if(operation->worker.joinable())operation->worker.join();
    }
    // Destruction drains even a request submitted just before global stop
    // denied the original caller's permit reacquisition.
    impl_->operations.clear();
    impl_->stopped=true;
}
size_t GuestAsyncFiles::pending_count() const {
    return std::count_if(impl_->operations.begin(),impl_->operations.end(),[](const auto& op){return bool(op->native);});
}
}
