#include "avatar_state.h"
#include "avatar_transform.h"
#include "avatar_clip_pose.h"
#include <limits>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main() {
    try {
        sfr::AvatarClipPose evaluated;
        require(!evaluated.current(1, 2, 3), "no clip before evaluation");
        evaluated.rider = 1; evaluated.animation = 2; evaluated.present = 3;
        evaluated.pose.valid = true;
        evaluated.pose.bones[5].rotation = {0, 0, 0, 1};
        require(evaluated.current(1, 2, 3).has_value(), "current rider receives evaluated clip");
        auto draw_pose = *evaluated.current(1, 2, 3);
        draw_pose.bones[5].rotation[1] = 0.7f;
        require(evaluated.current(1, 2, 3)->bones[5].rotation[1] == 0,
                "later procedural writes cannot alter evaluated clip snapshot");
        require(!evaluated.current(9, 2, 3) && !evaluated.current(1, 9, 3) && !evaluated.current(1, 2, 4),
                "a reused animation address cannot leak another rider or frame");
        const sfr::AvatarMatrix identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        auto world = identity;
        world[12] = 3; world[13] = 4; world[14] = -6;
        const sfr::AvatarMatrix projection{2,0,0,0, 0,3,0,0, 0,0,-1,-1, 0,0,-0.3f,0};
        const auto clip = sfr::avatar_model_to_clip(world, identity, projection, 2, -1);
        require(clip[0] == 4 && clip[5] == 6 && clip[10] == -2 && clip[11] == -2,
                "model scale and perspective matrix convention");
        require(clip[12] == 6 && clip[13] == 18 && std::abs(clip[14] - 5.7f) < 0.0001f && clip[15] == 6,
                "feet align before world position and perspective");
        sfr::AvatarFrameTransforms frames;
        require(!frames.consume(), "no transform before a draw");
        frames.publish({world, identity, projection});
        require(frames.consume().has_value(), "current frame receives transform");
        require(!frames.consume(), "previous frame transform cannot leak");
        auto invalid = world;
        invalid[0] = std::numeric_limits<float>::quiet_NaN();
        frames.publish({world, identity, projection});
        frames.publish({invalid, identity, projection});
        require(!frames.consume(), "invalid transforms suppress drawing");
        sfr::GuestMemory memory;
        require(sfr::single_player_avatar_racer(memory) == 0, "unmapped state");
        memory.map(0x83E50000, 0x10000);
        memory.map(0x82B00000, 0x10000);
        memory.map(0x10000000, 0x10000);
        constexpr uint32_t manager = 0x10000000, list = 0x10001000, rider = 0x10002000;
        memory.store<uint32_t>(0x83E52F8C, 1);
        memory.store<uint8_t>(0x82B0569F, 1);
        memory.store<uint32_t>(0x83E52FDC, manager);
        memory.store<uint32_t>(manager + 20, 1);
        memory.store<uint32_t>(manager + 36, list);
        memory.store<uint32_t>(manager + 40, list + 4);
        memory.store<uint32_t>(list, rider);
        memory.store<uint32_t>(rider + 100, 17);
        require(sfr::single_player_avatar_racer(memory) == rider, "live single-player Avatar");
        memory.store<uint8_t>(0x83E515FB, 0);
        require(sfr::single_player_avatar_racer(memory) == rider, "menu state is not race count");
        // Loading creates the local preview before all planned race entrants.
        memory.store<uint32_t>(manager + 20, 12);
        require(sfr::single_player_avatar_racer(memory) == rider,
                "Loading Avatar remains valid with one initialized racer and twelve planned entrants");
        memory.store<uint32_t>(manager + 40, list + 12 * 4);
        require(sfr::single_player_avatar_racer(memory) == rider,
                "full race list retains the same first-player identity");
        memory.store<uint32_t>(manager + 40, list + 4);
        for (const auto character : {0u, 1u, 18u}) {
            memory.store<uint32_t>(rider + 100, character);
            require(sfr::single_player_avatar_racer(memory) == 0, "ordinary character must not draw VRM");
        }
        memory.store<uint32_t>(rider + 100, 17);
        for (const uint8_t count : {0, 2}) {
            memory.store<uint8_t>(0x82B0569F, count);
            require(sfr::single_player_avatar_racer(memory) == 0, "only confirmed single-player races");
        }
        memory.store<uint8_t>(0x82B0569F, 1);
        memory.store<uint32_t>(0x83E52F8C, 0);
        require(sfr::single_player_avatar_racer(memory) == 0, "no model outside race");
        memory.store<uint32_t>(0x83E52F8C, 1);
        for (const uint32_t end : {list, list - 4, list + 3, 0x20000000u}) {
            memory.store<uint32_t>(manager + 40, end);
            require(sfr::single_player_avatar_racer(memory) == 0, "invalid live racer list");
        }
        memory.store<uint32_t>(manager + 40, list + 4);
        memory.store<uint32_t>(manager + 20, 0);
        require(sfr::single_player_avatar_racer(memory) == 0, "empty manager");
        memory.store<uint32_t>(manager + 20, 1);
        for (const uint32_t pointer : {0u, 0xFFFFFFF0u, 0x20000000u}) {
            memory.store<uint32_t>(list, pointer);
            require(sfr::single_player_avatar_racer(memory) == 0, "invalid racer pointer");
        }
        memory.store<uint32_t>(list, rider);
        for (const uint32_t pointer : {0u, 0xFFFFFFF0u, 0x20000000u}) {
            memory.store<uint32_t>(0x83E52FDC, pointer);
            require(sfr::single_player_avatar_racer(memory) == 0, "invalid or destroyed manager");
        }
        memory.store<uint32_t>(0x83E52FDC, manager);
        require(sfr::single_player_avatar_racer(memory) == rider, "new race reacquires current rider");
        std::cout << "Avatar race state tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
