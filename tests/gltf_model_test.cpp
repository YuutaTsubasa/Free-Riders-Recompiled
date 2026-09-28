#include "gltf_model.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool near(float a, float b, float slack = 1e-4f) { return std::fabs(a - b) <= slack; }

void put32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) bytes.push_back(uint8_t(value >> shift));
}

// A binary glTF of one triangle, built here so the test needs no file.
std::vector<uint8_t> one_triangle(const std::string& extra_node = {}, bool with_normals = true,
                                const std::string& material_properties = {}) {
    std::vector<float> positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::vector<float> normals = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    std::vector<uint16_t> indices = {0, 1, 2};
    std::vector<uint8_t> binary;
    for (float value : positions) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, 4);
        put32(binary, bits);
    }
    const uint32_t normals_at = uint32_t(binary.size());
    for (float value : normals) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, 4);
        put32(binary, bits);
    }
    const uint32_t indices_at = uint32_t(binary.size());
    for (uint16_t value : indices) { binary.push_back(uint8_t(value)); binary.push_back(uint8_t(value >> 8)); }
    while (binary.size() % 4) binary.push_back(0);

    std::string json = "{\"asset\":{\"version\":\"2.0\"},";
    json += "\"nodes\":[" + std::string(extra_node.empty() ? "{\"mesh\":0}" : extra_node) + "],";
    json += "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0";
    if (with_normals) json += ",\"NORMAL\":1";
    json += "},\"indices\":2,\"material\":3}]}],";
    json += "\"accessors\":[";
    json += "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},";
    json += "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},";
    json += "{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],";
    json += "\"bufferViews\":[";
    json += "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},";
    json += "{\"buffer\":0,\"byteOffset\":" + std::to_string(normals_at) + ",\"byteLength\":36},";
    json += "{\"buffer\":0,\"byteOffset\":" + std::to_string(indices_at) + ",\"byteLength\":6}],";
    json += "\"materials\":[{},{},{},{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.25,0.5,0.75,0.5]}" + material_properties + "}],";
    json += "\"buffers\":[{\"byteLength\":" + std::to_string(binary.size()) + "}]}";
    while (json.size() % 4) json += ' ';

    std::vector<uint8_t> out;
    put32(out, 0x46546C67u);
    put32(out, 2);
    put32(out, 0);  // patched below
    put32(out, uint32_t(json.size()));
    put32(out, 0x4E4F534Au);
    out.insert(out.end(), json.begin(), json.end());
    put32(out, uint32_t(binary.size()));
    put32(out, 0x004E4942u);
    out.insert(out.end(), binary.begin(), binary.end());
    const uint32_t length = uint32_t(out.size());
    for (int shift = 0, at = 8; shift < 32; shift += 8, ++at) out[size_t(at)] = uint8_t(length >> shift);
    return out;
}

