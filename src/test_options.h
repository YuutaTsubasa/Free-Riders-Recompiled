#pragma once
// The launcher's Test options page: diagnosis switches the game reads from its
// environment at start, which a bug report has asked (or may ask) a player to
// try. Players cannot easily set environment variables, and on Android and
// iOS not at all, so each is a choice on the page instead.
//
// One table lists them. Every option starts at Default, which exports
// nothing: the game then behaves as it does without the page. A choice is
// saved in settings.ini as test_option.<variable>=<value> and reaches the
// game through game_environment (launcher_settings.cpp) on every platform;
// the game writes the ones it was started with into game.log once, as a
// TEST_OPTIONS line, so a bug report shows them. Android's diagnostic ZIP
// keeps them too (DiagnosticsArchive.java's allowlist, checked by
// tests/test_test_options.py).
//
// Only switches that are safe to flip are listed: none takes a path, a mask
// or a number, and none breaks the game by design (SFR_TEST_NO_CLIP_DISTANCE=0,
// which crashes a Mali on purpose, is left out).
#include <span>
#include <string>
#include <string_view>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace sfr {
// Where an option means something; it is shown and exported only there.
namespace test_platform {
inline constexpr unsigned windows = 1, linux_desktop = 2, android = 4, macos = 8, ios = 16;
inline constexpr unsigned all = windows | linux_desktop | android | macos | ios;
#if defined(_WIN32)
inline constexpr unsigned current = windows;
#elif defined(__ANDROID__)
inline constexpr unsigned current = android;
#elif defined(__APPLE__) && TARGET_OS_IPHONE
inline constexpr unsigned current = ios;
#elif defined(__APPLE__)
inline constexpr unsigned current = macos;
#else
inline constexpr unsigned current = linux_desktop;
#endif
}

// The page's sections.
enum class TestGroup { graphics, processor, gameplay, logging };
inline constexpr const char* test_group_names[][2] = {
    {"Graphics", "圖形"},
    {"Processor", "處理器"},
    {"Gameplay", "遊戲"},
    {"Logs for a bug report", "回報問題用的紀錄"},
};

// A value other than Default: what the variable is set to, and its name on
// the page (English, Traditional Chinese).
struct TestChoice {
    const char* value;
    const char* label[2];
};

struct TestOption {
    const char* variable;  // the environment variable the game reads
    TestGroup group;
    unsigned platforms;    // test_platform bits
    const char* label[2];
    const char* hint[2];   // what it is for; says it is for testing
    std::span<const TestChoice> choices;  // besides Default, in the order shown
    const char* issue;     // where it came up, for the page and the PR ("" if nowhere yet)
};

