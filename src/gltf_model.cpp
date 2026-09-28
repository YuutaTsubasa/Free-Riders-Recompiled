#include "gltf_model.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <limits>

namespace sfr {
namespace {

// A JSON reader for what glTF uses and nothing more: objects, arrays,
// numbers, strings, true/false/null. Writing one is smaller than taking a
// dependency for it, and glTF never asks for more than this.
struct Json {
    enum class Kind { null, boolean, number, text, array, object };
    Kind kind = Kind::null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    const Json* find(const std::string& key) const {
        if (kind != Kind::object) return nullptr;
        const auto entry = object.find(key);
        return entry == object.end() ? nullptr : &entry->second;
    }
    // Numbers are read as doubles; glTF's indices and counts are integers
    // small enough to be exact in one.
    uint32_t whole(uint32_t fallback = 0) const {
        return kind == Kind::number && number >= 0 ? uint32_t(number) : fallback;
    }
    uint32_t whole_at(const std::string& key, uint32_t fallback = 0) const {
        const Json* const value = find(key);
        return value ? value->whole(fallback) : fallback;
    }
};

struct Parser {
    const char* at;
    const char* end;
    bool failed = false;

    void skip() {
        while (at < end && (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n')) ++at;
    }
    bool take(char c) {
        skip();
        if (at < end && *at == c) { ++at; return true; }
        return false;
    }
    bool word(const char* literal) {
        const size_t length = std::strlen(literal);
        if (size_t(end - at) < length || std::memcmp(at, literal, length) != 0) return false;
        at += length;
        return true;
    }
    std::string string() {
        std::string out;
        if (!take('"')) { failed = true; return out; }
        while (at < end && *at != '"') {
            if (*at == '\\' && at + 1 < end) {
                ++at;
                switch (*at) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    // The names glTF uses are plain; an escape is kept as the
                    // bytes of its code point when it is one, and skipped
                    // otherwise. Nothing here is matched against such a name.
                    if (end - at < 5) { failed = true; return out; }
                    unsigned code = 0;
                    for (int digit = 1; digit <= 4; ++digit) {
                        const char c = at[digit];
                        code <<= 4;
                        if (c >= '0' && c <= '9') code |= unsigned(c - '0');
                        else if (c >= 'a' && c <= 'f') code |= unsigned(c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') code |= unsigned(c - 'A' + 10);
                        else { failed = true; return out; }
                    }
                    at += 4;
                    if (code < 0x80) out += char(code);
                    else if (code < 0x800) {
                        out += char(0xC0 | (code >> 6));
                        out += char(0x80 | (code & 0x3F));
                    } else {
                        out += char(0xE0 | (code >> 12));
                        out += char(0x80 | ((code >> 6) & 0x3F));
                        out += char(0x80 | (code & 0x3F));
                    }
                    break;
                }
                default: out += *at; break;
                }
                ++at;
            } else {
                out += *at++;
            }
        }
        if (at >= end) { failed = true; return out; }
        ++at;
        return out;
    }
    Json value(size_t depth = 0) {
        // Unknown extension metadata is parsed too; bound both parsing and
        // destruction of the recursively owned JSON tree.
        if (depth >= 128) { failed = true; return {}; }
        skip();
        if (at >= end) { failed = true; return {}; }
        Json out;
        if (*at == '{') {
            ++at;
            out.kind = Json::Kind::object;
            skip();
            if (take('}')) return out;
            while (!failed) {
                skip();
                const std::string key = string();
                if (failed || !take(':')) { failed = true; break; }
                out.object[key] = value(depth + 1);
                if (take(',')) continue;
                if (take('}')) break;
                failed = true;
            }
            return out;
        }
        if (*at == '[') {
            ++at;
            out.kind = Json::Kind::array;
            skip();
            if (take(']')) return out;
            while (!failed) {
                out.array.push_back(value(depth + 1));
                if (take(',')) continue;
                if (take(']')) break;
                failed = true;
            }
            return out;
        }
        if (*at == '"') {
            out.kind = Json::Kind::text;
            out.text = string();
            return out;
        }
        if (word("true")) { out.kind = Json::Kind::boolean; out.boolean = true; return out; }
        if (word("false")) { out.kind = Json::Kind::boolean; out.boolean = false; return out; }
        if (word("null")) { out.kind = Json::Kind::null; return out; }
        // A number: strtod reads exactly what JSON allows and stops after it.
        char* stop = nullptr;
        const double parsed = std::strtod(at, &stop);
        if (stop == at) { failed = true; return out; }
        at = stop;
        out.kind = Json::Kind::number;
        out.number = parsed;
        return out;
    }
};

struct Matrix {  // column-major, as glTF stores one
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

Matrix multiply(const Matrix& left, const Matrix& right) {
    Matrix out;
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row) {
            float sum = 0;
            for (int inner = 0; inner < 4; ++inner) sum += left.m[inner * 4 + row] * right.m[column * 4 + inner];
            out.m[column * 4 + row] = sum;
        }
    return out;
}

Matrix from_node(const Json& node) {
    if (const Json* const matrix = node.find("matrix"); matrix && matrix->array.size() == 16) {
        Matrix out;
        for (int i = 0; i < 16; ++i) out.m[i] = float(matrix->array[size_t(i)].number);
        return out;
    }
    float translation[3] = {0, 0, 0}, rotation[4] = {0, 0, 0, 1}, scale[3] = {1, 1, 1};
    if (const Json* const value = node.find("translation"); value && value->array.size() == 3)
        for (int i = 0; i < 3; ++i) translation[i] = float(value->array[size_t(i)].number);
    if (const Json* const value = node.find("rotation"); value && value->array.size() == 4)
        for (int i = 0; i < 4; ++i) rotation[i] = float(value->array[size_t(i)].number);
    if (const Json* const value = node.find("scale"); value && value->array.size() == 3)
        for (int i = 0; i < 3; ++i) scale[i] = float(value->array[size_t(i)].number);
    const float x = rotation[0], y = rotation[1], z = rotation[2], w = rotation[3];
    Matrix out;
    out.m[0] = (1 - 2 * (y * y + z * z)) * scale[0];
    out.m[1] = (2 * (x * y + z * w)) * scale[0];
    out.m[2] = (2 * (x * z - y * w)) * scale[0];
    out.m[4] = (2 * (x * y - z * w)) * scale[1];
    out.m[5] = (1 - 2 * (x * x + z * z)) * scale[1];
    out.m[6] = (2 * (y * z + x * w)) * scale[1];
    out.m[8] = (2 * (x * z + y * w)) * scale[2];
    out.m[9] = (2 * (y * z - x * w)) * scale[2];
    out.m[10] = (1 - 2 * (x * x + y * y)) * scale[2];
    out.m[12] = translation[0];
    out.m[13] = translation[1];
    out.m[14] = translation[2];
    return out;
}

// The bytes an accessor addresses, and how to step through them.
struct Accessor {
    const uint8_t* data = nullptr;
    uint32_t count = 0, stride = 0, component = 0, components = 0;
    bool ok = false;
};

uint32_t component_size(uint32_t type) {
    switch (type) {
    case 5120: case 5121: return 1;  // byte, unsigned byte
    case 5122: case 5123: return 2;  // short, unsigned short
    case 5125: case 5126: return 4;  // unsigned int, float
    default: return 0;
    }
}

uint32_t component_count(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT4") return 16;
    return 0;
}

Accessor read_accessor(const Json& root, uint32_t index, const std::vector<uint8_t>& binary) {
    Accessor out;
    const Json* const accessors = root.find("accessors");
    if (!accessors || index >= accessors->array.size()) return out;
    const Json& accessor = accessors->array[index];
    out.component = accessor.whole_at("componentType");
    out.count = accessor.whole_at("count");
    const Json* const type = accessor.find("type");
    out.components = type ? component_count(type->text) : 0;
    const uint32_t element = component_size(out.component) * out.components;
    if (!element || !out.count) return out;
    const Json* const views = root.find("bufferViews");
    const Json* const view_index = accessor.find("bufferView");
    if (!views || !view_index) return out;  // a sparse or zero-filled accessor
    const uint32_t which = view_index->whole();
    if (which >= views->array.size()) return out;
    const Json& view = views->array[which];
    const uint64_t view_offset = view.whole_at("byteOffset");
    const uint64_t view_length = view.whole_at("byteLength");
    const uint32_t view_stride = view.whole_at("byteStride");
    out.stride = view_stride ? view_stride : element;
    const uint64_t offset = view_offset + accessor.whole_at("byteOffset");
    const uint64_t span = uint64_t(out.stride) * (out.count - 1) + element;
    if (offset + span > binary.size() || view_offset + view_length > binary.size()) return out;
    out.data = binary.data() + offset;
    out.ok = true;
    return out;
}

float read_float(const uint8_t* bytes, uint32_t component) {
    if (component == 5126) {
        float value = 0;
        std::memcpy(&value, bytes, sizeof value);
        return value;
    }
    return 0;
}

uint32_t read_index(const uint8_t* bytes, uint32_t component) {
    switch (component) {
    case 5121: return *bytes;
    case 5123: { uint16_t value = 0; std::memcpy(&value, bytes, sizeof value); return value; }
    case 5125: { uint32_t value = 0; std::memcpy(&value, bytes, sizeof value); return value; }
    default: return 0;
    }
}

float read_weight(const uint8_t* bytes, uint32_t component) {
    switch (component) {
    case 5126: return read_float(bytes, component);
    case 5121: return float(*bytes) / 255.0f;
    case 5123: { uint16_t value = 0; std::memcpy(&value, bytes, sizeof value); return float(value) / 65535.0f; }
    default: return 0;
    }
}

// A turn in the model's own axes, from degrees about x, then y, then z.
Matrix rotation_from_degrees(float x, float y, float z) {
    const float to_radians = 3.14159265358979f / 180.0f;
    const float cx = std::cos(x * to_radians), sx = std::sin(x * to_radians);
    const float cy = std::cos(y * to_radians), sy = std::sin(y * to_radians);
    const float cz = std::cos(z * to_radians), sz = std::sin(z * to_radians);
    Matrix rx, ry, rz;
    rx.m[5] = cx; rx.m[6] = sx; rx.m[9] = -sx; rx.m[10] = cx;
    ry.m[0] = cy; ry.m[2] = -sy; ry.m[8] = sy; ry.m[10] = cy;
    rz.m[0] = cz; rz.m[1] = sz; rz.m[4] = -sz; rz.m[5] = cz;
    return multiply(rz, multiply(ry, rx));
}

// The stance, bone by bone, in degrees about the model's own axes: x tips
// forward, y turns on the spot, z swings a limb out to the side. A VRM stands
// facing +z with +y up, so the model's left is at +x -- which is why the two
// sides carry opposite signs.
//
// This is a rider standing on a board: leaning into the wind, arms down and a
// little forward for balance, knees bent, feet apart across the board.
struct PoseBone { const char* bone; float x, y, z; };
constexpr PoseBone riding_stance[] = {
    {"spine", 8, 0, 0},           {"chest", 5, 0, 0},
    {"neck", -6, 0, 0},           {"head", -4, 0, 0},
    {"leftUpperArm", 0, -12, -58}, {"rightUpperArm", 0, 12, 58},
    {"leftLowerArm", 0, -26, -6},  {"rightLowerArm", 0, 26, 6},
    {"leftHand", 0, -8, 0},        {"rightHand", 0, 8, 0},
    {"leftUpperLeg", -18, 0, 7},   {"rightUpperLeg", -18, 0, -7},
    {"leftLowerLeg", 34, 0, 0},    {"rightLowerLeg", 34, 0, 0},
    {"leftFoot", -16, 0, 0},       {"rightFoot", -16, 0, 0},
};

// Which node each humanoid bone is, from whichever VRM extension the file
// carries. VRM 1.0 (VRMC_vrm) keeps them as an object of bone name to node;
// VRM 0.x (VRM) keeps a list of {bone, node}. Empty when the file names no
// humanoid at all, which is the answer for a model that is not a VRM.
std::map<std::string, uint32_t> humanoid_bones(const Json& root, bool& facing_backwards) {
    std::map<std::string, uint32_t> out;
    facing_backwards = false;
    const Json* const extensions = root.find("extensions");
    if (!extensions) return out;
    if (const Json* const vrm = extensions->find("VRMC_vrm")) {
        if (const Json* const humanoid = vrm->find("humanoid"))
            if (const Json* const bones = humanoid->find("humanBones"))
                for (const auto& [name, entry] : bones->object)
                    if (const Json* const node = entry.find("node")) out[name] = node->whole(~0u);
        return out;
    }
    if (const Json* const vrm = extensions->find("VRM")) {
        // A VRM 0.x model faces -z, the opposite of a 1.0 one.
        facing_backwards = true;
        if (const Json* const humanoid = vrm->find("humanoid"))
            if (const Json* const bones = humanoid->find("humanBones"))
                for (const Json& entry : bones->array)
                    if (const Json* const name = entry.find("bone"))
                        if (const Json* const node = entry.find("node")) out[name->text] = node->whole(~0u);
    }
    return out;
}

// A picture no larger than the limit, halved as many times as it takes. A
// model's textures are made for a renderer with room to spare; this one draws
// one model, and a 8192 square skin costs 256 MB on the way to the card.
DecodedImage shrink_to(DecodedImage image, uint32_t limit) {
    while (image.width > limit || image.height > limit) {
        const uint32_t width = (std::max)(1u, image.width / 2), height = (std::max)(1u, image.height / 2);
        std::vector<uint8_t> smaller(size_t(width) * height * 4);
        for (uint32_t row = 0; row < height; ++row)
            for (uint32_t column = 0; column < width; ++column)
                for (uint32_t channel = 0; channel < 4; ++channel) {
                    // The four pixels this one stands for, averaged.
                    uint32_t total = 0;
                    for (uint32_t down = 0; down < 2; ++down)
                        for (uint32_t across = 0; across < 2; ++across) {
                            const uint32_t y = (std::min)(row * 2 + down, image.height - 1);
                            const uint32_t x = (std::min)(column * 2 + across, image.width - 1);
                            total += image.rgba[(size_t(y) * image.width + x) * 4 + channel];
                        }
                    smaller[(size_t(row) * width + column) * 4 + channel] = uint8_t(total / 4);
                }
        image.width = width;
        image.height = height;
        image.rgba = std::move(smaller);
    }
    return image;
}

void fail(std::string* error, const char* why) {
    if (error) *error = why;
}

}  // namespace

struct GltfRig {
    struct Skin { std::vector<uint32_t> nodes; std::vector<Matrix> inverse_bind; };
    struct Influence { std::array<uint32_t, 4> joints{}; std::array<float, 4> weights{}; };
    struct Primitive {
        std::vector<float> positions, normals;
        std::vector<Influence> influences;
        uint32_t node = ~0u, skin = ~0u;
    };
    std::vector<Matrix> local, bind_world, basis;
    std::vector<uint32_t> parent, order;
    std::vector<int> native_bone;
    std::vector<Skin> skins;
    std::vector<Primitive> primitives;
    bool facing_backwards = false;
    float anchor_y = std::numeric_limits<float>::infinity();
};

namespace {
Matrix weighted_matrix(const GltfRig::Influence& influence, const std::vector<Matrix>& palette) {
    Matrix out;
    std::fill(std::begin(out.m), std::end(out.m), 0.0f);
    float total = 0;
    for (size_t i = 0; i < 4; ++i) {
        const float weight = influence.weights[i];
        if (!(weight > 0) || !std::isfinite(weight) || influence.joints[i] >= palette.size()) continue;
        const auto& joint = palette[influence.joints[i]];
        for (size_t j = 0; j < 16; ++j) out.m[j] += joint.m[j] * weight;
        total += weight;
    }
    if (total <= 0.0001f) return {};
    if (std::fabs(total - 1.0f) > 0.0001f)
        for (float& value : out.m) value /= total;
    return out;
}

Matrix rotation_basis(const Matrix& matrix) {
    Matrix out;
    // A matrix node can carry scale (and rounding/shear). Orthogonalize the
    // axes so transpose_rotation remains its inverse, including at identity.
    double axes[3][3]{};
    for (int column = 0; column < 3; ++column) {
        double original_length = 0;
        for (int row = 0; row < 3; ++row) {
            axes[column][row] = matrix.m[column * 4 + row];
            original_length += axes[column][row] * axes[column][row];
        }
        if (!(original_length > 0) || !std::isfinite(original_length)) return {};
        for (int previous = 0; previous < column; ++previous) {
            double projection = 0;
            for (int row = 0; row < 3; ++row) projection += axes[column][row] * axes[previous][row];
            for (int row = 0; row < 3; ++row) axes[column][row] -= projection * axes[previous][row];
        }
        double length = 0;
        for (double value : axes[column]) length += value * value;
        if (!(length > original_length * 1e-12)) return {};
        length = std::sqrt(length);
        for (int row = 0; row < 3; ++row) {
            axes[column][row] /= length;
            out.m[column * 4 + row] = float(axes[column][row]);
        }
    }
    return out;
}

Matrix transpose_rotation(const Matrix& matrix) {
    Matrix out;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) out.m[c * 4 + r] = matrix.m[r * 4 + c];
    return out;
}

bool quaternion_matrix(const std::array<float, 4>& quaternion, bool backwards, Matrix& out) {
    double norm = 0;
    for (float value : quaternion) {
        if (!std::isfinite(value)) return false;
        norm += double(value) * value;
    }
    if (!(norm > 1e-12)) return false;
    const float inverse_length = float(1 / std::sqrt(norm));
    const float flip = backwards ? -1.0f : 1.0f;
    const float x = quaternion[0] * inverse_length * flip, y = quaternion[1] * inverse_length;
    const float z = quaternion[2] * inverse_length * flip, w = quaternion[3] * inverse_length;
    out.m[0] = 1 - 2 * (y*y + z*z); out.m[1] = 2 * (x*y + z*w); out.m[2] = 2 * (x*z - y*w);
    out.m[4] = 2 * (x*y - z*w); out.m[5] = 1 - 2 * (x*x + z*z); out.m[6] = 2 * (y*z + x*w);
    out.m[8] = 2 * (x*z + y*w); out.m[9] = 2 * (y*z - x*w); out.m[10] = 1 - 2 * (x*x + y*y);
    return true;
}

bool evaluate_native_hierarchy(const GltfRig& rig, const AvatarPose& pose,
                               std::vector<Matrix>& world, std::vector<Matrix>* rotations = nullptr) {
    if (!pose.valid) return false;
    std::array<Matrix, 72> delta;
    // Mesh and attachment evaluation accept exactly the same capture.
    for (const float value : pose.bones[0].translation)
        if (!std::isfinite(value)) return false;
    for (size_t i = 0; i < delta.size(); ++i)
        if (!quaternion_matrix(pose.bones[i].rotation, rig.facing_backwards, delta[i])) return false;
    world.resize(rig.local.size());
    if (rotations) rotations->resize(rig.local.size());
    for (uint32_t node : rig.order) {
        Matrix local = rig.local[node];
        Matrix turn;
        if (const int native = rig.native_bone[node]; native >= 0) {
            // Native deltas use the normalized humanoid axes. Convert them to
            // this authored bone's bind basis, then inherit its posed parent.
            turn = multiply(transpose_rotation(rig.basis[node]), multiply(delta[size_t(native)], rig.basis[node]));
            local = multiply(local, turn);
        }
        world[node] = rig.parent[node] == ~0u ? local : multiply(world[rig.parent[node]], local);
        if (rotations) {
            // Derive axes from rotation-only locals, as for rig.basis. Taking
            // a rotation from world[node] would retain shear from a scaled
            // parent and could turn an identity pose into a rotated grip.
            const Matrix local_rotation = multiply(rotation_basis(rig.local[node]), turn);
            (*rotations)[node] = rig.parent[node] == ~0u ? local_rotation
                : rotation_basis(multiply((*rotations)[rig.parent[node]], local_rotation));
        }
    }
    return true;
}
}

std::optional<std::array<float, 16>> gltf_avatar_bone_transform(
    const GltfModel& model, const AvatarPose& pose, uint32_t native_bone, float scale) {
    if (!model.rig || native_bone >= pose.bones.size() || !(scale > 0) || !std::isfinite(scale) ||
        !std::isfinite(model.ground_y)) return std::nullopt;
    const GltfRig& rig = *model.rig;
    const auto found = std::find(rig.native_bone.begin(), rig.native_bone.end(), int(native_bone));
    if (found == rig.native_bone.end()) return std::nullopt;
    const size_t node = size_t(found - rig.native_bone.begin());
    std::vector<Matrix> world, rotations;
    if (!evaluate_native_hierarchy(rig, pose, world, &rotations)) return std::nullopt;

    // Remove the authored bind axes instead of exposing a glTF joint's local
    // axes or a skinning matrix with inverse bind. At identity every grip has
    // native model axes, even when the artist rotated/scaled its node.
    Matrix grip = multiply(rotations[node], transpose_rotation(rig.basis[node]));
    const float facing[3] = {rig.facing_backwards ? -1.f : 1.f, 1.f, rig.facing_backwards ? -1.f : 1.f};
    const float reflection[3] = {pose.mirrored ? -1.f : 1.f, 1.f, 1.f};
    std::array<float, 16> result{};
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row)
            // C * grip * C converts both normalized input axes and output
            // axes for VRM 0; mirror reflects only the completed output.
            result[column * 4 + row] = scale * reflection[row] * facing[row] *
                                      grip.m[column * 4 + row] * facing[column];
        const float position = reflection[column] * (facing[column] * world[node].m[12 + column] +
                                                      pose.bones[0].translation[column]);
        result[12 + column] = scale * (position - (column == 1 ? model.ground_y : 0.f));
    }
    // Column-major column-vector bytes equal row-major transposed row-vector
    // bytes. Translation therefore stays at 12..14 for the native getter.
    result[15] = 1;
    for (float value : result) if (!std::isfinite(value)) return std::nullopt;
    return result;
}