// A binary glTF of one skinned triangle hanging off a single bone, which the
// VRM extension calls the spine. Everything the pose code needs and nothing
// else: one joint, full weight, an identity bind pose.
std::vector<uint8_t> skinned_triangle(bool vrm_one_point_oh = true, bool hierarchy = false, bool scaled_parent = false,
                                    bool hands = false) {
    const std::vector<float> positions = hands ? std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0}
                                              : std::vector<float>{0, 1, 0, 1, 1, 0, 0, 1, 1};
    const std::vector<float> normals = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    const std::vector<float> weights = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    const float diagonal = std::sqrt(0.5f);
    const std::vector<float> bind = hands
        ? std::vector<float>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}
        : scaled_parent
        ? std::vector<float>{diagonal/2, -diagonal/2, 0, 0, diagonal, diagonal, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}
        : hierarchy
        ? std::vector<float>{0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, -3, 0, 0, 1}
        : std::vector<float>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::vector<uint8_t> binary;
    const auto put_floats = [&binary](const std::vector<float>& values) {
        for (float value : values) {
            uint32_t bits = 0;
            std::memcpy(&bits, &value, 4);
            put32(binary, bits);
        }
    };
    put_floats(positions);
    const uint32_t normals_at = uint32_t(binary.size());
    put_floats(normals);
    const uint32_t joints_at = uint32_t(binary.size());
    for (int vertex = 0; vertex < 3; ++vertex) for (int influence = 0; influence < 4; ++influence) binary.push_back(0);
    const uint32_t weights_at = uint32_t(binary.size());
    put_floats(weights);
    const uint32_t bind_at = uint32_t(binary.size());
    put_floats(bind);
    const uint32_t indices_at = uint32_t(binary.size());
    for (uint16_t value : {uint16_t(0), uint16_t(1), uint16_t(2)}) {
        binary.push_back(uint8_t(value));
        binary.push_back(uint8_t(value >> 8));
    }
    while (binary.size() % 4) binary.push_back(0);

    std::string json = "{\"asset\":{\"version\":\"2.0\"},";
    if (hands) {
        json += std::string(R"("nodes":[{"mesh":0,"skin":0},{"translation":[0,2,0],"children":[2,3])") +
            (scaled_parent ? R"(,"scale":[2,1,1])" : "") +
            R"(},{"translation":[1,2,3],"rotation":[0,0,0.3826834323650898,0.9238795325112867]},{"translation":[-1,2,3]}],)";
    } else json += scaled_parent
        ? R"("nodes":[{"mesh":0,"skin":0},{"scale":[2,1,1],"children":[2]},{"rotation":[0,0,0.3826834323650898,0.9238795325112867]}],)"
        : hierarchy
        ? R"("nodes":[{"mesh":0,"skin":0},{"translation":[0,2,0],"rotation":[0,0,0.7071067811865476,0.7071067811865476],"children":[2]},{"translation":[1,0,0]}],)"
        : R"("nodes":[{"mesh":0,"skin":0},{"translation":[0,0,0]}],)";
    json += "\"skins\":[{\"joints\":[" + std::string(hierarchy || hands ? "2" : "1") + "],\"inverseBindMatrices\":5}],";
    json += "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,";
    json += "\"JOINTS_0\":3,\"WEIGHTS_0\":4},\"indices\":2}]}],";
    json += "\"accessors\":[";
    json += "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},";
    json += "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},";
    json += "{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},";
    json += "{\"bufferView\":3,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},";
    json += "{\"bufferView\":4,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},";
    json += "{\"bufferView\":5,\"componentType\":5126,\"count\":1,\"type\":\"MAT4\"}],";
    json += "\"bufferViews\":[";
    json += "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},";
    json += "{\"buffer\":0,\"byteOffset\":" + std::to_string(normals_at) + ",\"byteLength\":36},";
    json += "{\"buffer\":0,\"byteOffset\":" + std::to_string(indices_at) + ",\"byteLength\":6},";
    json += "{\"buffer\":0,\"byteOffset\":" + std::to_string(joints_at) + ",\"byteLength\":12},";
    json += "{\"buffer\":0,\"byteOffset\":" + std::to_string(weights_at) + ",\"byteLength\":48},";
    json += "{\"buffer\":0,\"byteOffset\":" + std::to_string(bind_at) + ",\"byteLength\":64}],";
    if (hands) {
        json += vrm_one_point_oh
            ? R"("extensions":{"VRMC_vrm":{"humanoid":{"humanBones":{"hips":{"node":1},"leftHand":{"node":2},"rightHand":{"node":3}}}}},)"
            : R"("extensions":{"VRM":{"humanoid":{"humanBones":[{"bone":"hips","node":1},{"bone":"leftHand","node":2},{"bone":"rightHand","node":3}]}}},)";
    } else json += hierarchy
                ? (vrm_one_point_oh
                    ? R"("extensions":{"VRMC_vrm":{"humanoid":{"humanBones":{"hips":{"node":1},"spine":{"node":2}}}}},)"
                    : R"("extensions":{"VRM":{"humanoid":{"humanBones":[{"bone":"hips","node":1},{"bone":"spine","node":2}]}}},)")
                : vrm_one_point_oh
                ? "\"extensions\":{\"VRMC_vrm\":{\"humanoid\":{\"humanBones\":{\"spine\":{\"node\":1}}}}},"
                : "\"extensions\":{\"VRM\":{\"humanoid\":{\"humanBones\":[{\"bone\":\"spine\",\"node\":1}]}}},";
    json += "\"buffers\":[{\"byteLength\":" + std::to_string(binary.size()) + "}]}";
    while (json.size() % 4) json += ' ';

    std::vector<uint8_t> out;
    put32(out, 0x46546C67u);
    put32(out, 2);
    put32(out, 0);  // patched below
    put32(out, uint32_t(json.size()));
    put32(out, 0x4E4F534Au);
    out.insert(out.end(), json.begin(), json.end());
    put32(out, uint32_t(binary.size()));
    put32(out, 0x004E4942u);
    out.insert(out.end(), binary.begin(), binary.end());
    const uint32_t length = uint32_t(out.size());
    for (int shift = 0, at = 8; shift < 32; shift += 8, ++at) out[size_t(at)] = uint8_t(length >> shift);
    return out;
}