namespace test_option_detail {
inline constexpr TestChoice on[] = {{"1", {"On", "開啟"}}};
inline constexpr TestChoice off[] = {{"0", {"Off", "關閉"}}};
inline constexpr TestChoice on_off[] = {{"1", {"On", "開啟"}}, {"0", {"Off", "關閉"}}};
inline constexpr TestChoice core_plan[] = {
    {"13", {"As in 0.10.8", "與 0.10.8 相同"}},
    {"0", {"Left to Android", "交給 Android"}},
};

inline constexpr TestOption table[] = {
    // ---- graphics
    {"SFR_TEST_DISPATCH_LOOP", TestGroup::graphics, test_platform::all,
     {"Rewrite shader dispatch loops", "改寫著色器分派迴圈"},
     {"For testing (Vulkan): runs the shaders' dispatch loops as plain steps, as Macs, iPhones, Samsung "
      "Xclipse and Intel GPUs do by default. Try On if races are very slow on an older GPU, or if preparing "
      "pipelines does not advance; Off keeps the loops.",
      "測試用（Vulkan）：把著色器的分派迴圈改為依序執行，如同 Mac、iPhone、Samsung Xclipse 與 Intel GPU 的預設做法。"
      "若較舊的 GPU 在比賽中非常慢，或準備管線一直沒有進度，可試著開啟；關閉則保留迴圈。"},
     on_off, "#134, #224, #239"},
    {"SFR_TEST_NO_DESCRIPTOR_INDEXING", TestGroup::graphics, test_platform::all,
     {"Small texture sets", "小型材質組"},
     {"For testing (Vulkan): each draw binds a small set of textures of its own instead of one large array, "
      "as on GPUs without descriptor indexing.",
      "測試用（Vulkan）：每次繪製各自綁定一小組材質，而不是共用一個大型陣列，如同不支援 descriptor indexing 的 GPU。"},
     on, "#134"},
    {"SFR_TEST_NO_CLIP_DISTANCE", TestGroup::graphics, test_platform::all,
     {"Shaders without clip distance", "著色器不使用 clip distance"},
     {"For testing (Vulkan): leaves the clip distance out of every shader, as on Mali GPUs whose drivers crash on it.",
      "測試用（Vulkan）：所有著色器都不寫入 clip distance，如同在遇到它就會當機的 Mali GPU 上。"},
     on, "#166, #169"},
    {"SFR_TEST_DEPTH_D24S8", TestGroup::graphics, test_platform::all,
     {"24-bit depth buffer", "24 位元深度緩衝"},
     {"For testing (Vulkan): uses the D24S8 depth format that older Mali drivers need instead of D32S8.",
      "測試用（Vulkan）：使用舊版 Mali 驅動程式需要的 D24S8 深度格式，而不是 D32S8。"},
     on, "#169"},
    {"SFR_TEST_NO_DEVICE_ADDRESS", TestGroup::graphics, test_platform::all,
     {"No buffer device addresses", "不使用 buffer device address"},
     {"For testing (Vulkan): creates the device without buffer device addresses, which some older drivers "
      "lack or get wrong.",
      "測試用（Vulkan）：建立裝置時不啟用 buffer device address；部分較舊的驅動程式沒有或無法正確支援。"},
     on, "#169"},
    {"SFR_TEST_NO_INVERTED_DEPTH", TestGroup::graphics, test_platform::windows,
     {"Flip depth in a shader", "以著色器翻轉深度"},
     {"For testing (D3D12): draws the title's inverted viewports by flipping depth in a shader, as on Windows "
      "without that D3D12 feature.",
      "測試用（D3D12）：標題的反向視埠改由著色器翻轉深度來繪製，如同在沒有該 D3D12 功能的 Windows 上。"},
     on, "#177"},
    {"SFR_RENDER_THREAD", TestGroup::graphics, test_platform::all,
     {"Render thread", "渲染執行緒"},
     {"For testing: Off records the draws on the game's own thread instead of a thread of their own. Slower; "
      "it tells whether a crash or a drawing problem comes from that thread.",
      "測試用：關閉時，繪製指令改在遊戲本身的執行緒上記錄，而不是使用獨立的執行緒。速度較慢，用來判斷當機或繪製問題是否來自該執行緒。"},
     off, ""},
    {"SFR_INDEX_CACHE", TestGroup::graphics, test_platform::all,
     {"Index cache", "索引快取"},
     {"For testing: Off scans every draw's vertex indices again instead of keeping what an earlier scan found.",
      "測試用：關閉時，每次繪製都重新掃描頂點索引，不沿用先前掃描的結果。"},
     off, "#65"},
    {"SFR_STREAM_CACHE", TestGroup::graphics, test_platform::all,
     {"Keep whole vertex streams", "保留整段頂點資料"},
     {"For testing: Off copies the vertices of a draw whose indices are spread over a large vertex buffer every "
      "frame, as 0.10.27 did, instead of drawing them from that whole buffer kept on the GPU.",
      "測試用：關閉時，索引分散在大型頂點緩衝區中的繪製，每一格都重新複製它用到的頂點（與 0.10.27 相同），"
      "而不是直接從保留在 GPU 上的整段緩衝區繪製。"},
     off, "#176"},
    {"SFR_CONSTANT_RUNS", TestGroup::graphics, test_platform::all,
     {"Upload only used constants", "只上傳用到的常數"},
     {"For testing: Off uploads all of a draw's shader constants instead of only those its shaders read.",
      "測試用：關閉時，每次繪製都上傳全部著色器常數，而不只是著色器會讀取的部分。"},
     off, "#176"},
    {"SFR_CONSTANT_UPLOAD_REUSE", TestGroup::graphics, test_platform::all,
     {"Reuse shader constants", "重複使用著色器常數"},
     {"For testing: On uploads a draw's shader constants only when they changed. A performance experiment.",
      "測試用：開啟時，著色器常數只在改變時才上傳。這是效能實驗。"},
     on, "#65"},
    // ---- processor
    {"SFR_CORE_PLAN", TestGroup::processor, test_platform::android,
     {"Core placement", "核心配置"},
     {"For testing: which of the phone's cores the game's busiest threads run on: as 0.10.8 placed them, or all "
      "left to Android.",
      "測試用：遊戲最忙碌的執行緒要在手機的哪些核心上執行：改回 0.10.8 的配置，或全部交給 Android 決定。"},
     core_plan, "#176"},
    {"SFR_PERF_HINT", TestGroup::processor, test_platform::android,
     {"Performance hints", "效能提示"},
     {"For testing (Android 13 or later): tells Android each frame how long the game worked against a "
      "sixtieth of a second, so the phone can raise its clocks sooner. For phones whose fast core stays at a "
      "low clock during a race.",
      "測試用（Android 13 以上）：每一格告訴 Android 遊戲花了多少時間（目標為六十分之一秒），讓手機更快提高時脈。"
      "適用於比賽中快核心一直停在低時脈的手機。"},
     on, "#176"},
    {"SFR_AUDIO_PRIORITY", TestGroup::processor, test_platform::android,
     {"Sound threads first", "音效執行緒優先"},
     {"For testing: the threads that make the game's sound run ahead of its busy threads (the default), so a "
      "hot phone does not crackle. Off leaves them at normal priority, as 0.10.30 did.",
      "測試用：讓產生遊戲聲音的執行緒優先於忙碌的執行緒（預設如此），手機發熱時聲音才不會劈啪作響。關閉時改回 0.10.30 的一般優先權。"},
     off, "#176"},
    {"SFR_MAIN_HELPS_JOBS", TestGroup::processor, test_platform::all,
     {"Main thread runs race jobs", "主執行緒執行比賽工作"},
     {"For testing: the game's main thread runs queued race jobs itself instead of only waiting for its helper "
      "threads (the default). Off waits, as 0.10.24 did, which is slower where the helpers run on slow cores.",
      "測試用：遊戲的主執行緒自己執行排入佇列的比賽工作，而不是只等輔助執行緒（預設如此）。關閉時改回 0.10.24 的等待方式；輔助執行緒在慢核心上時會比較慢。"},
     on_off, "#176"},
    {"SFR_MAIN_HELPS_COLLISION", TestGroup::processor, test_platform::all,
     {"Main thread runs collision jobs", "主執行緒執行碰撞工作"},
     {"For testing: while the game's main thread waits for the collision check before a race step, it runs "
      "that check's queued jobs itself (the default). Off only waits, as 0.10.26 did.",
      "測試用：遊戲的主執行緒在比賽步進前等待碰撞檢查時，自己執行該檢查排入佇列的工作（預設如此）。關閉時只等待，與 0.10.26 相同。"},
     on_off, "#176"},
    {"SFR_MAIN_AFFINITY", TestGroup::processor, test_platform::windows,
     {"Pin the main thread", "固定主執行緒"},
     {"For testing: keeps the game's main thread on one processor (the default with six or more). Off may help "
      "a laptop that slows down in heavy parts of a course.",
      "測試用：讓遊戲的主執行緒固定在同一個處理器上（有六個以上處理器時預設如此）。若筆電在賽道的繁重段落變慢，關閉也許有幫助。"},
     on_off, "#165"},
    {"SFR_HOST_TIMING", TestGroup::processor, test_platform::windows,
     {"Opt out of power throttling", "不受電源節流影響"},
     {"For testing: Off leaves the game to Windows' power throttling and its coarse timer, as before. For "
      "comparing frame pacing on a handheld.",
      "測試用：關閉時，遊戲會像以前一樣受 Windows 的電源節流與較粗的計時器影響。用來比較掌機上的畫格節奏。"},
     off, ""},
    // ---- gameplay
    {"SFR_STEP_WAITS_COLLISION", TestGroup::gameplay, test_platform::all,
     {"Race step waits for collision", "比賽步進等待碰撞檢查"},
     {"For testing: Off turns off the fix for racers flying out of the course at the Fly board.",
      "測試用：關閉後會停用「選手在飛行板（Fly）處飛出賽道」的修正。"},
     off, "#153"},
    {"SFR_UNDRAWN_STAGE_AREAS", TestGroup::gameplay, test_platform::all,
     {"Skipped frames: stage areas only", "跳過的畫格：賽道只找區域"},
     {"For testing: in a race frame that frame skipping does not show, the course's own task only finds which "
      "of its areas the camera is in and leaves its objects undrawn (the default). Off runs all of its drawing "
      "there, as 0.10.24 did, for a course object that stands still or a racer that falls through the course.",
      "測試用：在比賽中不顯示的畫格（跳格）裡，賽道本身的工作只找出鏡頭所在的區域，不繪製它的物件（預設如此）。"
      "關閉時改回 0.10.24 的做法，執行它全部的繪製；用於賽道物件不動或選手穿過賽道掉落時。"},
     off, "#176"},
    {"SFR_TOUCH_IN_SCENES", TestGroup::gameplay, test_platform::android | test_platform::ios,
     {"Touch controls in cutscenes", "過場畫面顯示觸控按鈕"},
     {"For testing: On keeps the touch controls on screen in movies and story scenes, as 0.10.30 did. By default "
      "they are hidden there and a tap anywhere is START, which skips.",
      "測試用：開啟時，影片與劇情畫面也顯示觸控按鈕（與 0.10.30 相同）。預設在這些畫面隱藏按鈕，點一下畫面任何地方即為 START，可以略過。"},
     on, "#274"},
    // ---- logging
    {"SFR_FRAME_METRICS", TestGroup::logging, test_platform::all,
     {"Frame timing", "畫格時間"},
     {"For testing: writes how long each part of a frame took into game.log. Costs a little speed.",
      "測試用：把每個畫格各部分所花的時間寫進 game.log。會稍微降低速度。"},
     on, "#134"},
    {"SFR_GPU_TIMING", TestGroup::logging, test_platform::all,
     {"GPU timing", "GPU 時間"},
     {"For testing: writes the GPU's time for the frames into game.log.",
      "測試用：把 GPU 繪製畫格所花的時間寫進 game.log。"},
     on, "#134"},
    {"SFR_AUDIO_CUES", TestGroup::logging, test_platform::all,
     {"Sound cue IDs", "音效 cue ID"},
     {"For testing: writes every sound and voice line the game asks for into game.log, to report a wrong one.",
      "測試用：把遊戲要求播放的每個音效與語音寫進 game.log，用來回報播錯的聲音。"},
     on, "#47"},
};
}

