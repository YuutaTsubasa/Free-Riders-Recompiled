// Translation-only adapter. Input is prevalidated by scripts/prepare_shaders.py.
#include "shader_recompiler.h"
#include "shader_inputs.h"
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--dec3n-variant") {
        std::ifstream input(argv[2], std::ios::binary);
        if (!input) return 3;
        const std::string legacy((std::istreambuf_iterator<char>(input)), {});
        const auto variant = sfr::dec3n_vertex_shader_source(legacy);
        if (!variant) { std::cerr << "unrecognized vertex HLSL for DEC3N variant\n"; return 3; }
        std::ofstream output(argv[3], std::ios::binary | std::ios::trunc);
        output << *variant;
        output.close();
        return output ? 0 : 4;
    }
    if (argc != 4) {
        std::cerr << "usage: shader_translate original.bin shader.hlsl shader_common.h | --dec3n-variant input.hlsl output.hlsl\n";
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