void a_triangle_comes_back() {
    std::string error;
    const auto model = sfr::read_binary_gltf(one_triangle(), &error);
    require(model.has_value(), error.empty() ? "one triangle reads" : error.c_str());
    require(model->primitives.size() == 1, "one primitive");
    require(model->vertices == 3 && model->triangles == 1, "three vertices, one triangle");
    const auto& primitive = model->primitives[0];
    require(primitive.positions.size() == 9, "three positions");
    require(primitive.indices == std::vector<uint32_t>({0, 1, 2}), "the indices come through");
    require(primitive.normals.size() == 9 && near(primitive.normals[2], 1.0f), "the normals do too");
    require(primitive.material == 3, "and which material it wants");
    require(near(primitive.colour[0], 0.25f) && near(primitive.colour[3], 0.5f),
            "and that material's base colour");
    require(near(model->lowest[0], 0) && near(model->highest[0], 1), "the box spans the triangle");
    require(near(model->highest[1], 1) && near(model->lowest[2], 0), "on every axis");
}

// A mesh stands where its node puts it, and a child's node compounds with its
// parent's -- which is how a model's parts end up in one space.
void a_node_moves_its_mesh() {
    const auto model = sfr::read_binary_gltf(
        one_triangle("{\"mesh\":0,\"translation\":[10,0,0],\"scale\":[2,2,2]}"), nullptr);
    require(model.has_value(), "the placed triangle reads");
    const auto& positions = model->primitives[0].positions;
    require(near(positions[0], 10) && near(positions[3], 12), "translated and scaled");
    require(near(model->lowest[0], 10) && near(model->highest[0], 12), "the box follows");
}

void a_rotation_turns_the_normals() {
    // A quarter turn about X takes +Y to +Z, and so +Z to -Y.
    const float half = std::sqrt(0.5f);
    const std::string node = "{\"mesh\":0,\"rotation\":[" + std::to_string(half) + ",0,0," + std::to_string(half) + "]}";
    const auto model = sfr::read_binary_gltf(one_triangle(node), nullptr);
    require(model.has_value(), "the turned triangle reads");
    const auto& normals = model->primitives[0].normals;
    require(near(normals[1], -1.0f, 1e-3f) && near(normals[2], 0.0f, 1e-3f), "the normal turned with it");
}

void a_file_without_normals_still_reads() {
    const auto model = sfr::read_binary_gltf(one_triangle({}, false), nullptr);
    require(model.has_value(), "a file with no normals reads");
    require(model->primitives[0].normals.empty(), "and says it has none");
}

// The stance turns the bones the VRM names and the mesh follows. This file's
// only bone is the spine, which leans eight degrees forward: a point a metre
// above it swings that far toward the front.
void a_riding_pose_moves_the_skin() {
    std::string error;
    const auto model = sfr::read_binary_gltf(skinned_triangle(), &error, sfr::GltfPose::riding);
    require(model.has_value(), error.empty() ? "the skinned triangle reads" : error.c_str());
    const auto& primitive = model->primitives[0];
    const float lean = 8.0f * 3.14159265f / 180.0f;
    require(near(primitive.positions[1], std::cos(lean), 1e-3f), "the vertex leans with the bone");
    require(near(primitive.positions[2], std::sin(lean), 1e-3f), "forward, not backward");
    require(near(primitive.normals[1], -std::sin(lean), 1e-3f) && near(primitive.normals[2], std::cos(lean), 1e-3f),
            "and its normal leans the same way");
}

