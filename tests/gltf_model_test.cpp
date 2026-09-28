#include "gltf_model.h"

#include <cmath>
#include <cstring>
#include <iostream>
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
std::vector<uint8_t> one_triangle(const std::string& extra_node = {}, bool with_normals = true) {
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
    json += "\"materials\":[{},{},{},{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.25,0.5,0.75,0.5]}}],";
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
std::vector<uint8_t> skinned_triangle(bool vrm_one_point_oh = true) {
    const std::vector<float> positions = {0, 1, 0, 1, 1, 0, 0, 1, 1};
    const std::vector<float> normals = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    const std::vector<float> weights = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    const std::vector<float> bind = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
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
    json += "\"nodes\":[{\"mesh\":0,\"skin\":0},{\"translation\":[0,0,0]}],";
    json += "\"skins\":[{\"joints\":[1],\"inverseBindMatrices\":5}],";
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
    json += vrm_one_point_oh
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
}

int main(int argc, char** argv) {
    try {
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
        std::cout << "glTF model checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
