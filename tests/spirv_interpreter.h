#pragma once

// A small SPIR-V interpreter for tests: runs a module's single function with
// every input, uniform and push constant filled from a seed, and returns the
// words of its outputs. It knows the instructions the game's vertex shaders
// use (and fails on any other), so two versions of a shader can be compared
// bit for bit.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace sfr::test {

class SpirvInterpreter {
public:
    explicit SpirvInterpreter(std::span<const uint8_t> code) {
        if (code.size() < 20 || code.size() % 4) throw std::runtime_error("not a SPIR-V module");
        words_.resize(code.size() / 4);
        std::memcpy(words_.data(), code.data(), code.size());
        for (size_t i = 5; i < words_.size();) {
            const uint32_t count = words_[i] >> 16;
            if (!count || i + count > words_.size()) throw std::runtime_error("truncated instruction");
            instructions_.push_back({i, count});
            i += count;
        }
    }

    // The outputs' words, in the order of their ids; nothing when the run took
    // more than step_limit instructions.
    std::optional<std::vector<uint32_t>> run(uint32_t seed, size_t step_limit = 2000000) {
        state_ = seed * 2654435761u + 1;
        types_.clear(), values_.clear(), memory_.clear(), outputs_.clear(), labels_.clear(), result_types_.clear();
        size_t function_start = 0;
        for (size_t n = 0; n < instructions_.size(); ++n) {
            const uint32_t* w = &words_[instructions_[n].at];
            const uint32_t opcode = w[0] & 0xffff, count = instructions_[n].count;
            if (opcode == 54) {
                if (function_start) throw std::runtime_error("more than one function");
                function_start = n;
                continue;
            }
            if (function_start) {
                if (opcode == 248) labels_[w[1]] = n;
                continue;
            }
            global(opcode, w, count);
        }
        if (!function_start) throw std::runtime_error("no function");
        // Run from the first label.
        size_t at = function_start + 1;
        while (at < instructions_.size() && (words_[instructions_[at].at] & 0xffff) != 248) ++at;
        uint32_t previous = 0, current = words_[instructions_[at].at + 1];
        ++at;
        size_t steps = 0;
        for (;;) {
            if (++steps > step_limit) return std::nullopt;
            const uint32_t* w = &words_[instructions_[at].at];
            const uint32_t opcode = w[0] & 0xffff, count = instructions_[at].count;
            uint32_t next = 0;
            switch (opcode) {
            case 245: {  // OpPhi: all of a block's phis read the values before any is written
                std::vector<std::pair<uint32_t, std::vector<uint32_t>>> phis;
                while ((words_[instructions_[at].at] & 0xffff) == 245) {
                    const uint32_t* p = &words_[instructions_[at].at];
                    const uint32_t c = instructions_[at].count;
                    bool found = false;
                    for (uint32_t k = 3; k + 1 < c && !found; k += 2)
                        if (p[k + 1] == previous) phis.push_back({p[2], value(p[k])}), found = true;
                    if (!found) throw std::runtime_error("a phi without the edge taken");
                    result_types_[p[2]] = p[1];
                    ++at;
                }
                for (auto& [id, v] : phis) values_[id] = std::move(v);
                continue;
            }
            case 246: case 247: break;  // merges
            case 249: next = w[1]; break;
            case 250: next = value(w[1])[0] ? w[2] : w[3]; break;
            case 251: {
                const uint32_t selector = value(w[1])[0];
                next = w[2];
                for (uint32_t k = 3; k + 1 < count; k += 2)
                    if (w[k] == selector) next = w[k + 1];
                break;
            }
            case 253: {
                std::vector<uint32_t> out;
                for (uint32_t id : outputs_) out.insert(out.end(), memory_[id].begin(), memory_[id].end());
                return out;
            }
            default: execute(opcode, w, count);
            }
            if (next) {
                previous = current, current = next;
                if (!labels_.contains(next)) throw std::runtime_error("a branch to an unknown label");
                at = labels_[next] + 1;
            } else {
                ++at;
            }
        }
    }

private:
    struct Instruction { size_t at; uint32_t count; };
    struct Type {
        uint32_t opcode = 0, words = 0;   // words: the size of a value
        std::vector<uint32_t> members;    // struct members, or the element (vector, matrix, array)
        uint32_t length = 0;              // vector/matrix/array length
        uint32_t width = 32, sign = 0;    // scalars
        uint32_t storage = 0, pointee = 0;  // pointers
    };
    // A pointer: a variable and the first word in it.
    static uint32_t pointer_variable(const std::vector<uint32_t>& v) { return v[0]; }

