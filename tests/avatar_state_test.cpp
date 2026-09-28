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
        evaluated.manager = 4; evaluated.renderer = 5; evaluated.controller = 6;
        require(evaluated.current(1, 2, 3, 4, 5, 6).has_value(), "complete current owner identity");
        require(!evaluated.current(1, 2, 3, 9, 5, 6) && !evaluated.current(1, 2, 3, 4, 9, 6) &&
                !evaluated.current(1, 2, 3, 4, 5, 9), "replaced manager, renderer or evaluator cannot reuse pose");
        const sfr::AvatarMatrix identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        auto world = identity;
        world[12] = 3; world[13] = 4; world[14] = -6;
        const sfr::AvatarMatrix projection{2,0,0,0, 0,3,0,0, 0,0,-1,-1, 0,0,-0.3f,0};
        const auto clip = sfr::avatar_model_to_clip(world, identity, projection, 2, -1);
        require(clip[0] == 4 && clip[5] == 6 && clip[10] == -2 && clip[11] == -2,
                "model scale and perspective matrix convention");
        require(clip[12] == 6 && clip[13] == 18 && std::abs(clip[14] - 5.7f) < 0.0001f && clip[15] == 6,
                "feet align before world position and perspective");
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
        constexpr uint32_t rider2 = 0x10003000, cpu = 0x10004000;
        constexpr uint32_t character = 0x10005000, character2 = 0x10005100, cpu_character = 0x10005200;
        constexpr uint32_t renderer = 0x10006000, renderer2 = 0x10006100, cpu_renderer = 0x10006200;
        constexpr uint32_t controller = 0x10007000, animation = 0x10008000;
        auto bind = [&](uint32_t owner, uint32_t model, uint32_t render) {
            memory.store<uint32_t>(owner + 100, 17);
            memory.store<uint32_t>(owner + 3208, model);
            memory.store<uint32_t>(model + 8, render);
            memory.store<uint32_t>(model + 12, controller);
            memory.store<uint32_t>(model + 16, animation);
        };
        bind(rider, character, renderer);
        bind(rider2, character2, renderer2);
        bind(cpu, cpu_character, cpu_renderer);
        memory.store<uint32_t>(list + 4, rider2);
        memory.store<uint32_t>(list + 8, cpu);
        memory.store<uint32_t>(manager + 20, 12);
        memory.store<uint32_t>(manager + 40, list + 12);
        const auto first = sfr::local_avatar_for_renderer(memory, renderer);
        require(first && first->rider == rider,
                "single-player renderer resolves the local Avatar");
        require(!sfr::local_avatar_for_renderer(memory, renderer2), "one-player race excludes later entrants");
        memory.store<uint8_t>(0x82B0569F, 2);
        memory.store<uint32_t>(rider + 100, 0);
        const auto second = sfr::local_avatar_for_renderer(memory, renderer2);
        require(second && second->manager == manager && second->rider == rider2 &&
                    second->renderer == renderer2 && second->controller == controller &&
                    second->animation == animation && second->local_slot == 1 && second->local_count == 2,
                "player two Avatar resolves its own model and animation identity");
        require(!sfr::local_avatar_for_renderer(memory, renderer), "ordinary first player does not become Avatar");
        require(!sfr::local_avatar_for_renderer(memory, cpu_renderer), "CPU Avatar cannot use the local model");
        require(sfr::local_avatar_for_animation(memory, controller, renderer2, animation).has_value(),
                "matching local animation is accepted");
        const auto animation_owner = sfr::local_avatar_for_animation(memory, animation);
        require(animation_owner && animation_owner->rider == rider2 && animation_owner->renderer == renderer2 &&
                    animation_owner->controller == controller,
                "animation-only lookup recovers the selected second player's renderer and controller");
        require(!sfr::local_avatar_for_animation(memory, 0) &&
                    !sfr::local_avatar_for_animation(memory, animation + 4),
                "animation-only lookup rejects absent or unrelated buffers");
        require(!sfr::local_avatar_for_animation(memory, controller + 4, renderer2, animation) &&
                    !sfr::local_avatar_for_animation(memory, controller, renderer2, animation + 4) &&
                    !sfr::local_avatar_for_animation(memory, controller, cpu_renderer, animation),
                "animation identity cannot cross controllers, buffers, or CPU racers");
        memory.store<uint32_t>(rider2 + 108, 2);
        require(sfr::local_avatar_for_renderer(memory, renderer2).has_value(),
                "results autopilot does not change the selected local Avatar");
        memory.store<uint32_t>(manager + 40, list + 8);
        require(sfr::local_avatar_for_renderer(memory, renderer2).has_value(),
                "second local preview draws before the planned CPU entrants load");
        memory.store<uint32_t>(manager + 40, list + 4);
        require(!sfr::local_avatar_for_renderer(memory, renderer2), "uninitialized second slot is ignored");
        require(!sfr::local_avatar_for_animation(memory, animation),
                "animation-only lookup cannot claim an uninitialized local slot or a CPU entrant");
        memory.store<uint32_t>(rider + 100, 17);
        require(sfr::local_avatar_for_renderer(memory, renderer).has_value(),
                "first local preview draws while the second local is still loading");
        memory.store<uint32_t>(manager + 40, list + 8);
        require(sfr::local_avatar_for_renderer(memory, renderer).has_value() &&
                    sfr::local_avatar_for_renderer(memory, renderer2).has_value(),
                "renderer identity remains distinct even if both local entrants are Avatars");
        for (const uint32_t pointer : {0u, 0xFFFFFFF0u, 0x20000000u}) {
            memory.store<uint32_t>(rider2 + 3208, pointer);
            require(!sfr::local_avatar_for_renderer(memory, renderer2), "invalid character wrapper is rejected");
        }
        memory.store<uint32_t>(rider2 + 3208, character2);
        for (const uint32_t pointer : {0u, 0xFFFFFFF0u, 0x20000000u}) {
            memory.store<uint32_t>(character2 + 8, pointer);
            require(!sfr::local_avatar_for_renderer(memory, pointer), "invalid renderer is rejected");
        }
        memory.store<uint32_t>(character2 + 8, renderer2);
        memory.store<uint32_t>(character2 + 12, 0);
        require(!sfr::local_avatar_for_animation(memory, 0, renderer2, animation), "null controller is rejected");
        require(sfr::local_avatar_for_renderer(memory, renderer2).has_value(), "renderer permits a static pose fallback");
        memory.store<uint32_t>(character2 + 12, controller);
        memory.store<uint32_t>(character2 + 16, 0xFFFFFFF0u);
        require(!sfr::local_avatar_for_animation(memory, controller, renderer2, 0xFFFFFFF0u),
                "unmapped animation is rejected");
        memory.store<uint32_t>(character2 + 16, animation);
        for (const uint8_t count : {0, 3, 255}) {
            memory.store<uint8_t>(0x82B0569F, count);
            require(!sfr::local_avatar_for_renderer(memory, renderer2), "unsupported local counts are rejected");
        }
        memory.store<uint8_t>(0x82B0569F, 2);
        for (const uint32_t end : {list, list - 4, list + 3, list + 13 * 4, 0x20000000u}) {
            memory.store<uint32_t>(manager + 40, end);
            require(!sfr::local_avatar_for_renderer(memory, renderer2), "invalid shared racer vector is rejected");
        }
        memory.store<uint32_t>(manager + 40, list + 8);
        memory.store<uint32_t>(0x83E52F8C, 0);
        require(!sfr::local_avatar_for_renderer(memory, renderer2), "shared lookup stops outside the race");
        memory.store<uint32_t>(0x83E52F8C, 1);
        memory.store<uint32_t>(0x83E52FDC, 0);
        require(!sfr::local_avatar_for_renderer(memory, renderer2), "shared lookup does not reuse a destroyed manager");
        std::cout << "Avatar race state tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
