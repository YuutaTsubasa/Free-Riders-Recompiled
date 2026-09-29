#include "shader_pack_format.h"
#include <iostream>
#include <vector>

void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}
void rejects(std::span<const uint8_t> bytes) {
    try { sfr::shader_pack_count(bytes); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("incompatible shader pack was accepted");
}
int main() {
    try {
        const std::vector<uint8_t> good{'S','F','R','S','H','P','K','2',9,0,0,0,0xD4,1,0,0};
        require(sfr::shader_pack_count(good) == 468, "current ABI count decoded");
        for (size_t n=0;n<good.size();++n) rejects(std::span(good).first(n));
        auto old=good;old[7]='1'; rejects(old);
        auto wrong=good;wrong[8]=6; rejects(wrong);
        wrong=good;wrong[9]=1; rejects(wrong);
        wrong=good;wrong[0]='X'; rejects(wrong);
        auto empty=good;
        for (size_t i=12;i<16;++i) empty[i]=0;
        rejects(empty);
        std::cout << "shader pack format tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
