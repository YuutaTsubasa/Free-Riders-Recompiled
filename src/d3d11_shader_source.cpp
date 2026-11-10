#include "d3d11_shader_source.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <string_view>
#include <vector>

namespace sfr {
namespace {
size_t replace_all(std::string& text, std::string_view from, std::string_view to) {
    size_t count = 0;
    for (size_t at = 0; (at = text.find(from, at)) != std::string::npos; at += to.size(), ++count)
        text.replace(at, from.size(), to);
    return count;
}

bool identifier_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Every whole-word `word` renamed.
void rename_word(std::string& text, std::string_view word, std::string_view to) {
    for (size_t at = 0; (at = text.find(word, at)) != std::string::npos;) {
        const bool before = at > 0 && identifier_char(text[at - 1]);
        const bool after = at + word.size() < text.size() && identifier_char(text[at + word.size()]);
        if (before || after) {
            at += word.size();
            continue;
        }
        text.replace(at, word.size(), to);
        at += to.size();
    }
}

// XenosRecomp's dispatch loop as guarded blocks in case order: block n runs
// when pc is n, then sets pc to the next case (the fall-through); a jump
// ("pc = m; continue;") restarts the loop, whose blocks lead to case m; a
// break ends the loop as before. Only the loop the translator writes is
// understood.
bool rewrite_dispatch_loop(std::string& text, std::string* why) {
    const std::string head = "\tuint pc = 0;\n\twhile (true)\n";
    const std::string tail = "\n\t\t}\n\t\tbreak;\n\t}\n";
    const size_t at = text.find(head);
    if (at == std::string::npos) return true;
    const size_t end = text.find(tail, at);
    if (end == std::string::npos) {
        if (why) *why = "dispatch loop without its end";
        return false;
    }
    const std::string region = text.substr(at, end - at);
    const std::string opening = "\t{\n\t\tswitch (pc)\n\t\t{\n";
    const size_t body_at = region.find(opening);
    if (body_at == std::string::npos) {
        if (why) *why = "dispatch loop without its switch";
        return false;
    }
    struct Case { std::string number; std::vector<std::string_view> lines; };
    std::vector<Case> cases;
    std::string_view body = std::string_view(region).substr(body_at + opening.size());
    while (!body.empty()) {
        const size_t newline = body.find('\n');
        const std::string_view line = body.substr(0, newline);
        body = newline == std::string_view::npos ? std::string_view{} : body.substr(newline + 1);
        constexpr std::string_view label = "\t\tcase ";
        if (line.starts_with(label) && line.ends_with(':')) {
            const std::string_view number = line.substr(label.size(), line.size() - label.size() - 1);
            if (number.empty() || number.find_first_not_of("0123456789") != std::string_view::npos) {
                if (why) *why = "dispatch loop case is not a number";
                return false;
            }
            cases.push_back({std::string(number), {}});
        } else if (!cases.empty()) {
            cases.back().lines.push_back(line);
        } else if (line.find_first_not_of(" \t") != std::string_view::npos) {
            if (why) *why = "dispatch loop code before its first case";
            return false;
        }
    }
    if (cases.empty()) {
        if (why) *why = "dispatch loop without cases";
        return false;
    }
    std::string out = "\tuint pc = 0;\n\t[loop] while (true)\n\t{\n";
    for (size_t i = 0; i < cases.size(); ++i) {
        out += "\t\tif (pc == " + cases[i].number + "u)\n\t\t{\n";
        for (const auto line : cases[i].lines) {
            out += line;
            out += '\n';
        }
        if (i + 1 < cases.size()) out += "\t\t\tpc = " + cases[i + 1].number + "u;\n";
        out += "\t\t}\n";
    }
    out += "\t\tbreak;\n\t}\n";
    text.replace(at, end + tail.size() - at, out);
    return true;
}
}

std::optional<std::string> d3d11_shader_source(std::string text, std::string* why) {
    const auto refuse = [&](const char* reason) -> std::optional<std::string> {
        if (why) *why = reason;
        return std::nullopt;
    };
    // The specialization constant: a macro given at compile time.
    replace_all(text, "uint g_SpecConstants();", "");
    replace_all(text, "#define g_SpecConstants() g_SpecConstants", "");
    replace_all(text, "g_SpecConstants()", "(SFR_SPEC_CONSTANTS)");
    // A keyword of FXC's (effects).
    rename_word(text, "texture", "sfrTexture");
    // FXC's preprocessor takes no macro with an empty parameter list.
    if (!replace_all(text, "#define DEFINE_SHARED_CONSTANTS()", "#define DEFINE_SHARED_CONSTANTS"))
        return refuse("no shared constant macro");
    replace_all(text, "DEFINE_SHARED_CONSTANTS();", "DEFINE_SHARED_CONSTANTS");
    // The per-draw layout's fixed arrays, without register spaces.
    const std::pair<const char*, const char*> heaps[] = {
        {"Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);",
         "Texture2D<float4> g_Texture2DDescriptorHeap[16] : register(t0);"},
        {"Texture2DArray<float4> g_Texture2DArrayDescriptorHeap[] : register(t0, space1);",
         "Texture2DArray<float4> g_Texture2DArrayDescriptorHeap[16] : register(t16);"},
        {"TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);",
         "TextureCube<float4> g_TextureCubeDescriptorHeap[16] : register(t32);"},
        {"SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);",
         "SamplerState g_SamplerDescriptorHeap[16] : register(s0);"},
    };
    for (const auto& [from, to] : heaps)
        if (!replace_all(text, from, to)) return refuse("no descriptor heap declaration");
    replace_all(text, "RWStructuredBuffer<uint> g_ConditionalSurveyBuffer : register(u0, space4);", "");
    for (char b = '0'; b <= '9'; ++b)
        replace_all(text, std::string("register(b") + b + ", space4)", std::string("register(b") + b + ")");
    // [[vk::location(n)]] and the like.
    for (size_t at = 0; (at = text.find("[[vk::", at)) != std::string::npos;) {
        size_t end = text.find("]]", at);
        if (end == std::string::npos) return refuse("unterminated vk attribute");
        end += 2;
        while (end < text.size() && (text[end] == ' ' || text[end] == '\t')) ++end;
        text.erase(at, end - at);
    }
    replace_all(text, "in uint iFace : SV_IsFrontFace", "in bool iFace : SV_IsFrontFace");
    replace_all(text, "(swappedFloats & (1ull << semanticIndex)) != 0",
                "((semanticIndex & 63u) < 32u && (swappedFloats & (1u << (semanticIndex & 31u))) != 0)");
    if (!rewrite_dispatch_loop(text, why)) return std::nullopt;
    // FXC does not expand a function-like macro named as the array it reads.
    {
        std::set<std::string> names;
        constexpr std::string_view define = "#define ";
        for (size_t at = 0; (at = text.find(define, at)) != std::string::npos; at += define.size()) {
            const size_t name_end = text.find("(INDEX) ", at + define.size());
            if (name_end == std::string::npos) break;
            const std::string name = text.substr(at + define.size(), name_end - at - define.size());
            if (!name.empty() && std::all_of(name.begin(), name.end(), identifier_char)) names.insert(name);
        }
        for (const auto& name : names) {
            for (const char* type : {"float4 ", "int4 ", "bool "})
                replace_all(text, std::string(type) + name + "[", std::string(type) + name + "_sfrArray[");
            const std::string line_start = "#define " + name + "(INDEX) ";
            for (size_t at = 0; (at = text.find(line_start, at)) != std::string::npos;) {
                const size_t line_end = text.find('\n', at);
                std::string line = text.substr(at, line_end - at);
                replace_all(line, name + "[", name + "_sfrArray[");
                text.replace(at, line_end - at, line);
                at += line.size();
            }
        }
    }
    // The resolved-framebuffer flag from the shared constants; the index
    // itself is the literal fetch constant number.
    replace_all(text, "g_Texture2DDescriptorHeap[descriptor & 0x7fffffffu]", "g_Texture2DDescriptorHeap[descriptor]");
    replace_all(text, "(descriptor & 0x80000000u)", "(sfrSharedIndices[descriptor >> 2][descriptor & 3] & 0x80000000u)");
    replace_all(text, "g_Texture2DDescriptorHeap[resourceDescriptorIndex & 0x7fffffffu]",
                "g_Texture2DDescriptorHeap[resourceDescriptorIndex]");
    // The D3D branch's shared constant block, moved ahead of the helpers
    // that read it, its index members replaced by literals.
    size_t start = text.find("#define getPixelCoord");
    if (start != std::string::npos) start = text.find("#ifdef __spirv__", start);
    if (start != std::string::npos) start = text.find("#elif defined(__air__)", start);
    if (start != std::string::npos) start = text.find("#else\n", start);
    if (start == std::string::npos) return refuse("no D3D branch of the constant blocks");
    const std::string block_head = "cbuffer SharedConstants : register(b2)";
    const size_t block_at = text.find(block_head, start);
    if (block_at == std::string::npos) return refuse("no shared constant block");
    const size_t open = text.find('{', block_at);
    const size_t close = text.find("};", open);
    if (open == std::string::npos || close == std::string::npos) return refuse("malformed shared constant block");
    std::string body = text.substr(open + 1, close - open - 1);
    std::string defines;
    {
        static const std::regex member(
            R"(\tuint (\w+?)_(Texture2DArray|Texture2D|TextureCube|Sampler)DescriptorIndex : packoffset\(c(\d+)\.([xyzw])\);\n)");
        std::string rest;
        auto last = body.cbegin();
        for (std::sregex_iterator it(body.cbegin(), body.cend(), member), done; it != done; ++it) {
            const auto& m = *it;
            rest.append(last, m[0].first);
            last = m[0].second;
            const std::string kind = m[2];
            const int c = std::stoi(m[3]);
            const int swizzle = int(std::string_view("xyzw").find(m[4].str()[0]));
            const int base = kind == "Sampler" ? 12 : kind == "Texture2D" ? 0 : kind == "Texture2DArray" ? 4 : 8;
            const int number = (c - base) * 4 + swizzle;
            if (number < 0 || number >= 16) return refuse("fetch constant outside the shared constants");
            defines += "#define " + m[1].str() + "_" + kind + "DescriptorIndex " + std::to_string(number) + "u\n";
        }
        rest.append(last, body.cend());
        body = std::move(rest);
    }
    text.erase(block_at, close + 2 - block_at);
    const std::string block = "cbuffer SharedConstants : register(b2)\n{\n\tuint4 sfrSharedIndices[16] : packoffset(c0);\n" +
                              body + "};\n" + defines;
    const size_t anchor = text.find("Texture2D<float4> g_Texture2DDescriptorHeap[16] : register(t0);");
    if (anchor == std::string::npos || anchor > block_at) return refuse("descriptor heaps after the shared constants");
    text.insert(anchor, block);
    return "#define select(c, a, b) ((c) ? (a) : (b))\n" + text;
}
}