bool pose_gltf_model(GltfModel& model, const AvatarPose& pose) {
    if (!model.rig || model.rig->primitives.size() != model.primitives.size()) return false;
    const GltfRig& rig = *model.rig;
    std::vector<Matrix> world;
    if (!evaluate_native_hierarchy(rig, pose, world)) return false;
    for (size_t i = 0; i < model.primitives.size(); ++i)
        if (model.primitives[i].positions.size() != rig.primitives[i].positions.size() ||
            model.primitives[i].normals.size() != rig.primitives[i].normals.size()) return false;
    std::vector<std::vector<Matrix>> palettes(rig.skins.size());
    for (size_t s = 0; s < rig.skins.size(); ++s) {
        const auto& skin = rig.skins[s];
        auto& palette = palettes[s];
        palette.reserve(skin.nodes.size());
        for (size_t j = 0; j < skin.nodes.size(); ++j)
            palette.push_back(skin.nodes[j] < world.size() ? multiply(world[skin.nodes[j]], skin.inverse_bind[j]) : skin.inverse_bind[j]);
    }
    bool any = false;
    for (size_t p = 0; p < rig.primitives.size(); ++p) {
        const auto& source = rig.primitives[p];
        auto& out = model.primitives[p];
        for (size_t v = 0; v < source.positions.size() / 3; ++v) {
            const Matrix transform = !source.influences.empty() ? weighted_matrix(source.influences[v], palettes[source.skin])
                : (source.skin == ~0u ? world[source.node] : Matrix{});
            for (int axis = 0; axis < 3; ++axis) {
                float value = transform.m[12 + axis];
                for (int c = 0; c < 3; ++c) value += transform.m[c * 4 + axis] * source.positions[v * 3 + c];
                if (rig.facing_backwards && axis != 1) value = -value;
                // The capture is already in native model axes. Translate
                // after VRM 0 facing conversion, once for the entire model.
                value += pose.bones[0].translation[axis];
                if (pose.mirrored && axis == 0) value = -value;
                out.positions[v * 3 + axis] = value;
                if (!any) model.lowest[axis] = model.highest[axis] = value;
                else {
                    model.lowest[axis] = (std::min)(model.lowest[axis], value);
                    model.highest[axis] = (std::max)(model.highest[axis], value);
                }
            }
            any = true;
            if (!source.normals.empty()) {
                float normal[3]{};
                for (int axis = 0; axis < 3; ++axis) {
                    for (int c = 0; c < 3; ++c) normal[axis] += transform.m[c * 4 + axis] * source.normals[v * 3 + c];
                    if (rig.facing_backwards && axis != 1) normal[axis] = -normal[axis];
                    if (pose.mirrored && axis == 0) normal[axis] = -normal[axis];
                }
                const float length = std::sqrt(normal[0]*normal[0] + normal[1]*normal[1] + normal[2]*normal[2]);
                for (int axis = 0; axis < 3; ++axis) out.normals[v * 3 + axis] = length > 0 ? normal[axis] / length : 0;
            }
        }
    }
    return true;
}