// Asked for the file as it was authored, nothing is turned -- which is what
// makes it possible to tell a posing mistake from a modelling one.
void the_rest_pose_leaves_the_file_alone() {
    const auto model = sfr::read_binary_gltf(skinned_triangle(), nullptr, sfr::GltfPose::rest);
    require(model.has_value(), "the skinned triangle reads unposed");
    const auto& positions = model->primitives[0].positions;
    require(near(positions[1], 1.0f) && near(positions[2], 0.0f), "the vertex is where the file put it");
}

// A VRM 0.x model faces -z where a 1.0 one faces +z, and comes out of here
// facing the same way as a 1.0 one -- stance and all, not just geometry.
void an_older_vrm_faces_the_same_way() {
    const auto older = sfr::read_binary_gltf(skinned_triangle(false), nullptr, sfr::GltfPose::riding);
    const auto newer = sfr::read_binary_gltf(skinned_triangle(true), nullptr, sfr::GltfPose::riding);
    require(older.has_value() && newer.has_value(), "both read");
    const auto& old_positions = older->primitives[0].positions;
    const auto& new_positions = newer->primitives[0].positions;
    // The lean ends up forward in both, although one file was authored facing
    // the other way and had to be posed the other way round to get there.
    require(new_positions[2] > 0.1f && near(old_positions[2], new_positions[2], 1e-3f),
            "both lean the same way, and forward");
    require(near(old_positions[1], new_positions[1], 1e-3f), "and by the same amount");
    // The geometry itself is turned: what the older file put on one side is on
    // the other, which is what facing the other way means.
    require(near(old_positions[3], -new_positions[3], 1e-3f), "the older model is turned to face us the same way");
    const auto unposed = sfr::read_binary_gltf(skinned_triangle(false), nullptr, sfr::GltfPose::rest);
    require(unposed.has_value() && near(unposed->primitives[0].positions[3], -1.0f),
            "and an older file is turned even when it is not posed");
}

void what_is_refused() {
    std::string error;
    require(!sfr::read_binary_gltf({}, &error) && !error.empty(), "an empty file is refused with a reason");
    std::vector<uint8_t> wrong = one_triangle();
    wrong[0] = 'x';
    require(!sfr::read_binary_gltf(wrong, &error), "a file that is not glTF is refused");
    std::vector<uint8_t> newer = one_triangle();
    newer[4] = 3;
    require(!sfr::read_binary_gltf(newer, &error), "a version this does not read is refused");
    // An index past the end of the vertices would read someone else's memory.
    std::vector<uint8_t> broken = one_triangle();
    broken[broken.size() - 4] = 3;  // the last index, followed by two padding bytes
    require(!sfr::read_binary_gltf(broken, &error), "an index outside the vertex array is refused");
}

void cyclic_nodes_are_refused() {
    std::string error;
    require(!sfr::read_binary_gltf(one_triangle("{\"mesh\":0,\"children\":[1]},{\"children\":[0]}"), &error)
                && !error.empty(), "a two-node cycle is refused");
    require(!sfr::read_binary_gltf(one_triangle("{\"mesh\":0,\"children\":[1]},{\"children\":[2]},{\"children\":[0]}"), &error),
            "a longer cycle is refused");
    require(!sfr::read_binary_gltf(one_triangle("{\"mesh\":0,\"children\":[0]}"), &error),
            "a self-parent is refused");
    const auto model = sfr::read_binary_gltf(one_triangle("{\"translation\":[2,0,0],\"children\":[1]},{\"mesh\":0}"));
    require(model && near(model->primitives[0].positions[0], 2), "an acyclic child still inherits its parent transform");
}

void deeply_nested_json_is_refused() {
    std::string error;
    const std::string nested = std::string(256, '[') + "0" + std::string(256, ']');
    require(!sfr::read_binary_gltf(one_triangle("{\"mesh\":0,\"extras\":" + nested + "}"), &error)
                && !error.empty(), "excessive array nesting is refused");
    std::string objects = "0";
    for (int depth = 0; depth < 256; ++depth) objects = "{\"x\":" + objects + "}";
    require(!sfr::read_binary_gltf(one_triangle("{\"mesh\":0,\"extras\":" + objects + "}"), &error),
            "excessive object nesting is refused");
    require(bool(sfr::read_binary_gltf(one_triangle("{\"mesh\":0,\"extras\":[[[0]]]}"))),
            "ordinary nested metadata still reads");
}

