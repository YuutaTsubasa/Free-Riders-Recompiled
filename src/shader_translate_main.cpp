// Translation-only adapter. Input is prevalidated by scripts/prepare_shaders.py.
#include "shader_recompiler.h"
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: shader_translate original.bin shader.hlsl shader_common.h\n";
        return 2;
    }
    std::ifstream source(argv[1], std::ios::binary), common(argv[3], std::ios::binary);
    if (!source || !common) return 3;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(source)), {});
    std::string include((std::istreambuf_iterator<char>(common)), {});
    if (data.size() < 36 || include.empty()) return 3;
    ShaderRecompiler recompiler;
    recompiler.recompile(data.data(), include);
    std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
    output << recompiler.out;
    output.close();
    if (!output) return 4;
    std::cout << (recompiler.isPixelShader ? "pixel" : "vertex")
              << " HLSL bytes=" << recompiler.out.size()
              << " spec_constants_mask=" << recompiler.specConstantsMask << '\n';
    return 0;
}