bool model_wanted() {
    static const bool wanted = !avatar_model_path().empty();
    return wanted;
}

std::filesystem::path avatar_model_path() {
#ifdef _WIN32
    const wchar_t* path = _wgetenv(L"SFR_AVATAR_MODEL");
#else
    const char* path = std::getenv("SFR_AVATAR_MODEL");
#endif
    return path ? std::filesystem::path(path) : std::filesystem::path{};
}

std::optional<GltfModel> read_binary_gltf(const std::vector<uint8_t>& bytes, std::string* error, GltfPose pose) {
    if (bytes.size() < 20) { fail(error, "shorter than a binary glTF header"); return std::nullopt; }
    uint32_t magic = 0, version = 0, length = 0;
    std::memcpy(&magic, bytes.data(), 4);
    std::memcpy(&version, bytes.data() + 4, 4);
    std::memcpy(&length, bytes.data() + 8, 4);
    if (magic != 0x46546C67u) { fail(error, "not a binary glTF (no glTF magic)"); return std::nullopt; }
    if (version != 2) { fail(error, "only glTF version 2 is read"); return std::nullopt; }
    if (length > bytes.size()) { fail(error, "the header's length is past the end of the file"); return std::nullopt; }

    std::string json;
    std::vector<uint8_t> binary;
    uint64_t at = 12;
    while (at + 8 <= length) {
        uint32_t chunk_length = 0, chunk_type = 0;
        std::memcpy(&chunk_length, bytes.data() + at, 4);
        std::memcpy(&chunk_type, bytes.data() + at + 4, 4);
        at += 8;
        if (at + chunk_length > length) { fail(error, "a chunk reaches past the end of the file"); return std::nullopt; }
        if (chunk_type == 0x4E4F534Au) json.assign(reinterpret_cast<const char*>(bytes.data() + at), chunk_length);
        else if (chunk_type == 0x004E4942u) binary.assign(bytes.data() + at, bytes.data() + at + chunk_length);
        at += chunk_length;
        at = (at + 3) & ~uint64_t(3);
    }
    if (json.empty()) { fail(error, "no JSON chunk"); return std::nullopt; }

    Parser parser{json.data(), json.data() + json.size()};
    const Json root = parser.value();
    if (parser.failed || root.kind != Json::Kind::object) { fail(error, "the JSON chunk does not parse"); return std::nullopt; }

    const Json* const nodes = root.find("nodes");
    const Json* const meshes = root.find("meshes");
    if (!meshes) { fail(error, "no meshes"); return std::nullopt; }

    // Each node's transform, walked from the scene's roots so that a mesh
    // stands where its parents put it. A node with no parent is its own root,
    // which covers files that leave the scene list out.
    const size_t node_count = nodes ? nodes->array.size() : 0;
    std::vector<Matrix> local(node_count), world(node_count);
    std::vector<bool> placed(node_count, false);
    std::vector<bool> visiting(node_count, false);
    std::vector<uint32_t> parent(node_count, ~0u);
    for (size_t index = 0; index < node_count; ++index) {
        local[index] = from_node(nodes->array[index]);
        if (const Json* const children = nodes->array[index].find("children"))
            for (const Json& child : children->array)
                if (child.whole() < node_count) parent[child.whole()] = uint32_t(index);
    }

    // Parents before children, so a world transform is only ever built on one
    // that is already built.
    std::vector<uint32_t> order;
    order.reserve(node_count);
    for (size_t index = 0; index < node_count; ++index) {
        std::vector<uint32_t> chain;
        uint32_t walk = uint32_t(index);
        while (walk != ~0u && !placed[walk]) {
            if (visiting[walk]) { fail(error, "a cycle in the node hierarchy"); return std::nullopt; }
            visiting[walk] = true;
            chain.push_back(walk);
            walk = parent[walk];
        }
        for (auto item = chain.rbegin(); item != chain.rend(); ++item) {
            placed[*item] = true;
            order.push_back(*item);
        }
    }

    // The stance, as a turn for each bone the VRM extension names. A model
    // with no humanoid, or one asked for in its rest pose, gets none and comes
    // out exactly as it was authored.
    bool facing_backwards = false;
    const std::map<std::string, uint32_t> bones = humanoid_bones(root, facing_backwards);
    auto rig = std::make_shared<GltfRig>();
    rig->local = local;
    rig->parent = parent;
    rig->order = order;
    rig->facing_backwards = facing_backwards;
    rig->bind_world.resize(node_count);
    rig->basis.resize(node_count);
    rig->native_bone.assign(node_count, -1);
    // The game's generic humanoid mapping at 0x82190438, consumed by
    // sub_82291A48. Other native bones stay unmapped until their axes and
    // hierarchy are verified; do not guess intermediate/finger joints.
    const std::pair<const char*, int> native_bones[] = {
        {"hips", 0}, {"spine", 1}, {"chest", 5}, {"neck", 14}, {"head", 19},
        {"leftShoulder", 12}, {"rightShoulder", 16}, {"leftUpperArm", 20}, {"rightUpperArm", 22},
        {"leftLowerArm", 25}, {"rightLowerArm", 28}, {"leftHand", 33}, {"rightHand", 36},
        {"leftUpperLeg", 2}, {"rightUpperLeg", 3}, {"leftLowerLeg", 6}, {"rightLowerLeg", 8},
        {"leftFoot", 11}, {"rightFoot", 15}
    };
    for (const auto& [name, native] : native_bones)
        if (const auto found = bones.find(name); found != bones.end() && found->second < node_count)
            rig->native_bone[found->second] = native;
    for (uint32_t node : order) {
        rig->bind_world[node] = parent[node] == ~0u ? local[node] : multiply(rig->bind_world[parent[node]], local[node]);
        // Compose rotation-only locals: a parent's nonuniform scale followed
        // by a child's rotation shears bind_world and cannot define its axes.
        const Matrix local_basis = rotation_basis(local[node]);
        rig->basis[node] = parent[node] == ~0u ? local_basis
            : rotation_basis(multiply(rig->basis[parent[node]], local_basis));
    }
    std::map<uint32_t, Matrix> turns;
    if (pose == GltfPose::riding) {
        for (const PoseBone& posed : riding_stance) {
            const auto found = bones.find(posed.bone);
            if (found == bones.end() || found->second >= node_count) continue;
            // A model that faces the other way needs the same stance written
            // the other way round: turning the finished model half a turn
            // about y flips the x and z parts of every rotation in it.
            turns[found->second] = facing_backwards ? rotation_from_degrees(-posed.x, posed.y, -posed.z)
                                                    : rotation_from_degrees(posed.x, posed.y, posed.z);
        }
    }

    for (const uint32_t index : order) {
        const uint32_t above = parent[index];
        world[index] = above != ~0u && above != index ? multiply(world[above], local[index]) : local[index];
        const auto turn = turns.find(index);
        if (turn == turns.end()) continue;
        // Turn the bone about its own place: the joint stays where it is, its
        // direction changes, and everything below it follows -- which is what
        // posing a skeleton means. Only the rotation part is touched, so the
        // translation is simply kept.
        Matrix& here = world[index];
        Matrix turned;
        for (int column = 0; column < 3; ++column)
            for (int row = 0; row < 3; ++row) {
                float sum = 0;
                for (int inner = 0; inner < 3; ++inner) sum += turn->second.m[inner * 4 + row] * here.m[column * 4 + inner];
                turned.m[column * 4 + row] = sum;
            }
        for (int i = 12; i < 15; ++i) turned.m[i] = here.m[i];
        here = turned;
    }

    // Each skin's joint matrices: where a bone is now against where it was
    // when the mesh was made. In the rest pose every one of these is the
    // identity, which is why an unposed model needs no skinning at all.
    std::vector<std::vector<Matrix>> skins;
    std::vector<std::vector<Matrix>> bind_palettes;
    if (const Json* const skin_list = root.find("skins"))
        for (const Json& skin : skin_list->array) {
            std::vector<Matrix> joints;
            const Json* const joint_list = skin.find("joints");
            if (!joint_list) { skins.push_back(joints); rig->skins.emplace_back(); bind_palettes.emplace_back(); continue; }
            std::vector<Matrix> binds(joint_list->array.size());
            if (const Json* const bind_index = skin.find("inverseBindMatrices")) {
                const Accessor bind = read_accessor(root, bind_index->whole(), binary);
                if (bind.ok && bind.components == 16 && bind.component == 5126)
                    for (uint32_t j = 0; j < bind.count && j < binds.size(); ++j) {
                        const uint8_t* const bytes_at = bind.data + uint64_t(j) * bind.stride;
                        for (int i = 0; i < 16; ++i) binds[j].m[i] = read_float(bytes_at + i * 4, bind.component);
                    }
            }
            GltfRig::Skin retained;
            retained.inverse_bind = binds;
            std::vector<Matrix> bind_palette;
            for (size_t j = 0; j < joint_list->array.size(); ++j) {
                const uint32_t node = joint_list->array[j].whole(~0u);
                retained.nodes.push_back(node);
                bind_palette.push_back(node < node_count ? multiply(rig->bind_world[node], binds[j]) : binds[j]);
                joints.push_back(node < node_count ? multiply(world[node], binds[j]) : binds[j]);
            }
            rig->skins.push_back(std::move(retained));
            bind_palettes.push_back(std::move(bind_palette));
            skins.push_back(std::move(joints));
        }

    GltfModel model;
    // Pictures, decoded once each and only when a part actually uses one. A
    // texture points at an image; an image in a GLB is a slice of the binary
    // chunk. One that is kept outside the file, or in a format the decoder
    // does not read, simply leaves that part with its material's colour.
    static const uint32_t texture_limit = [] {
        const char* const text = std::getenv("SFR_AVATAR_TEXTURE_MAX");
        const long value = text ? std::strtol(text, nullptr, 10) : 2048;
        return uint32_t(value >= 64 ? value : 2048);
    }();
    // SFR_AVATAR_MODEL_PLAIN=1 leaves every picture behind, so that a shape
    // can be looked at without its paint -- and so that a load is quick.
    static const bool plain = [] {
        const char* const text = std::getenv("SFR_AVATAR_MODEL_PLAIN");
        return text && *text && *text != '0';
    }();
    std::map<uint32_t, uint32_t> decoded;
    const auto picture_for = [&](uint32_t texture_index) -> uint32_t {
        if (plain) return ~0u;
        const Json* const textures = root.find("textures");
        const Json* const images = root.find("images");
        if (!textures || !images || texture_index >= textures->array.size()) return ~0u;
        const uint32_t source = textures->array[texture_index].whole_at("source", ~0u);
        if (source >= images->array.size()) return ~0u;
        if (const auto found = decoded.find(source); found != decoded.end()) return found->second;
        decoded[source] = ~0u;  // asked for and answered, even when the answer is no
        const Json& image = images->array[source];
        const Json* const view_index = image.find("bufferView");
        if (!view_index) return ~0u;
        const Json* const views = root.find("bufferViews");
        if (!views || view_index->whole(~0u) >= views->array.size()) return ~0u;
        const Json& view = views->array[view_index->whole()];
        const uint64_t offset = view.whole_at("byteOffset"), length = view.whole_at("byteLength");
        if (!length || offset + length > binary.size()) return ~0u;
        const std::vector<uint8_t> bytes(binary.begin() + int64_t(offset), binary.begin() + int64_t(offset + length));
        DecodedImage picture = decode_image(bytes);
        if (!picture) return ~0u;
        model.images.push_back(shrink_to(std::move(picture), texture_limit));
        decoded[source] = uint32_t(model.images.size() - 1);
        return decoded[source];
    };
    bool any = false;
    for (size_t index = 0; index < node_count; ++index) {
        const Json* const mesh_index = nodes->array[index].find("mesh");
        if (!mesh_index) continue;
        const uint32_t which = mesh_index->whole(~0u);
        if (which >= meshes->array.size()) continue;
        // A skinned mesh takes its place from its bones, not from the node it
        // hangs on: glTF says that node's own transform is not applied.
        const std::vector<Matrix>* skin = nullptr;
        uint32_t chosen_skin = ~0u;
        if (const Json* const skin_index = nodes->array[index].find("skin")) {
            const uint32_t chosen = skin_index->whole(~0u);
            if (chosen < skins.size() && !skins[chosen].empty()) { skin = &skins[chosen]; chosen_skin = chosen; }
        }
        const Matrix unmoved;
        const Matrix& place = skin ? unmoved : world[index];
        const Json* const primitives = meshes->array[which].find("primitives");
        if (!primitives) continue;
        for (const Json& primitive : primitives->array) {
            const Json* const attributes = primitive.find("attributes");
            if (!attributes) continue;
            const Json* const position_index = attributes->find("POSITION");
            if (!position_index) continue;
            const Accessor positions = read_accessor(root, position_index->whole(), binary);
            if (!positions.ok || positions.components != 3 || positions.component != 5126) continue;
            GltfPrimitive out;
            GltfRig::Primitive source;
            source.node = uint32_t(index);
            source.skin = chosen_skin;
            source.positions.reserve(size_t(positions.count) * 3);
            out.material = primitive.whole_at("material", ~0u);
            if (const Json* const materials = root.find("materials"); materials && out.material < materials->array.size()) {
                const Json& material = materials->array[out.material];
                if (const Json* sided = material.find("doubleSided")) out.double_sided = sided->boolean;
                if (const Json* extensions = material.find("extensions"))
                    out.unlit = extensions->find("KHR_materials_unlit") != nullptr;
                if (const Json* const pbr = material.find("pbrMetallicRoughness")) {
                    if (const Json* const base = pbr->find("baseColorFactor"); base && base->array.size() >= 3)
                        for (size_t channel = 0; channel < base->array.size() && channel < 4; ++channel)
                            out.colour[channel] = float(base->array[channel].number);
                    if (const Json* const base = pbr->find("baseColorTexture"))
                        out.image = picture_for(base->whole_at("index", ~0u));
                }
                // What the material calls see-through. A cut-out is all this
                // draws: a part asking to be blended is cut out at the same
                // half-way mark, which is right for hair and wrong for glass.
                if (const Json* const mode = material.find("alphaMode"); mode && mode->text != "OPAQUE") {
                    const Json* const cutoff = material.find("alphaCutoff");
                    out.alpha_cutoff = cutoff && cutoff->kind == Json::Kind::number ? float(cutoff->number) : 0.5f;
                }
            }
            // What moves each vertex: the bones it is weighted to when the
            // mesh is skinned, and the node's own transform when it is not.
            Accessor joints, weights;
            if (skin) {
                if (const Json* const joint_index = attributes->find("JOINTS_0"))
                    joints = read_accessor(root, joint_index->whole(), binary);
                if (const Json* const weight_index = attributes->find("WEIGHTS_0"))
                    weights = read_accessor(root, weight_index->whole(), binary);
            }
            const bool skinning = skin && joints.ok && weights.ok && joints.components == 4 && weights.components == 4
                                  && joints.count == positions.count && weights.count == positions.count;
            if (skinning) {
                source.influences.resize(positions.count);
                for (uint32_t vertex = 0; vertex < positions.count; ++vertex)
                    for (uint32_t i = 0; i < 4; ++i) {
                        source.influences[vertex].joints[i] = read_index(joints.data + uint64_t(vertex) * joints.stride + i * component_size(joints.component), joints.component);
                        source.influences[vertex].weights[i] = read_weight(weights.data + uint64_t(vertex) * weights.stride + i * component_size(weights.component), weights.component);
                    }
            }
            const auto bone_matrix = [&](uint32_t vertex) {
                Matrix out;
                for (float& element : out.m) element = 0;
                const uint8_t* const joint_bytes = joints.data + uint64_t(vertex) * joints.stride;
                const uint8_t* const weight_bytes = weights.data + uint64_t(vertex) * weights.stride;
                const uint32_t joint_size = component_size(joints.component);
                const uint32_t weight_size = component_size(weights.component);
                float total = 0;
                for (uint32_t influence = 0; influence < 4; ++influence) {
                    const float weight = read_weight(weight_bytes + influence * weight_size, weights.component);
                    if (weight <= 0) continue;
                    const uint32_t which = read_index(joint_bytes + influence * joint_size, joints.component);
                    if (which >= skin->size()) continue;
                    for (int element = 0; element < 16; ++element) out.m[element] += (*skin)[which].m[element] * weight;
                    total += weight;
                }
                // Weights that do not add up to one would drag the vertex
                // toward the origin; a vertex with no weight at all stays put.
                if (total <= 0.0001f) return Matrix{};
                if (std::fabs(total - 1.0f) > 0.0001f)
                    for (float& element : out.m) element /= total;
                return out;
            };
            out.positions.reserve(size_t(positions.count) * 3);
            for (uint32_t vertex = 0; vertex < positions.count; ++vertex) {
                const uint8_t* const bytes_at = positions.data + uint64_t(vertex) * positions.stride;
                const float x = read_float(bytes_at, positions.component);
                const float y = read_float(bytes_at + 4, positions.component);
                const float z = read_float(bytes_at + 8, positions.component);
                source.positions.insert(source.positions.end(), {x, y, z});
                const Matrix bones = skinning ? bone_matrix(vertex) : Matrix{};
                const Matrix& moves = skinning ? bones : place;
                out.positions.push_back(moves.m[0] * x + moves.m[4] * y + moves.m[8] * z + moves.m[12]);
                out.positions.push_back(moves.m[1] * x + moves.m[5] * y + moves.m[9] * z + moves.m[13]);
                out.positions.push_back(moves.m[2] * x + moves.m[6] * y + moves.m[10] * z + moves.m[14]);
            }
            if (const Json* const normal_index = attributes->find("NORMAL")) {
                const Accessor normals = read_accessor(root, normal_index->whole(), binary);
                if (normals.ok && normals.components == 3 && normals.component == 5126 && normals.count == positions.count) {
                    out.normals.reserve(size_t(normals.count) * 3);
                    source.normals.reserve(size_t(normals.count) * 3);
                    for (uint32_t vertex = 0; vertex < normals.count; ++vertex) {
                        const uint8_t* const bytes_at = normals.data + uint64_t(vertex) * normals.stride;
                        const float x = read_float(bytes_at, normals.component);
                        const float y = read_float(bytes_at + 4, normals.component);
                        const float z = read_float(bytes_at + 8, normals.component);
                        source.normals.insert(source.normals.end(), {x, y, z});
                        // The rotation part only: a normal carries no position.
                        const Matrix bones = skinning ? bone_matrix(vertex) : Matrix{};
                        const Matrix& moves = skinning ? bones : place;
                        float nx = moves.m[0] * x + moves.m[4] * y + moves.m[8] * z;
                        float ny = moves.m[1] * x + moves.m[5] * y + moves.m[9] * z;
                        float nz = moves.m[2] * x + moves.m[6] * y + moves.m[10] * z;
                        const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
                        if (length > 0) { nx /= length; ny /= length; nz /= length; }
                        out.normals.push_back(nx);
                        out.normals.push_back(ny);
                        out.normals.push_back(nz);
                    }
                }
            }
            if (const Json* const texture_index = attributes->find("TEXCOORD_0")) {
                const Accessor coordinates = read_accessor(root, texture_index->whole(), binary);
                if (coordinates.ok && coordinates.components == 2 && coordinates.component == 5126
                    && coordinates.count == positions.count) {
                    out.texcoords.reserve(size_t(coordinates.count) * 2);
                    for (uint32_t vertex = 0; vertex < coordinates.count; ++vertex) {
                        const uint8_t* const bytes_at = coordinates.data + uint64_t(vertex) * coordinates.stride;
                        out.texcoords.push_back(read_float(bytes_at, coordinates.component));
                        out.texcoords.push_back(read_float(bytes_at + 4, coordinates.component));
                    }
                }
            }
            // A picture with nowhere to put it is no use.
            if (out.texcoords.size() != size_t(positions.count) * 2) out.image = ~0u;
            if (const Json* const index_accessor = primitive.find("indices")) {
                const Accessor indices = read_accessor(root, index_accessor->whole(), binary);
                if (!indices.ok || indices.components != 1) continue;
                out.indices.reserve(indices.count);
                bool sane = true;
                for (uint32_t element = 0; element < indices.count; ++element) {
                    const uint32_t value = read_index(indices.data + uint64_t(element) * indices.stride, indices.component);
                    if (value >= positions.count) { sane = false; break; }
                    out.indices.push_back(value);
                }
                if (!sane) continue;
            } else {
                out.indices.reserve(positions.count);
                for (uint32_t vertex = 0; vertex < positions.count; ++vertex) out.indices.push_back(vertex);
            }
            if (out.indices.size() < 3) continue;
            for (size_t element = 0; element + 2 < out.positions.size(); element += 3) {
                for (int axis = 0; axis < 3; ++axis) {
                    const float value = out.positions[element + size_t(axis)];
                    if (!any) model.lowest[axis] = model.highest[axis] = value;
                    else {
                        model.lowest[axis] = (std::min)(model.lowest[axis], value);
                        model.highest[axis] = (std::max)(model.highest[axis], value);
                    }
                }
                any = true;  // after the first vertex, not after the last
            }
            model.vertices += positions.count;
            model.triangles += out.indices.size() / 3;
            for (size_t vertex = 0; vertex < source.positions.size() / 3; ++vertex) {
                const Matrix transform = skinning ? weighted_matrix(source.influences[vertex], bind_palettes[chosen_skin])
                    : (skin ? Matrix{} : rig->bind_world[index]);
                const float y = transform.m[1] * source.positions[vertex * 3]
                    + transform.m[5] * source.positions[vertex * 3 + 1]
                    + transform.m[9] * source.positions[vertex * 3 + 2] + transform.m[13];
                rig->anchor_y = (std::min)(rig->anchor_y, y);
            }
            rig->primitives.push_back(std::move(source));
            model.primitives.push_back(std::move(out));
        }
    }
    if (model.primitives.empty()) { fail(error, "no drawable primitive"); return std::nullopt; }
    model.ground_y = rig->anchor_y;
    if (facing_backwards) {
        // A VRM 0.x model faces -z where a 1.0 one faces +z. Turning it half a
        // turn about y -- which negates x and z -- means everything that draws
        // one of these can treat both the same way.
        for (GltfPrimitive& primitive : model.primitives) {
            for (size_t element = 0; element + 2 < primitive.positions.size(); element += 3) {
                primitive.positions[element] = -primitive.positions[element];
                primitive.positions[element + 2] = -primitive.positions[element + 2];
            }
            for (size_t element = 0; element + 2 < primitive.normals.size(); element += 3) {
                primitive.normals[element] = -primitive.normals[element];
                primitive.normals[element + 2] = -primitive.normals[element + 2];
            }
        }
        for (int axis : {0, 2}) {
            const float lowest = -model.highest[axis], highest = -model.lowest[axis];
            model.lowest[axis] = lowest;
            model.highest[axis] = highest;
        }
    }
    const bool has_native_bone = std::any_of(rig->native_bone.begin(), rig->native_bone.end(), [](int bone) { return bone >= 0; });
    const bool has_weights = std::any_of(rig->primitives.begin(), rig->primitives.end(), [](const auto& part) { return !part.influences.empty(); });
    if (has_native_bone && has_weights) model.rig = std::move(rig);
    return model;
}

std::optional<GltfModel> load_binary_gltf(const std::filesystem::path& file, std::string* error, GltfPose pose) {
    std::ifstream in(file, std::ios::binary);
    if (!in) { fail(error, "the file cannot be opened"); return std::nullopt; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return read_binary_gltf(bytes, error, pose);
}

}