void native_root_translation_moves_the_whole_model() {
    for (const bool newer : {false, true}) for (const bool hierarchy : {false, true}) {
        auto model = sfr::read_binary_gltf(skinned_triangle(newer, hierarchy));
        require(bool(model), "root motion fixture loads");
        const auto authored = model->primitives[0];
        sfr::AvatarPose pose;
        pose.valid = true;
        pose.bones[0].translation = {0.25f, 0.966f, -0.5f};
        pose.bones[1].translation = {40, 50, 60};
        require(sfr::pose_gltf_model(*model, pose), "native root translation accepted");
        for (size_t i = 0; i < authored.positions.size(); ++i)
            require(near(model->primitives[0].positions[i], authored.positions[i] + pose.bones[0].translation[i % 3]),
                    "native root translation applies once in the same direction for both VRM versions");
        require(near(model->lowest[1], 1.966f) && near(model->highest[1], 1.966f),
                "root rise changes both animated bounds");
        require(near(model->ground_y, 1), "root rise preserves authored ground reference");
        const auto moved = model->primitives[0];
        require(sfr::pose_gltf_model(*model, pose) && model->primitives[0].positions == moved.positions,
                "root motion never accumulates between frames");
        for (int axis = 0; axis < 3; ++axis) {
            const auto finite = pose.bones[0].translation;
            for (const float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
                pose.bones[0].translation[axis] = invalid;
                require(!sfr::pose_gltf_model(*model, pose) && model->primitives[0].positions == moved.positions &&
                        model->primitives[0].normals == moved.normals && near(model->lowest[1], 1.966f),
                        "nonfinite root translation rejects the whole update before mutation");
            }
            pose.bones[0].translation = finite;
        }
        pose.bones[0].translation = {};
        require(sfr::pose_gltf_model(*model, pose) && model->primitives[0].positions == authored.positions,
                "zero root motion restores bind while non-root translations preserve proportions");
    }
}

void native_pose_updates_from_bind() {
    auto model = sfr::read_binary_gltf(skinned_triangle(), nullptr, sfr::GltfPose::riding);
    const auto rest = sfr::read_binary_gltf(skinned_triangle());
    require(model && rest, "animated fixture loads");
    sfr::AvatarPose pose;
    pose.valid = true;
    require(sfr::pose_gltf_model(*model, pose), "native identity pose is accepted");
    require(model->primitives[0].positions == rest->primitives[0].positions,
            "identity restores authored bind, not baked riding vertices");
    const float half = std::sqrt(0.5f);
    pose.bones[1].rotation = {half, 0, 0, half};
    require(sfr::pose_gltf_model(*model, pose), "native spine rotation is accepted");
    const auto moved = model->primitives[0];
    require(near(moved.positions[2], 1) && near(moved.normals[1], -1),
            "native rotation moves weighted vertex and normal");
    require(near(model->lowest[1], -1) && near(model->ground_y, rest->lowest[1]),
            "rotation may lower geometry without changing authored grounding");
    require(sfr::pose_gltf_model(*model, pose) && model->primitives[0].positions == moved.positions,
            "repeated native pose never accumulates");
    pose.bones[1].rotation[0] = std::numeric_limits<float>::quiet_NaN();
    require(!sfr::pose_gltf_model(*model, pose) && model->primitives[0].positions == moved.positions,
            "invalid quaternion rejects whole update without corrupting geometry");
    auto ordinary = sfr::read_binary_gltf(one_triangle());
    const auto before = ordinary->primitives[0].positions;
    require(!sfr::pose_gltf_model(*ordinary, pose) && ordinary->primitives[0].positions == before,
            "ordinary models retain their static geometry");
    pose.bones[1].rotation = {half, 0, 0, half};
    auto older = sfr::read_binary_gltf(skinned_triangle(false));
    require(sfr::pose_gltf_model(*older, pose), "VRM zero native pose accepted");
    require(near(older->primitives[0].positions[2], moved.positions[2]),
            "VRM zero facing conversion preserves native lean direction");
    pose.bones[1].rotation = {2 * half, 0, 0, 2 * half};
    require(sfr::pose_gltf_model(*model, pose) && near(model->primitives[0].normals[1], -1),
            "finite quaternions are normalized");
    pose.bones[1].rotation = {0, 0, 0, 0};
    require(!sfr::pose_gltf_model(*model, pose), "a zero quaternion is refused");
    pose = {};
    require(!sfr::pose_gltf_model(*model, pose), "an unavailable capture keeps the static fallback");
    pose.valid = true;
    pose.bones[1].translation = {40, 50, 60};
    require(sfr::pose_gltf_model(*model, pose) && model->primitives[0].positions == rest->primitives[0].positions,
            "non-root translation never changes VRM proportions");
}

