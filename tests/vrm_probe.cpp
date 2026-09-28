// Reads a .vrm (a binary glTF) and says what came out, so a model can be
// checked without starting the game:  sfr_vrm_probe <file.vrm> [rest|ride]
//
// The pose matters to what it says: a rider's stance is a crouch, so the box
// around a posed model is shorter than the one around the T-pose it was
// authored in.
#include "gltf_model.h"

#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: sfr_vrm_probe <file.vrm> [rest|ride]\n";
        return 2;
    }
    // The stance the game draws it in, or the file as authored.
    const bool riding = argc > 2 && std::string(argv[2]) == "ride";
    std::string error;
    const auto model = sfr::load_binary_gltf(argv[1], &error,
                                             riding ? sfr::GltfPose::riding : sfr::GltfPose::rest);
    if (!model) {
        std::cerr << "cannot read " << argv[1] << ": " << error << '\n';
        return 1;
    }
    std::cout << "primitives " << model->primitives.size() << ", vertices " << model->vertices
              << ", triangles " << model->triangles << '\n';
    std::cout << "box " << model->lowest[0] << ',' << model->lowest[1] << ',' << model->lowest[2]
              << " to " << model->highest[0] << ',' << model->highest[1] << ',' << model->highest[2] << '\n';
    std::cout << "height " << (model->highest[1] - model->lowest[1]) << " (a metre is one unit in VRM)\n";
    std::cout << "pictures " << model->images.size();
    for (const auto& picture : model->images) std::cout << ' ' << picture.width << 'x' << picture.height;
    std::cout << '\n';
    size_t painted = 0;
    for (const auto& primitive : model->primitives)
        if (primitive.image != ~0u) ++painted;
    std::cout << painted << " of " << model->primitives.size() << " parts are painted with one" << '\n';
    // With a directory named, every picture is written out as raw pixels
    // (width, height, then four bytes each) so that a decoder fault can be
    // looked at rather than guessed at.
    if (argc > 3) {
        for (size_t index = 0; index < model->images.size(); ++index) {
            const std::string name = std::string(argv[3]) + "/picture" + std::to_string(index) + ".raw";
            std::ofstream out(name, std::ios::binary);
            const uint32_t size[2] = {model->images[index].width, model->images[index].height};
            out.write(reinterpret_cast<const char*>(size), sizeof size);
            out.write(reinterpret_cast<const char*>(model->images[index].rgba.data()),
                      std::streamsize(model->images[index].rgba.size()));
        }
        std::cout << "wrote " << model->images.size() << " pictures to " << argv[3] << '\n';
    }
    for (size_t index = 0; index < model->primitives.size(); ++index) {
        const auto& primitive = model->primitives[index];
        std::cout << "  part " << index << " material " << int(primitive.material) << " picture "
                  << int(primitive.image) << " triangles " << primitive.indices.size() / 3
                  << " colour " << primitive.colour[0] << ',' << primitive.colour[1] << ',' << primitive.colour[2]
                  << (primitive.texcoords.empty() ? " (no texture coordinates)" : "") << '\n';
    }
    // With a directory named, each part's finished positions are written out
    // as well, so they can be compared against the file itself.
    if (argc > 3)
        for (size_t index = 0; index < model->primitives.size(); ++index) {
            const std::string name = std::string(argv[3]) + "/part" + std::to_string(index) + ".pos";
            std::ofstream out(name, std::ios::binary);
            out.write(reinterpret_cast<const char*>(model->primitives[index].positions.data()),
                      std::streamsize(model->primitives[index].positions.size() * sizeof(float)));
        }
    size_t without_normals = 0;
    for (const auto& primitive : model->primitives)
        if (primitive.normals.empty()) ++without_normals;
    if (without_normals) std::cout << without_normals << " primitive(s) have no normals\n";
    return 0;
}
