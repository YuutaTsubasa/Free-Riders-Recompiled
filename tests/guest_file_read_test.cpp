#include "guest_files.h"
#include "asset_files.h"
#include "guest_memory.h"
#include "test_platform.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if(!value) throw std::runtime_error(message); }
template<class F> void stop(F f) {
    try { f(); } catch(const sfr::RuntimeStop&) { return; }
    throw std::runtime_error("expected checked read rejection");
}
struct Fixture {
    std::filesystem::path root=std::filesystem::temp_directory_path()/
        ("sfr-guest-read-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    const std::array<uint8_t,9> bytes{'a',0,0x80,0xff,'z',1,2,3,4};
    Fixture() {
        require(std::filesystem::create_directory(root), "unique fixture directory");
        std::ofstream file(root/"data.bin",std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        require(bool(file), "write fixture bytes");
    }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root,ignored); }
};
}
int main() {
    try {
        Fixture fixture;
        sfr::AssetFiles files(fixture.root);
        auto opened=files.open("game:/data.bin",0);
        require(opened.status==0, "open actual read fixture");
        sfr::GuestMemory memory;
        memory.map(0x10000,0x1000);
        memory.add_read_only_word(0x10400,[]{return 0u;});
        sfr::GuestFiles guest(memory,files);
        constexpr uint32_t buffer=0x10100,io=0x10000,offset=0x10200;
        sfr::GuestFiles::ReadRequest request{opened.handle,0,0,0,io,buffer,3,0};
        auto reset=[&] {
            for(uint32_t i=0;i<32;++i) memory.store<uint8_t>(buffer+i,0xab);
            memory.store<uint64_t>(io,0x1122334455667788ull);
        };
        auto preserved=[&] {
            for(uint32_t i=0;i<32;++i) require(memory.load<uint8_t>(buffer+i)==0xab,"rejection preserves buffer");
            require(memory.load<uint64_t>(io)==0x1122334455667788ull,"rejection preserves IO block");
        };
        auto rejected=[&](const auto& bad) {
            const auto first=files.read(opened.handle,1,0);
            require(first.bytes.size()==1 && first.bytes[0]=='a',"reset native position");
            reset(); stop([&]{guest.read(bad);}); preserved();
            const auto second=files.read(opened.handle,1);
            require(second.offset==1 && second.bytes.size()==1 && second.bytes[0]==0,
                    "guest preflight failure preserves native position");
        };
        for(uint32_t mode=0;mode<3;++mode) {
            auto bad=request;
            if(mode==0) bad.event=1;
            if(mode==1) bad.apc=1;
            if(mode==2) bad.context=1;
            rejected(bad);
        }
        auto bad=request; bad.buffer=0x10ffe; rejected(bad);
        bad=request; bad.buffer=0x10400; rejected(bad);
        bad=request; bad.io_output=0x10ffc; rejected(bad);
        bad=request; bad.io_output=0x10400; rejected(bad);
        bad=request; bad.io_output=buffer+1; rejected(bad);
        bad=request; bad.offset_pointer=0x20000; rejected(bad);
        bad=request; bad.buffer=0; rejected(bad);
        memory.store<uint64_t>(offset,UINT64_MAX);
        bad=request; bad.offset_pointer=offset; rejected(bad);
        memory.store<uint64_t>(offset,0);
        request.offset_pointer=offset;
        reset(); auto result=guest.read(request);
        require(result.status==0 && result.transferred==3 && result.offset==0,"explicit first read result");
        require(memory.load<uint64_t>(io)==3,"BE IO status0 transferred3");
        for(uint32_t i=0;i<3;++i) require(memory.load<uint8_t>(buffer+i)==fixture.bytes[i],"exact binary bytes copied");
        require(memory.load<uint8_t>(buffer+3)==0xab,"read preserves untransferred bytes");
        request.offset_pointer=0; request.length=4;
        reset(); result=guest.read(request);
        require(result.status==0 && result.transferred==4 && result.offset==3,"sequential read position");
        for(uint32_t i=0;i<4;++i) require(memory.load<uint8_t>(buffer+i)==fixture.bytes[3+i],"sequential data");
        request.length=3;
        reset(); result=guest.read(request);
        require(result.status==0 && result.transferred==2 && result.offset==7,"partial end read succeeds");
        require(memory.load<uint64_t>(io)==2 && memory.load<uint8_t>(buffer)==3 && memory.load<uint8_t>(buffer+1)==4 &&
                memory.load<uint8_t>(buffer+2)==0xab,"partial copy boundary and IO count");
        reset(); result=guest.read(request);
        require(result.status==0xc0000011 && result.transferred==0 && result.offset==9,"EOF result");
        require(memory.load<uint64_t>(io)==0xc000001100000000ull && memory.load<uint8_t>(buffer)==0xab,"EOF IO and unchanged buffer");
        request.length=0; request.buffer=0; request.offset_pointer=offset;
        result=guest.read(request);
        require(result.status==0 && result.transferred==0 && result.offset==9 && memory.load<uint64_t>(io)==0,
                "zero read succeeds without seeking or accessing null buffer");
        request.length=2; request.buffer=buffer; request.io_output=0;
        memory.store<uint64_t>(offset,2); reset(); result=guest.read(request);
        require(result.status==0 && result.offset==2 && memory.load<uint8_t>(buffer)==0x80 && memory.load<uint8_t>(buffer+1)==0xff,
                "optional IO and explicit seek");
        require(memory.load<uint64_t>(io)==0x1122334455667788ull,"null IO preserves other memory");
        memory.store<uint64_t>(offset,UINT64_MAX-1);
        result=guest.read(request);
        require(result.offset==4 && memory.load<uint8_t>(buffer)=='z' && memory.load<uint8_t>(buffer+1)==1,
                "FILE_USE_FILE_POINTER_POSITION uses current pointer");
        request.io_output=io; request.offset_pointer=0; request.handle=0xffffffff;
        reset(); result=guest.read(request);
        require(result.status==0xc0000008 && result.transferred==0 && memory.load<uint64_t>(io)==0xc000000800000000ull &&
                memory.load<uint8_t>(buffer)==0xab,"invalid handle error IO without data");
        require(guest.close(opened.handle)==0 && files.open_count()==0,"guest close releases native file");
        stop([&]{guest.close(opened.handle);});
        const auto reopened=files.open("game:/data.bin",0);
        require(reopened.status==0,"guest close permits real exclusive reopen");
        files.close(reopened.handle);
        request.handle=opened.handle;
        require(guest.read(request).status==0xc0000008,"closed handle is invalid");
        std::cout<<"Guest file read checks passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