void native_mirroring_reflects_the_completed_pose() {
    for (const bool newer : {false, true}) {
        auto model = sfr::read_binary_gltf(skinned_triangle(newer));
        require(bool(model), "mirroring fixture loads");
        sfr::AvatarPose pose;
        pose.valid = true;
        require(!pose.mirrored, "native mirroring defaults off");
        const float half = std::sqrt(0.5f);
        pose.bones[1].rotation = {0, half, 0, half};
        pose.bones[0].translation = {0.25f, 0.966f, -0.5f};
        require(sfr::pose_gltf_model(*model, pose), "unmirrored reference pose accepted");
        const auto before = model->primitives[0];
        const float min_x = model->lowest[0], max_x = model->highest[0];
        require(std::fabs(before.normals[0]) > 0.9f, "fixture has an X normal to reflect");
        pose.mirrored = true;
        require(sfr::pose_gltf_model(*model, pose), "mirrored pose accepted");
        for (size_t i = 0; i < before.positions.size(); ++i) {
            const float sign = i % 3 == 0 ? -1.0f : 1.0f;
            require(near(model->primitives[0].positions[i], sign * before.positions[i]),
                    "mirroring reflects final X including native root translation");
            require(near(model->primitives[0].normals[i], sign * before.normals[i]),
                    "mirroring reflects normal X while preserving Y and Z");
        }
        require(near(model->lowest[0], -max_x) && near(model->highest[0], -min_x), "mirrored bounds follow reflected vertices");
        require(near(model->ground_y, 1), "mirroring preserves fixed ground reference");
        require(model->primitives[0].indices == before.indices, "CPU reflection leaves winding to the renderer");
        const auto mirrored = model->primitives[0];
        require(sfr::pose_gltf_model(*model, pose) && model->primitives[0].positions == mirrored.positions,
                "repeated reflection rebuilds from bind instead of toggling geometry");
        pose.mirrored = false;
        require(sfr::pose_gltf_model(*model, pose) && model->primitives[0].positions == before.positions,
                "unmirroring restores the same translated pose");
    }
}

void authored_ground_is_defined_for_every_loaded_pose() {
    const auto ordinary = sfr::read_binary_gltf(one_triangle("{\"mesh\":0,\"translation\":[0,3,0]}"));
    const auto rest = sfr::read_binary_gltf(skinned_triangle());
    const auto riding = sfr::read_binary_gltf(skinned_triangle(), nullptr, sfr::GltfPose::riding);
    require(ordinary && rest && riding, "ground reference fixtures load");
    require(near(ordinary->ground_y, 3) && near(rest->ground_y, 1) && near(riding->ground_y, 1),
            "static, rest and riding models retain the authored bind minimum");
    require(!near(riding->lowest[1], riding->ground_y), "static riding deformation does not redefine grounding");
}

void native_pose_inherits_parents_and_authored_axes() {
    auto model = sfr::read_binary_gltf(skinned_triangle(true, true));
    const auto plain = sfr::read_binary_gltf(skinned_triangle());
    require(model && plain, "rotated bind hierarchy reads");
    sfr::AvatarPose pose;
    pose.valid = true;
    require(sfr::pose_gltf_model(*model, pose), "hierarchy identity pose accepted");
    for (size_t i = 0; i < 9; ++i)
        require(near(model->primitives[0].positions[i], plain->primitives[0].positions[i]),
                "authored bind and inverse bind cancel even with rotated and translated parent");
    const float half = std::sqrt(0.5f);
    pose.bones[0].rotation = {half, 0, 0, half};
    require(sfr::pose_gltf_model(*model, pose) && near(model->primitives[0].positions[2], -1),
            "parent rotates child geometry about the parent's authored pivot");
    pose.bones[0].rotation = {0, 0, 0, 1};
    pose.bones[1].rotation = {half, 0, 0, half};
    require(sfr::pose_gltf_model(*model, pose) && near(model->primitives[0].positions[2], -2),
            "native X delta stays X with a bone authored ninety degrees around Z");
    require(near(model->primitives[0].normals[1], -1), "normal uses the retargeted bind basis");
    pose.bones[0].rotation = {half, 0, 0, half};
    require(sfr::pose_gltf_model(*model, pose) && near(model->primitives[0].positions[2], 1),
            "parent and child deltas compose through the hierarchy");
}