    uint32_t random() {
        state_ ^= state_ << 13, state_ ^= state_ >> 17, state_ ^= state_ << 5;
        return state_;
    }
    // Mostly small floats, some small integers and bit patterns.
    uint32_t random_word() {
        const uint32_t r = random();
        switch (r % 4) {
        case 0: return r >> 29;  // 0..7
        case 1: return r;
        default: {
            const float f = float(int32_t(r >> 8) % 4096) / 1024.0f;
            uint32_t bits;
            std::memcpy(&bits, &f, 4);
            return bits;
        }
        }
    }
    const Type& type(uint32_t id) {
        auto it = types_.find(id);
        if (it == types_.end()) throw std::runtime_error("unknown type " + std::to_string(id));
        return it->second;
    }
    std::vector<uint32_t> value(uint32_t id) {
        auto it = values_.find(id);
        if (it == values_.end()) throw std::runtime_error("a value read before it is made: " + std::to_string(id));
        return it->second;
    }

    void global(uint32_t opcode, const uint32_t* w, uint32_t count) {
        switch (opcode) {
        case 20: types_[w[1]] = {20, 1}; break;
        case 21: types_[w[1]] = {21, 1, {}, 0, w[2], w[3]}; break;
        case 22: types_[w[1]] = {22, 1, {}, 0, w[2]}; break;
        case 23: case 24: {
            Type t{opcode, type(w[2]).words * w[3], {w[2]}, w[3]};
            types_[w[1]] = t;
            break;
        }
        case 28: {
            const uint32_t length = value(w[3])[0];
            types_[w[1]] = {28, type(w[2]).words * length, {w[2]}, length};
            break;
        }
        case 29: types_[w[1]] = {29, type(w[2]).words * 4096, {w[2]}, 4096}; break;
        case 30: {
            Type t{30, 0};
            for (uint32_t k = 2; k < count; ++k) t.members.push_back(w[k]), t.words += type(w[k]).words;
            types_[w[1]] = t;
            break;
        }
        case 32: types_[w[1]] = {32, 1, {}, 0, 32, 0, w[2], w[3]}; break;
        case 19: case 33: types_[w[1]] = {opcode, 0}; break;
        case 41: case 48: values_[w[2]] = {1}; break;
        case 42: case 49: values_[w[2]] = {0}; break;
        case 43: case 50: values_[w[2]] = {w[3]}; break;
        case 44: case 51: {
            std::vector<uint32_t> v;
            for (uint32_t k = 3; k < count; ++k) {
                auto part = value(w[k]);
                v.insert(v.end(), part.begin(), part.end());
            }
            values_[w[2]] = v;
            break;
        }
        case 1: case 46: values_[w[2]] = std::vector<uint32_t>(type(w[1]).words, 0); break;
        case 52: throw std::runtime_error("a spec constant operation");
        case 59: {
            const Type& p = type(w[1]);
            std::vector<uint32_t> words(type(p.pointee).words, 0);
            // Input, Uniform, PushConstant, StorageBuffer, UniformConstant: random.
            if (p.storage == 1 || p.storage == 2 || p.storage == 9 || p.storage == 12 || p.storage == 0)
                for (auto& word : words) word = random_word();
            if (count >= 5) words = value(w[4]);
            if (p.storage == 3) outputs_.push_back(w[2]);
            memory_[w[2]] = std::move(words);
            values_[w[2]] = {w[2], 0, p.pointee};
            break;
        }
        case 25: case 26: case 27: throw std::runtime_error("an image type");
        default: break;  // capabilities, names, decorations, ...
        }
    }

    // The words of a component of a composite type at an index.
    std::pair<uint32_t, uint32_t> member(uint32_t type_id, uint32_t index) {  // (offset, type)
        const Type& t = type(type_id);
        if (t.opcode == 30) {
            uint32_t offset = 0;
            if (index >= t.members.size()) throw std::runtime_error("a struct index out of range");
            for (uint32_t k = 0; k < index; ++k) offset += type(t.members[k]).words;
            return {offset, t.members[index]};
        }
        if (t.opcode == 23 || t.opcode == 24 || t.opcode == 28 || t.opcode == 29) {
            const uint32_t element = t.members[0];
            return {(t.length ? index % t.length : 0) * type(element).words, element};
        }
        throw std::runtime_error("an index into a scalar");
    }