inline std::span<const TestOption> test_options() { return test_option_detail::table; }

inline bool test_option_applies(const TestOption& option, unsigned platform = test_platform::current) {
    return (option.platforms & platform) != 0;
}

inline const TestOption* find_test_option(std::string_view variable) {
    for (const auto& option : test_options())
        if (variable == option.variable) return &option;
    return nullptr;
}

// The choice with this value, or nullptr (Default, or not one of the option's).
inline const TestChoice* find_test_choice(const TestOption& option, std::string_view value) {
    for (const auto& choice : option.choices)
        if (value == choice.value) return &choice;
    return nullptr;
}

// settings.ini's key for an option: test_option.<variable>.
inline constexpr std::string_view test_option_key_prefix = "test_option.";

// The game's TEST_OPTIONS line for game.log: every listed variable it was
// started with, set and not empty, in the table's order; "none" without any.
// get is getenv, or a stand-in for a test.
template <class Get>
std::string describe_test_options(Get get) {
    std::string line = "TEST_OPTIONS";
    bool any = false;
    for (const auto& option : test_options()) {
        const char* value = get(option.variable);
        if (!value || !*value) continue;
        line += ' ';
        line += option.variable;
        line += '=';
        line += value;
        any = true;
    }
    if (!any) line += " none";
    return line;
}
}