void native_identity_preserves_a_scaled_parent() {
    auto model = sfr::read_binary_gltf(skinned_triangle(true, true, true));
    require(bool(model), "nonuniformly scaled rig reads");
    const auto authored = model->primitives[0];
    sfr::AvatarPose pose;
    pose.valid = true;
    require(sfr::pose_gltf_model(*model, pose), "scaled rig identity pose accepted");
    for (size_t i = 0; i < authored.positions.size(); ++i) {
        require(near(model->primitives[0].positions[i], authored.positions[i]),
                "identity preserves geometry under scale(2,1,1) and child Z45");
        require(near(model->primitives[0].normals[i], authored.normals[i]),
                "identity preserves scaled rig normals");
    }
    pose.bones[1].rotation = {std::sqrt(0.5f), 0, 0, std::sqrt(0.5f)};
    require(sfr::pose_gltf_model(*model, pose), "scaled rig animated pose accepted");
    for (float value : model->primitives[0].positions) require(std::isfinite(value), "scaled animation has finite positions");
    for (float value : model->primitives[0].normals) require(std::isfinite(value), "scaled animation has finite normals");
    require(std::fabs(model->primitives[0].positions[2] - authored.positions[2]) > 0.1f,
            "scaled rig still moves under a nonidentity pose");
    require(near(model->primitives[0].positions[2], 1) && near(model->primitives[0].normals[1], -1),
            "native rotation axes exclude inherited nonuniform scale");
}

