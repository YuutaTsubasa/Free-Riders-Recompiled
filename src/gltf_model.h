#pragma once
#include "image_decode.h"
#include "avatar_pose.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace sfr {

struct GltfRig;

// One drawable piece of a model: triangles in one space, indexed.
struct GltfPrimitive {
    std::vector<float> positions;   // three per vertex
    std::vector<float> normals;     // three per vertex, or empty when the file has none
    std::vector<uint32_t> indices;  // three per triangle
    uint32_t material = ~0u;        // the file's material index, or ~0
    // The material's base colour. A VRM that paints its skin and clothes with
    // textures leaves this white for those parts, so it is a floor to build
    // on rather than the whole answer.
    float colour[4] = {1, 1, 1, 1};
    // Where in that material's picture each vertex sits, two per vertex, and
    // which picture it is: an index into GltfModel::images, or ~0 for a part
    // whose colour is the material's alone.
    std::vector<float> texcoords;
    uint32_t image = ~0u;
    // What the material calls see-through: anything below this much alpha is
    // not drawn at all. Zero for a material that is opaque.
    float alpha_cutoff = 0;
    bool double_sided = false;
    bool unlit = false;
};

struct GltfModel {
    std::vector<GltfPrimitive> primitives;
    // The pictures the primitives point at, decoded, four bytes a pixel. Only
    // the ones something actually uses are here.
    std::vector<DecodedImage> images;
    // The box the whole model occupies, for standing it somewhere and sizing
    // it against a character.
    float lowest[3] = {0, 0, 0}, highest[3] = {0, 0, 0};
    // Authored bind minimum, fixed across animation so jumps and crouches
    // change height relative to the board instead of being grounded again.
    float ground_y = 0;
    uint64_t vertices = 0, triangles = 0;
    // Shared immutable authored geometry, hierarchy and skin influences.
    std::shared_ptr<const GltfRig> rig;
};

// Rebuild geometry from authored bind data and native rotation deltas, then
// apply bone 0 translation in native meters and the final X reflection.
// Other translations are ignored to preserve authored VRM proportions.
// Recalculates current bounds while leaving the bind ground_y fixed.
// Invalid/unrigged input returns false without changing the model.
bool pose_gltf_model(GltfModel& model, const AvatarPose& pose);

// A native bone's model-space attachment transform, encoded for row vectors.
// Shares the mesh pose hierarchy and includes VRM facing, root motion, mirror,
// fixed ground_y and uniform model scale, but not the racer's world transform.
// Grip axes neutralize the authored bind rotation and exclude authored scale
// and shear; this defines normalized humanoid axes, not verified Xbox grip
// calibration. Native per-item offsets remain the caller's responsibility.
// Missing bones/rigs and invalid pose/scale return nothing without mutation.
std::optional<std::array<float, 16>> gltf_avatar_bone_transform(
    const GltfModel& model, const AvatarPose& pose, uint32_t native_bone, float scale);

// How the model should stand.
//
// A VRM is authored in a T-pose, arms straight out, which is not how anyone
// rides a board. `riding` rotates the humanoid bones the VRM extension names
// into a rider's stance at load as a static fallback. Rigged VRMs retain their
// authored bind data so pose_gltf_model can replace that stance each frame.
enum class GltfPose { rest, riding };

// Reads a binary glTF -- which is what a .vrm is -- and returns the geometry
// in one space, each mesh already carrying its node's transform, skinned into
// the pose asked for.
//
// A part's texture is decoded too, and shrunk if it is larger than anything
// worth drawing at: SFR_AVATAR_TEXTURE_MAX, 2048 by default. An image in a
// format the decoder does not read leaves that part with its material's plain
// colour rather than failing the whole model.
//
// Returns nothing when the file is not a binary glTF this can read; when
// `error` is given it is told why.
std::optional<GltfModel> load_binary_gltf(const std::filesystem::path& file, std::string* error = nullptr,
                                          GltfPose pose = GltfPose::rest);

// Whether the player named a model to draw for their Avatar
// (SFR_AVATAR_MODEL). Asked before a frame is presented, so it is read
// once.
bool model_wanted();
std::filesystem::path avatar_model_path();

// The same from bytes already in hand (what the tests use).
std::optional<GltfModel> read_binary_gltf(const std::vector<uint8_t>& bytes, std::string* error = nullptr,
                                          GltfPose pose = GltfPose::rest);

}