    static float f(uint32_t bits) { float v; std::memcpy(&v, &bits, 4); return v; }
    static uint32_t u(float v) { uint32_t bits; std::memcpy(&bits, &v, 4); return bits; }

    void execute(uint32_t opcode, const uint32_t* w, uint32_t count) {
        auto& out = values_[opcode == 62 ? 0 : (count > 2 ? w[2] : 0)];
        const auto unary = [&](auto fn) {
            auto a = value(w[3]);
            for (auto& x : a) x = fn(x);
            out = a;
        };
        const auto binary = [&](auto fn) {
            auto a = value(w[3]), b = value(w[4]);
            std::vector<uint32_t> r((std::max)(a.size(), b.size()));
            for (size_t i = 0; i < r.size(); ++i) r[i] = fn(a[i % a.size()], b[i % b.size()]);
            out = r;
        };
        const auto fbin = [&](auto fn) { binary([&](uint32_t a, uint32_t b) { return u(fn(f(a), f(b))); }); };
        const auto fcmp = [&](auto fn) { binary([&](uint32_t a, uint32_t b) { return uint32_t(fn(f(a), f(b))); }); };
        const auto shift = [](uint32_t b) { return b & 31; };
        switch (opcode) {
        case 59: {  // a Function variable
            const Type& p = type(w[1]);
            memory_[w[2]] = count >= 5 ? value(w[4]) : std::vector<uint32_t>(type(p.pointee).words, 0);
            out = {w[2], 0, p.pointee};
            break;
        }
        case 61: {
            const auto p = value(w[3]);
            const auto& m = memory_[p[0]];
            const uint32_t n = type(w[1]).words;
            if (p[1] + n > m.size()) throw std::runtime_error("a load out of its variable");
            out.assign(m.begin() + p[1], m.begin() + p[1] + n);
            break;
        }
        case 62: {
            const auto p = value(w[1]);
            const auto v = value(w[2]);
            auto& m = memory_[p[0]];
            if (p[1] + v.size() > m.size()) throw std::runtime_error("a store out of its variable");
            std::copy(v.begin(), v.end(), m.begin() + p[1]);
            values_.erase(0);
            break;
        }
        case 65: case 66: {
            auto p = value(w[3]);
            uint32_t offset = p[1], pointee = p[2];
            for (uint32_t k = 4; k < count; ++k) {
                const auto [at, next] = member(pointee, value(w[k])[0]);
                offset += at, pointee = next;
            }
            out = {p[0], offset, pointee};
            break;
        }
        case 79: {
            auto a = value(w[3]), b = value(w[4]);
            a.insert(a.end(), b.begin(), b.end());
            std::vector<uint32_t> r;
            for (uint32_t k = 5; k < count; ++k) r.push_back(w[k] < a.size() ? a[w[k]] : 0);
            out = r;
            break;
        }
        case 80: {
            std::vector<uint32_t> r;
            for (uint32_t k = 3; k < count; ++k) {
                auto part = value(w[k]);
                r.insert(r.end(), part.begin(), part.end());
            }
            out = r;
            break;
        }
        case 81: case 82: {
            const bool insert = opcode == 82;
            auto composite = value(w[insert ? 4 : 3]);
            // Values carry no type: walk the declared type of the composite.
            uint32_t offset = 0, t = type_of_value(w[insert ? 4 : 3]);
            for (uint32_t k = insert ? 5 : 4; k < count; ++k) {
                const auto [at, next] = member(t, w[k]);
                offset += at, t = next;
            }
            if (insert) {
                const auto object = value(w[3]);
                std::copy(object.begin(), object.end(), composite.begin() + offset);
                out = composite;
            } else {
                const uint32_t n = type(w[1]).words;
                out.assign(composite.begin() + offset, composite.begin() + offset + n);
            }
            break;
        }
        case 83: case 124: out = value(w[3]); break;
        case 109: unary([](uint32_t a) { const float x = f(a); return x != x ? 0u : x <= 0 ? 0u : x >= 4294967040.0f ? 0xffffffffu : uint32_t(x); }); break;
        case 110: unary([](uint32_t a) { const float x = f(a); return uint32_t(x != x ? 0 : x <= -2147483648.0f ? INT32_MIN : x >= 2147483520.0f ? INT32_MAX : int32_t(x)); }); break;
        case 111: unary([](uint32_t a) { return u(float(int32_t(a))); }); break;
        case 112: unary([](uint32_t a) { return u(float(a)); }); break;
        case 126: unary([](uint32_t a) { return 0u - a; }); break;
        case 127: unary([](uint32_t a) { return u(-f(a)); }); break;
        case 128: binary([](uint32_t a, uint32_t b) { return a + b; }); break;
        case 129: fbin([](float a, float b) { return a + b; }); break;
        case 130: binary([](uint32_t a, uint32_t b) { return a - b; }); break;
        case 131: fbin([](float a, float b) { return a - b; }); break;
        case 132: binary([](uint32_t a, uint32_t b) { return a * b; }); break;
        case 133: fbin([](float a, float b) { return a * b; }); break;
        case 134: binary([](uint32_t a, uint32_t b) { return b ? a / b : 0u; }); break;
        case 135: binary([](uint32_t a, uint32_t b) { return b && !(a == 0x80000000u && b == 0xffffffffu) ? uint32_t(int32_t(a) / int32_t(b)) : 0u; }); break;
        case 136: fbin([](float a, float b) { return a / b; }); break;
        case 141: fbin([](float a, float b) { return a - b * std::floor(a / b); }); break;
        case 142: {
            auto a = value(w[3]);
            const float s = f(value(w[4])[0]);
            for (auto& x : a) x = u(f(x) * s);
            out = a;
            break;
        }
        case 148: {
            const auto a = value(w[3]), b = value(w[4]);
            float sum = 0;
            for (size_t i = 0; i < a.size(); ++i) sum += f(a[i]) * f(b[i]);
            out = {u(sum)};
            break;
        }
        case 154: case 155: {
            const auto a = value(w[3]);
            bool r = opcode == 155;
            for (uint32_t x : a) r = opcode == 154 ? (r || x) : (r && x);
            out = {uint32_t(r)};
            break;
        }
        case 156: unary([](uint32_t a) { return uint32_t(std::isnan(f(a))); }); break;
        case 157: unary([](uint32_t a) { return uint32_t(std::isinf(f(a))); }); break;
        case 164: binary([](uint32_t a, uint32_t b) { return uint32_t(a == b); }); break;
        case 165: binary([](uint32_t a, uint32_t b) { return uint32_t(a != b); }); break;
        case 166: binary([](uint32_t a, uint32_t b) { return uint32_t(a || b); }); break;
        case 167: binary([](uint32_t a, uint32_t b) { return uint32_t(a && b); }); break;
        case 168: unary([](uint32_t a) { return uint32_t(!a); }); break;
        case 169: {
            const auto c = value(w[3]), a = value(w[4]), b = value(w[5]);
            std::vector<uint32_t> r(a.size());
            for (size_t i = 0; i < r.size(); ++i) r[i] = c[i % c.size()] ? a[i] : b[i];
            out = r;
            break;
        }
        case 170: binary([](uint32_t a, uint32_t b) { return uint32_t(a == b); }); break;
        case 171: binary([](uint32_t a, uint32_t b) { return uint32_t(a != b); }); break;
        case 172: binary([](uint32_t a, uint32_t b) { return uint32_t(a > b); }); break;
        case 173: binary([](uint32_t a, uint32_t b) { return uint32_t(int32_t(a) > int32_t(b)); }); break;
        case 174: binary([](uint32_t a, uint32_t b) { return uint32_t(a >= b); }); break;
        case 175: binary([](uint32_t a, uint32_t b) { return uint32_t(int32_t(a) >= int32_t(b)); }); break;
        case 176: binary([](uint32_t a, uint32_t b) { return uint32_t(a < b); }); break;
        case 177: binary([](uint32_t a, uint32_t b) { return uint32_t(int32_t(a) < int32_t(b)); }); break;
        case 178: binary([](uint32_t a, uint32_t b) { return uint32_t(a <= b); }); break;
        case 179: binary([](uint32_t a, uint32_t b) { return uint32_t(int32_t(a) <= int32_t(b)); }); break;
        case 180: fcmp([](float a, float b) { return a == b; }); break;
        case 182: fcmp([](float a, float b) { return a == a && b == b && a != b; }); break;
        case 183: fcmp([](float a, float b) { return !(a == b); }); break;
        case 184: fcmp([](float a, float b) { return a < b; }); break;
        case 186: fcmp([](float a, float b) { return a > b; }); break;
        case 188: fcmp([](float a, float b) { return a <= b; }); break;
        case 190: fcmp([](float a, float b) { return a >= b; }); break;
        case 194: binary([&](uint32_t a, uint32_t b) { return a >> shift(b); }); break;
        case 195: binary([&](uint32_t a, uint32_t b) { return uint32_t(int32_t(a) >> shift(b)); }); break;
        case 196: binary([&](uint32_t a, uint32_t b) { return a << shift(b); }); break;
        case 197: binary([](uint32_t a, uint32_t b) { return a | b; }); break;
        case 198: binary([](uint32_t a, uint32_t b) { return a ^ b; }); break;
        case 199: binary([](uint32_t a, uint32_t b) { return a & b; }); break;
        case 200: unary([](uint32_t a) { return ~a; }); break;
        case 12: ext(w, count, out); break;
        default: throw std::runtime_error("instruction " + std::to_string(opcode) + " is not interpreted");
        }
        if (opcode != 62 && count > 2) result_types_[w[2]] = w[1];
    }