void native_hand_transforms_match_the_rendered_pose() {
    for (const bool newer : {false, true}) for (const bool scaled : {false, true}) {
        auto model = sfr::read_binary_gltf(skinned_triangle(newer, true, scaled, true));
        require(bool(model), "hand attachment fixture loads");
        sfr::AvatarPose pose;
        pose.valid = true;
        auto hand = sfr::gltf_avatar_bone_transform(*model, pose, 33, 1);
        require(bool(hand), "mapped hand has an attachment transform");
        for (size_t row = 0; row < 3; ++row) for (size_t column = 0; column < 3; ++column)
            require(near((*hand)[row * 4 + column], row == column ? 1.f : 0.f),
                    "identity neutralizes authored hand rotation without inheriting nonuniform scale");
        const float facing = newer ? 1.f : -1.f;
        require(near((*hand)[12], facing * (scaled ? 2.f : 1.f)) && near((*hand)[13], 4 - model->ground_y) &&
                near((*hand)[14], facing * 3), "hand origin includes hierarchy, facing and fixed grounding");
        require(bool(sfr::gltf_avatar_bone_transform(*model, pose, 36, 1)), "right hand maps independently");

        const float half = std::sqrt(0.5f);
        pose.bones[33].rotation = {half, 0, 0, half};
        hand = sfr::gltf_avatar_bone_transform(*model, pose, 33, 2);
        require(hand && near((*hand)[0], 2) && near((*hand)[6], 2) && near((*hand)[9], -2) &&
                near((*hand)[5], 0) && near((*hand)[10], 0),
                "native X hand turn survives authored axes, VRM facing and uniform model scale");
        pose.bones[0].rotation = {0, half, 0, half};
        pose.bones[0].translation = {0.25f, 0.966f, -0.5f};
        pose.bones[33].translation = {100, 200, 300};
        for (const bool mirrored : {false, true}) {
            pose.mirrored = mirrored;
            hand = sfr::gltf_avatar_bone_transform(*model, pose, 33, 2);
            require(hand && sfr::pose_gltf_model(*model, pose), "parent motion and mirror evaluate for mesh and attachment");
            // The first vertex is exactly the hand node's origin; compare in
            // render model space after the same fixed-ground and scale step.
            for (int axis = 0; axis < 3; ++axis)
                require(near((*hand)[12 + axis], 2 * (model->primitives[0].positions[axis] -
                        (axis == 1 ? model->ground_y : 0))), "attachment origin follows the rendered hand exactly");
            require(near((*hand)[3], 0) && near((*hand)[7], 0) && near((*hand)[11], 0) && near((*hand)[15], 1),
                    "attachment output is a row-vector affine matrix");
        }
        pose.mirrored = false;
        const auto unmirrored = sfr::gltf_avatar_bone_transform(*model, pose, 33, 2);
        pose.mirrored = true;
        const auto mirrored = sfr::gltf_avatar_bone_transform(*model, pose, 33, 2);
        require(unmirrored && mirrored, "both reflection transforms available");
        for (size_t i = 0; i < 16; ++i)
            require(near((*mirrored)[i], (*unmirrored)[i] * (i % 4 == 0 ? -1.f : 1.f)),
                    "mirror reflects final X basis and translation once");
        require(!sfr::gltf_avatar_bone_transform(*model, pose, 72, 1) &&
                !sfr::gltf_avatar_bone_transform(*model, pose, 19, 1), "invalid and unmapped bones have no transform");
        for (float scale : {0.f, -1.f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
            require(!sfr::gltf_avatar_bone_transform(*model, pose, 33, scale), "invalid scale is rejected");
        pose.bones[33].rotation = {0, 0, 0, 0};
        require(!sfr::gltf_avatar_bone_transform(*model, pose, 33, 1), "invalid quaternion rejects attachment");
        pose = {};
        require(!sfr::gltf_avatar_bone_transform(*model, pose, 33, 1), "unavailable pose rejects attachment");
        pose.valid = true;
        pose.bones[0].translation[0] = std::numeric_limits<float>::quiet_NaN();
        require(!sfr::gltf_avatar_bone_transform(*model, pose, 33, 1), "invalid root motion rejects attachment");
    }
    sfr::GltfModel empty;
    sfr::AvatarPose pose;
    pose.valid = true;
    require(!sfr::gltf_avatar_bone_transform(empty, pose, 33, 1), "missing rig has no attachment");
}
}

int main(int argc, char** argv) {
    try {
        const auto flags = sfr::read_binary_gltf(one_triangle({}, true,
            R"(,"doubleSided":true,"extensions":{"KHR_materials_unlit":{},"VRMC_materials_mtoon":{"specVersion":"1.0"}})"));
        require(flags && flags->primitives[0].double_sided && flags->primitives[0].unlit,
                "VRM material double-sided and unlit fallback are preserved");
        const auto defaults = sfr::read_binary_gltf(one_triangle());
        require(defaults && !defaults->primitives[0].double_sided && !defaults->primitives[0].unlit,
                "ordinary material keeps lighting and backface culling");
        const auto disabled = sfr::read_binary_gltf(one_triangle({}, true, R"(,"doubleSided":false)"));
        require(disabled && !disabled->primitives[0].double_sided, "explicit single-sided material");
        if (argc > 1) {
            const std::string test = argv[1];
            if (test == "cycles") cyclic_nodes_are_refused();
            else if (test == "depth") deeply_nested_json_is_refused();
            else throw std::runtime_error("unknown test");
            return 0;
        }
        a_triangle_comes_back();
        a_node_moves_its_mesh();
        a_rotation_turns_the_normals();
        a_file_without_normals_still_reads();
        a_riding_pose_moves_the_skin();
        the_rest_pose_leaves_the_file_alone();
        an_older_vrm_faces_the_same_way();
        what_is_refused();
        cyclic_nodes_are_refused();
        deeply_nested_json_is_refused();
        native_root_translation_moves_the_whole_model();
        native_mirroring_reflects_the_completed_pose();
        authored_ground_is_defined_for_every_loaded_pose();
        native_pose_updates_from_bind();
        native_pose_inherits_parents_and_authored_axes();
        native_identity_preserves_a_scaled_parent();
        native_hand_transforms_match_the_rendered_pose();
        std::cout << "glTF model checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
