#include "guest_async_files.h"
#include "guest_memory.h"
#include "asset_files.h"
#include "native_sync_objects.h"
#include "test_platform.h"
#include <atomic>
#include <filesystem>
#include <future>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
template<class F> void rejects(F action){
    try {action();} catch(const std::runtime_error&){return;}
    throw std::runtime_error("asynchronous read must reject before effects");
}
struct Fixture {
    std::filesystem::path root;
    std::vector<uint8_t> payload=std::vector<uint8_t>(14774);
    Fixture(){
        for(size_t i=0;i<payload.size();++i)payload[i]=uint8_t(i*37+19);
        for(unsigned attempt=0;attempt<100;++attempt){
            auto path=std::filesystem::temp_directory_path()/(
                "sfr-guest-async-"+std::to_string(GetCurrentProcessId())+"-"+
                std::to_string(GetTickCount64())+"-"+std::to_string(attempt));
            if(std::filesystem::create_directory(path)){root=path;break;}
        }
        require(!root.empty(),"create native asynchronous read fixture");
        std::ofstream stream(root/"read.bin",std::ios::binary);
        stream.write(reinterpret_cast<const char*>(payload.data()),payload.size());
        require(bool(stream),"write real synthetic fixture");
    }
    ~Fixture(){std::error_code error;std::filesystem::remove_all(root,error);}
};
constexpr uint32_t buffer=0x401a3834,io=0x82b50114,offset=0x74060c50,length=0x20000;
void prepare(sfr::GuestMemory& memory){
    memory.map(0x401a3000,0x22000);memory.map(0x82b50000,0x2000);memory.map(0x74060000,0x1000);
    for(uint32_t i=0;i<length;++i)memory.store<uint8_t>(buffer+i,0xa5);
    memory.store<uint64_t>(io,0x1122334455667788ull);memory.store<uint64_t>(offset,0);
}
void check_completed(sfr::GuestMemory& memory,const Fixture& fixture){
    require(memory.load<uint32_t>(io)==0 && memory.load<uint32_t>(io+4)==fixture.payload.size(),
            "real completion publishes final BE status and short byte count");
    for(uint32_t i=0;i<length;++i)
        require(memory.load<uint8_t>(buffer+i)==(i<fixture.payload.size()?fixture.payload[i]:0xa5),
                "completed bytes match the real file and preserve the unread tail");
}
void original_shape_publishes_before_retained_event(){
    Fixture fixture;sfr::GuestMemory memory;prepare(memory);sfr::AssetFiles files(fixture.root);
    sfr::NativeSyncObjects sync;sfr::GuestExecution execution;
    const auto file=files.open("game:/read.bin",1,sfr::AssetFiles::OpenMode::asynchronous_unbuffered);
    const auto event=sync.create_event(false,false).handle;
    const auto outer_event=sync.create_event(false,false).handle;
    auto retained=sync.retain_wait(event);
    unsigned offset_reads=0;memory.add_read_only_word(offset+4,[&]{++offset_reads;return 0u;});
    std::atomic<unsigned> observed=0;
    sfr::GuestAsyncFiles adapter(memory,files,sync,execution,[&](const auto& request,const auto& result){
        require(request.context==io && result.status==0 && result.transferred==fixture.payload.size(),
                "opaque APC context is preserved without being invoked");
        check_completed(memory,fixture);
        require(retained->wait(0).status==0x102 && sync.wait(outer_event,0)==0x102,
                "guest events cannot signal before publication; original outer completion stays untouched");
        ++observed;
    });
    auto permit=execution.enter(10);
    std::atomic<bool> other_ran=false;
    std::exception_ptr other_error;
    std::jthread other([&]{try{
        auto lease=execution.enter(11);
        memory.store<uint32_t>(0x82b51000,0x5a5a5a5a);
        other_ran=true;
    }catch(...){other_error=std::current_exception();}});
    struct Cleanup {
        sfr::GuestExecution& execution;
        std::unique_ptr<sfr::GuestExecution::Lease>& permit;
        std::jthread& thread;
        ~Cleanup(){execution.stop();permit.reset();if(thread.joinable())thread.join();}
    } cleanup{execution,permit,other};
    execution.wait_until_ready(11);
    const sfr::GuestFiles::ReadRequest request{file.handle,event,0,io,io,buffer,length,offset};
    const auto result=adapter.read(request,*permit);
    require(result.status==0 || result.status==0x103,"native submit returns actual completion or pending");
    require(offset_reads==1 && result.offset==0,"explicit offset is snapshotted exactly once before returning");
    require(other_ran,"native-only submission releases guest ownership for another actual guest thread");
    if(result.status==0x103){
        require(adapter.pending_count()==1 && observed==0 && memory.load<uint64_t>(io)==0x0000010300000000ull,
                "pending return precedes completion publication");
        rejects([&]{memory.store<uint8_t>(buffer,0);});
        rejects([&]{memory.decommit(0x401a3000,0x1000);});
        rejects([&]{files.close(file.handle);});
    }
    require(sync.close(event)==0,"guest may release event handle while retained completion ownership survives");
    sfr::NativeSyncObjects::WaitResult wait{};
    permit->run_blocking([&](std::stop_token stop){wait=retained->wait(5000,stop);});
    require(wait.status==0 && !wait.cancelled,"real retained event completes the guest wait");
    check_completed(memory,fixture);
    require(observed==1 && adapter.pending_count()==0 && sync.wait(outer_event,0)==0x102,
            "exactly one final publication releases request ownership without notifying the original outer waiter");
    files.close(file.handle);
    memory.store<uint8_t>(buffer,0x71);
    permit.reset();other.join();if(other_error)std::rethrow_exception(other_error);
    adapter.shutdown();execution.rethrow_failure();
    std::cout<<"guest async original-shape native_pending="<<(result.status==0x103)<<'\n';
}
void preflight_rejection_preserves_outputs_and_event(){
    Fixture fixture;sfr::GuestMemory memory;prepare(memory);sfr::AssetFiles files(fixture.root);
    sfr::NativeSyncObjects sync;sfr::GuestExecution execution;
    const auto file=files.open("game:/read.bin",1,sfr::AssetFiles::OpenMode::asynchronous_unbuffered);
    const auto event=sync.create_event(true,true).handle;
    sfr::GuestAsyncFiles adapter(memory,files,sync,execution);
    auto permit=execution.enter(10);
    const sfr::GuestFiles::ReadRequest request{file.handle,event,0,io,io,buffer,length,offset};
    for(unsigned field=0;field<6;++field){
        auto bad=request;
        switch(field){
        case 0:bad.apc=0x12340000;break;
        case 1:bad.io_output=buffer+4;break;
        case 2:bad.buffer=0xfffffff0;break;
        case 3:bad.offset_pointer=0;break;
        case 4:bad.io_output=0x82b51ffc;break;
        case 5:bad.length=1;break;
        }
        rejects([&]{adapter.read(bad,*permit);});
        require(memory.load<uint64_t>(io)==0x1122334455667788ull && memory.load<uint32_t>(buffer)==0xa5a5a5a5 &&
                sync.wait(event,0)==0 && adapter.pending_count()==0,
                "unsupported requests preserve all guest effects and retained request ownership");
    }
    files.close(file.handle);permit.reset();adapter.shutdown();
}
void global_stop_drains_without_late_guest_publication(){
    Fixture fixture;sfr::GuestMemory memory;prepare(memory);sfr::AssetFiles files(fixture.root);
    sfr::NativeSyncObjects sync;sfr::GuestExecution execution;
    const auto file=files.open("game:/read.bin",1,sfr::AssetFiles::OpenMode::asynchronous_unbuffered);
    const auto event=sync.create_event(false,false).handle;
    std::atomic<unsigned> publications=0;
    sfr::GuestAsyncFiles adapter(memory,files,sync,execution,[&](const auto&,const auto&){++publications;});
    auto permit=execution.enter(10);
    const auto result=adapter.read({file.handle,event,0,io,io,buffer,length,offset},*permit);
    execution.stop();permit.reset();adapter.shutdown();
    require(adapter.pending_count()==0,"shutdown drains every retained native request");
    if(result.status==0x103){
        require(publications==0 && memory.load<uint32_t>(buffer)==0xa5a5a5a5 &&
                memory.load<uint64_t>(io)==0x0000010300000000ull && sync.wait(event,0)==0x102,
                "shutdown does not publish or signal after execution has stopped");
    }else check_completed(memory,fixture);
    files.close(file.handle);memory.store<uint8_t>(buffer,1);
    execution.rethrow_failure();
    std::cout<<"guest async cancellation native_pending="<<(result.status==0x103)<<'\n';
}
void shutdown_waits_for_issuing_native_submission_scope(){
    Fixture fixture;sfr::GuestMemory memory;prepare(memory);sfr::AssetFiles files(fixture.root);
    sfr::NativeSyncObjects sync;sfr::GuestExecution execution;
    const auto file=files.open("game:/read.bin",1,sfr::AssetFiles::OpenMode::asynchronous_unbuffered);
    const auto event=sync.create_event(false,false).handle;
    std::promise<void> entered,release;
    auto entered_future=entered.get_future();auto release_future=release.get_future();
    sfr::GuestAsyncFiles adapter(memory,files,sync,execution,{},[&](std::stop_token){
        entered.set_value();release_future.wait(); // Deterministic native-only observation boundary.
    });
    std::exception_ptr issuing_error;
    std::jthread issuer([&]{try{
        auto permit=execution.enter(10);
        adapter.read({file.handle,event,0,io,io,buffer,length,offset},*permit);
    }catch(const sfr::GuestExecutionCancelled&){}catch(...){issuing_error=std::current_exception();}});
    entered_future.wait();
    std::promise<void> shutdown_done;auto done=shutdown_done.get_future();
    std::jthread shutdown([&]{adapter.shutdown();shutdown_done.set_value();});
    while(!execution.stopped())std::this_thread::yield();
    const bool premature=done.wait_for(std::chrono::milliseconds(50))==std::future_status::ready;
    release.set_value();issuer.join();shutdown.join();
    require(!premature,"shutdown must retain requests until their issuing native submission scope exits");
    if(issuing_error)std::rethrow_exception(issuing_error);
    require(adapter.pending_count()==0 && memory.load<uint32_t>(buffer)==0xa5a5a5a5 && sync.wait(event,0)==0x102,
            "interrupted submit drains ownership without late data publication or event signal");
    files.close(file.handle);memory.store<uint8_t>(buffer,1);
}
}
int main(){
    unsigned failed=0;
    for(auto test:{original_shape_publishes_before_retained_event,preflight_rejection_preserves_outputs_and_event,
                   global_stop_drains_without_late_guest_publication,shutdown_waits_for_issuing_native_submission_scope}){
        try{test();}catch(const std::exception& error){std::cerr<<error.what()<<'\n';++failed;}
    }
    return failed?1:0;
}