    uint32_t type_of_value(uint32_t id) {
        auto it = result_types_.find(id);
        if (it != result_types_.end()) return it->second;
        // A constant: its type is in the declaration.
        for (const auto& in : instructions_) {
            const uint32_t* w = &words_[in.at];
            const uint32_t opcode = w[0] & 0xffff;
            if ((opcode == 44 || opcode == 46 || opcode == 1 || opcode == 51) && in.count >= 3 && w[2] == id)
                return result_types_[id] = w[1];
        }
        throw std::runtime_error("a composite of unknown type");
    }

    void ext(const uint32_t* w, uint32_t count, std::vector<uint32_t>& out) {
        const uint32_t instruction = w[4];
        std::vector<std::vector<uint32_t>> args;
        for (uint32_t k = 5; k < count; ++k) args.push_back(value(w[k]));
        std::vector<uint32_t> r(args.at(0).size());
        for (size_t i = 0; i < r.size(); ++i) {
            const auto a = [&](size_t n) { return args.at(n)[i % args[n].size()]; };
            const auto fa = [&](size_t n) { return f(a(n)); };
            switch (instruction) {
            case 1: r[i] = u(std::round(fa(0))); break;
            case 3: r[i] = u(std::trunc(fa(0))); break;
            case 4: r[i] = u(std::fabs(fa(0))); break;
            case 8: r[i] = u(std::floor(fa(0))); break;
            case 9: r[i] = u(std::ceil(fa(0))); break;
            case 10: r[i] = u(fa(0) - std::floor(fa(0))); break;
            case 13: r[i] = u(std::sin(fa(0))); break;
            case 14: r[i] = u(std::cos(fa(0))); break;
            case 26: r[i] = u(std::pow(fa(0), fa(1))); break;
            case 27: r[i] = u(std::exp(fa(0))); break;
            case 28: r[i] = u(std::log(fa(0))); break;
            case 29: r[i] = u(std::exp2(fa(0))); break;
            case 30: r[i] = u(std::log2(fa(0))); break;
            case 31: r[i] = u(std::sqrt(fa(0))); break;
            case 32: r[i] = u(1.0f / std::sqrt(fa(0))); break;
            case 37: case 79: r[i] = u(std::fmin(fa(0), fa(1))); break;
            case 38: r[i] = a(0) < a(1) ? a(0) : a(1); break;
            case 39: r[i] = int32_t(a(0)) < int32_t(a(1)) ? a(0) : a(1); break;
            case 40: case 80: r[i] = u(std::fmax(fa(0), fa(1))); break;
            case 41: r[i] = a(0) > a(1) ? a(0) : a(1); break;
            case 42: r[i] = int32_t(a(0)) > int32_t(a(1)) ? a(0) : a(1); break;
            case 43: case 81: r[i] = u(std::fmin(std::fmax(fa(0), fa(1)), fa(2))); break;
            case 46: r[i] = u(fa(0) * (1 - fa(2)) + fa(1) * fa(2)); break;
            case 50: r[i] = u(fa(0) * fa(1) + fa(2)); break;
            default: throw std::runtime_error("extended instruction " + std::to_string(instruction) + " is not interpreted");
            }
        }
        out = r;
    }

    std::vector<uint32_t> words_;
    std::vector<Instruction> instructions_;
    std::unordered_map<uint32_t, Type> types_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> values_;
    std::unordered_map<uint32_t, uint32_t> result_types_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> memory_;
    std::vector<uint32_t> outputs_;
    std::unordered_map<uint32_t, size_t> labels_;
    uint32_t state_ = 1;
};

}  // namespace sfr::test
