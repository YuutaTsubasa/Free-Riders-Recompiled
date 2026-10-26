#pragma once
#include "achievements.h"
#include "achievement_progress.h"
#include "achievement_toast.h"
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sfr {
// Issue #288: the achievement notification and the in-game achievements
// page, drawn by the port over the game (screen_overlay.h).
//
// The notification recreates the console's system notification of the
// time: a dark rounded bar at the bottom centre with the achievement's own
// icon (from the game's XDBF), "Achievement unlocked" and the gamerscore and
// name; it rises in, stays about four seconds and fades (achievement_toast.h).
//
// The page lists every achievement with its icon, name, description,
// gamerscore, when it was unlocked and, for those that need a number of
// things, how far the player is (achievement_progress.h, read from the
// game's own player record). It opens with both sticks clicked in (L3 +
// R3) on a controller, F1 on the keyboard, or by touch (tapping the
// notification, or a three-finger tap); B / Escape / F1 or the same click
// closes it. While it is open the game is told the system UI is showing
// (XN_SYS_UI, as the console's Guide did: in a race the game pauses itself)
// and reads no input.

// The page's navigation, apart from drawing (tested on its own).
struct AchievementPageNav {
    int count = 0;     // rows
    int visible = 5;   // rows on screen at once
    int selected = 0;
    int first = 0;     // first row shown
    void move(int by);  // clamps, and scrolls to keep the selection shown
    void page(int by) { move(by * visible); }
};

// The order the page lists them in: unlocked ones first, newest first, then
// the rest in the game's order. Indices into title.achievements.
std::vector<size_t> achievement_page_order(const TitleData& title, const UnlockedAchievements& unlocked);

// The page's texts in the game's language (Xbox language ids, 1 English ...
// 6 Italian; anything else is English).
struct AchievementTexts {
    const char* unlocked_toast;  // "Achievement unlocked"
    const char* title;           // "Achievements"
    const char* secret_name;
    const char* secret_text;
    const char* locked;
    const char* close;
    const char* select;
    const char* page;
};
const AchievementTexts& achievement_texts(uint32_t xbox_language);

class AchievementOverlay {
public:
    struct Host {
        // Buttons of every controller, read even while the game's input is held.
        std::function<uint16_t()> pad_buttons;
        // A key held on the keyboard (Windows virtual-key numbers), in focus.
        std::function<bool(int)> key_down;
        // Guest memory reads (false / empty if not readable).
        std::function<bool(uint32_t address, std::span<uint8_t> out)> read;
        // The page opened (true) or closed: tell the game, hold its input.
        std::function<void(bool)> page_shown;
    };
    static AchievementOverlay& instance();

    // Once at start: the game's title data, the player's file of unlocked
    // achievements, the language and whether to show notifications.
    void configure(std::vector<uint8_t> xdbf, std::filesystem::path unlocked_file, uint32_t xbox_language,
                   bool notifications, Host host);
    bool configured() const;
    // The game awarded these (any thread): those not unlocked before are
    // noted and shown. Returns them.
    std::vector<uint32_t> awarded(std::span<const uint32_t> ids, int64_t now);
    // Once a present, on the game's thread: input, the page, notifications.
    // `present` numbers the presents (for the test switches below).
    //   SFR_TEST_ACHIEVEMENT_AWARD=present:id,...  award as the game would
    //   SFR_TEST_ACHIEVEMENT_PAGE=open:close[:rows]  open the page, move
    //                                    the selection down, close it
    //   SFR_ACHIEVEMENT_TRACE=<file>     every 1200 presents: the progress
    //                                    on stderr, the player record's bytes
    //                                    written to the file
    void poll(double now, uint64_t present);
    // Touches in game-picture coordinates (0..1 over the picture), each
    // frame the presentation reads them.
    struct Touch { int64_t id; float x, y; };
    void touches(std::span<const Touch> touches, double now);
    bool page_open() const;
    void open_page(bool open);

    struct Impl;
private:
    AchievementOverlay();
    Impl* impl_;
};
}
