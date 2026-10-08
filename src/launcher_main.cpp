// The launcher: the player picks the window, sound and advanced settings
// (kept in settings.ini beside it), then it starts the game with them and
// waits. Closing the game closes the launcher; if the game stops on its
// own, the launcher comes back with the end of game.log.
// Windows draws with Direct3D 11; Linux and Android with SDL's renderer.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <imm.h>
#include <shellapi.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#else
#include <SDL.h>
#ifdef __ANDROID__
#include <unistd.h>
#endif

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#endif

#include "imgui_internal.h"
#include "installer.h"
#include "launcher_art.h"
#include "launcher_platform.h"
#include "camera_capture.h"
#include "kinect_sensor.h"
#include "kinect_preview.h"
#include "text_wrap.h"
#include "voice_commands.h"
#include "launcher_settings.h"
#include "mod_loader.h"
#include "game_language.h"
#include "input_bindings.h"
#include "pad_devices.h"
#include "launcher_sound.h"
#include "sony_gamepad.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
#endif

namespace {
namespace fs = std::filesystem;

// ---------------------------------------------------------------- text
enum Text {
    TitleLine, Subtitle,
    TabDisplay, TabSound, TabGeneral, TabMotion, TabFiles,
    WindowSize, DesktopSize, Fullscreen, FullscreenHint, VSync, VSyncHint, RenderResolution, RenderResolutionHint, NativeResolution,
    Sound, SoundHint, Volume,
    SkipMovies, SkipMoviesHint,
    VertexCache, VertexCacheHint, GpuPipeline, GpuPipelineHint, RaceEvery, RaceEveryHint,
    CameraLabel, CameraHint, CameraHintOff, CameraHintPicture, CameraHintKinect, CameraOff, CameraPicture, CameraMotion, CameraDevice, CameraDeviceHint, CameraNone,
    CameraTest, CameraTesting, CameraWorks, CameraSilent, CameraClosed, CameraMirror, CameraMirrorHint,
    CameraKinect, KinectRow, KinectRowHint, KinectMissing,
    KinectNoRuntime, KinectNoSensor, KinectFailed, KinectDownload, KinectDownloadV2, KinectInstallHint,
    VoiceLabel, VoiceHint,
    KinectPreviewOpen, KinectPreviewClose,
    CameraDebug, CameraDebugHint,
    ImageDirectory, ImageDirectoryHint, AssetDirectory, AssetDirectoryHint, Browse, Found, Missing, FilesHint,
    StartGame, Quit, Defaults,
    MissingFiles, MissingGame, LaunchFailed, Ready,
    Stopped, StoppedDetail, BackToSettings, OpenLog,
    InstallSubtitle, InstallTitle, InstallIntro, ChooseIso, ChooseFolder, InstallFromDisc,
    SourceDisc, SourceFolder, SourceReady, SourceUnreadable, SourceNotADisc, SourceWrongGame,
    SourceFiles, InstallDestination, FreeSpace, NotEnoughSpace, Install, Later,
    StageChecking, StageCopying, StageDecoding, StageFinishing, Remaining, Cancel,
    InstallFailed, InstallCancelledText, InstalledTitle, InstalledText, OpenSettings, NoShaderTools,
    GuideSelect, GuideBack, GuideQuit, GuidePlay, GuideCancel, GuideTabs,
    QuitTitle, QuitText, CancelInstallTitle, CancelInstallText, Yes, No,
    UiSoundsLabel, UiSoundsHint, GraphicsBackendLabel, GraphicsBackendHint,
    ShaderPack, ShaderPackHint, ChoosePack, InstallIntroFile, InstallFromImage, NoShaderPack,
    LanguageLabel, LanguageHint, LanguageSystem, LanguageEnglish, LanguageChinese, TouchLabel, TouchHint, TiltLabel, TiltHint,
    BarSettings, BarInstaller, BarInstalling, BarStopped,
    TabControls, PlayerOneDevice, PlayerOneDeviceHint, PlayerTwoDevice, PlayerTwoDeviceHint,
    DeviceBoth, DeviceGamepad, DeviceKeyboard, DeviceOff,
    BindingSet, BindingSetHint, SetOneKeyboard, SetOnePad, SetTwoKeyboard, SetTwoPad,
    PressKey, PressButton, Unbound, ResetBindings, BindingsHint, SticksFixed,
    PlayerOneGamepad, PlayerTwoGamepad, GamepadHint, GamepadAutomatic, GamepadMissing,
    AvatarModel, AvatarModelHint, AvatarModelNone, AvatarModelMissing, AvatarModelImportFailed, Clear,
    GameLanguageLabel, GameLanguageHint, VoiceLanguageLabel, VoiceLanguageHint, VoiceFollowsGame, TabAvatar,
    SectionLanguage, SectionPlayback, SectionReset, SectionWindow, SectionRendering, SectionPerformance,
    SectionGameAudio, SectionLauncherAudio, SectionPlayer1, SectionPlayer2, SectionBindings, SectionTouch, SectionVoice,
    SectionResolution, WindowResolutionHint, SectionDiagnostics, ExportDiagnostics, ExportDiagnosticsHint,
    TabMods, SectionMods, ModsHint, ModsEmpty, ModsExternal, ModsRefresh, ModsOpenFolder, MoveUp, MoveDown,
    TextCount
};

// English, Traditional Chinese.
constexpr std::array<std::array<const char*, 2>, TextCount> texts{{
    {"Free Riders Recompiled", "Free Riders Recompiled"},
    {"Settings take effect when the game starts.", "設定會在遊戲啟動時套用。"},
    {"Graphics", "畫面與效能"},
    {"Sound", "音效"},
    {"General", "一般"},
    {"Motion input", "體感與攝影機"},
    {"Game files", "遊戲檔案"},
    {"Window resolution", "視窗解析度"},
    {"desktop", "桌面大小"},
    {"Full screen", "全螢幕"},
    {"Borderless full screen. Alt+Enter switches while playing.", "無邊框全螢幕。遊戲中可按 Alt+Enter 切換。"},
    {"Vertical sync", "垂直同步"},
    {"Waits for the display between frames. The game never runs above 60 fps either way.",
     "每格畫面等待螢幕更新。無論是否開啟，遊戲都不會超過 60 fps。"},
    {"Rendering resolution", "遊戲渲染解析度"},
    {"Pixels actually drawn by the game, independent of the window size. Lower values reduce GPU load; higher values improve detail.",
     "遊戲實際繪製的像素數，與視窗大小分開設定。較低值可減輕 GPU 負擔，較高值可增加畫面細節。"},
    {"native", "原生"},
    {"Sound", "聲音"},
    {"Plays the game's sound on the default output device.", "在預設的輸出裝置播放遊戲聲音。"},
    {"Volume", "音量"},
    {"Skip movies", "略過影片"},
    {"Ends the opening and story movies after their first frames.", "開場與劇情影片只播放開頭便結束。"},
    {"Vertex cache", "頂點快取"},
    {"Keeps geometry that does not change on the GPU. Turn it off if models look out of date.",
     "將沒有變動的幾何資料保留在 GPU 上。若模型顯示不正確，可以關閉。"},
    {"Frame ahead", "提前一格"},
    {"Lets the graphics card work one frame behind the game. Turn it off if the picture flickers or looks wrong.",
     "讓顯示卡比遊戲晚一格工作。若畫面閃爍或顯示不正確，可以關閉。"},
    {"Draw every Nth race frame", "比賽中每 N 格繪製一次"},
    {"Above 1, a slower computer keeps the race at full speed with fewer frames drawn.",
     "大於 1 時，較慢的電腦也能維持比賽速度，但畫面較不流暢。"},
    {"Camera", "攝影機"},
    {"Motion tracks your body for Player 1. Controller input takes priority; after 1.5 seconds idle, fresh camera tracking resumes. Lost tracking falls back to the controller. Player 2 keeps its assigned controller.",
     "「體感」追蹤你的身體來控制 1P。操作手把時優先接手，停止操作 1.5 秒後恢復攝影機追蹤；失去追蹤時退回手把。2P 保留原本指定的手把。"},
    {"No camera: the game is played with controllers and the keyboard.",
     "不使用攝影機：用手把或鍵盤遊玩。"},
    {"Opens a webcam only for the pictures the game shows of you; you still play with controllers and the keyboard.",
     "開啟 webcam，只把影像交給遊戲顯示；操作仍然用手把或鍵盤。"},
    {"A real Kinect tracks the players' bodies, as on the console: its skeletons and its depth and colour cameras go to the game, which reads your moves itself. Controllers still work in the menus.",
     "由實體 Kinect 追蹤玩家的身體，跟主機上一樣：骨架、深度與彩色影像直接交給遊戲，由遊戲自己判斷動作。選單中仍可使用手把。"},
    {"Off", "關閉"},
    {"Picture", "畫面"},
    {"Motion", "體感"},
    {"Which camera", "使用哪一台"},
    {"The camera to read, when the host has more than one.", "當電腦上有多台攝影機時，要讀取哪一台。"},
    {"No camera found", "找不到攝影機"},
    {"Test", "測試"},
    {"Looking...", "確認中..."},
    {"Pictures are arriving", "有畫面"},
    {"Opened, but no picture arrives", "開得起來，但沒有畫面"},
    {"This camera will not open", "這台攝影機打不開"},
    {"Mirrored camera", "攝影機畫面左右相反"},
    {"Turn this on when the camera shows you as a mirror does. With it wrong, raising one hand moves the other, and the cursor runs off the side of the screen.",
     "若攝影機畫面像照鏡子一樣左右相反，請開啟此項。設定相反時，舉起一隻手會動到另一隻，游標也會跑到畫面邊緣。"},
    {"Kinect", "Kinect"},
    {"Kinect sensor", "Kinect 感測器"},
    {"A real Kinect tracks you as the console did: an Xbox 360 Kinect (Kinect for Windows SDK 1.8) or a Kinect v2 for Xbox One (SDK 2.0); the first one found is used. The preview shows whether it works, what it sees and the skeletons it tracks, and tilts it. It closes when the game starts.",
     "由實體 Kinect 像主機一樣追蹤你的身體：Xbox 360 版（需 Kinect for Windows SDK 1.8）或 Xbox One 版 Kinect v2（需 SDK 2.0），會使用第一台找到的。預覽會顯示它是否正常、拍到的畫面與追蹤到的骨架，也能調整角度；開始遊戲時會自動關閉。"},
    {"No Kinect found", "找不到 Kinect"},
    {"No Kinect SDK is installed", "尚未安裝 Kinect SDK"},
    {"No Kinect connected", "沒有連接 Kinect"},
    {"Kinect found, but it will not start", "找到 Kinect，但無法啟動"},
    {"SDK 1.8 (Xbox 360)", "SDK 1.8（Xbox 360 版）"},
    {"SDK 2.0 (Kinect v2)", "SDK 2.0（Kinect v2）"},
    {"An Xbox 360 Kinect needs the full Kinect for Windows SDK 1.8 (the Runtime alone refuses it); a Kinect v2 needs SDK 2.0 and a USB 3.0 port. Install the one for your sensor, plug it in through its adapter, then open the preview again. If it still will not start, close other programs using the Kinect.",
     "Xbox 360 版 Kinect 需要完整的 Kinect for Windows SDK 1.8（只裝 Runtime 會被拒絕）；Kinect v2 需要 SDK 2.0 與 USB 3.0 連接埠。請安裝對應的 SDK，用轉接器接上感測器後再開啟一次預覽。若仍無法啟動，請關閉其他正在使用 Kinect 的程式。"},
    {"Voice commands", "語音指令"},
    {"Say the Kinect's commands again: start, OK, back, next, pause... in English or Chinese (開始、確定、返回、下一步、暫停…). Uses Windows speech recognition and the default microphone: make the Kinect's microphone array the default recording device to use it.",
     "恢復 Kinect 的語音指令：start、OK、back、next、pause……英文或中文都可以（開始、確定、返回、下一步、暫停…）。使用 Windows 語音辨識與預設麥克風：要用 Kinect 的麥克風陣列，請把它設為預設錄音裝置。"},
    {"Open preview", "開啟預覽"},
    {"Close preview", "關閉預覽"},
    {"Skeleton debug window", "骨架 Debug 視窗"},
    {"Shows only the skeleton received by the game, in front and side views, with no camera image. Closing this window does not stop the game. Changes apply on the next game launch.",
     "僅顯示遊戲收到的骨架，提供正面與側面視圖，不顯示攝影機影像。關閉此視窗不會停止遊戲。設定會在下次啟動遊戲時套用。"},
    {"Game code image", "遊戲程式映像"},
    {"The game's decoded code and data (complete.txt, image.bin).", "解碼後的遊戲程式與資料（complete.txt、image.bin）。"},
    {"Game data", "遊戲資料"},
    {"The files of the game disc (default.xex and its folders).", "遊戲光碟中的檔案（default.xex 與各資料夾）。"},
    {"Browse...", "瀏覽..."},
    {"Found", "已找到"},
    {"Not found", "找不到"},
    {"Installing from a disc image or folder fills in both.", "從光碟映像檔或資料夾安裝時，會自動設定這兩項。"},
    {"Start game", "開始遊戲"},
    {"Quit", "離開"},
    {"Restore defaults", "恢復預設值"},
    {"Game files are missing. See Game files.", "缺少遊戲檔案，請到「遊戲檔案」設定。"},
    {"The game program (sfr_cpu_diagnostic) is not beside the launcher.", "啟動器旁找不到遊戲程式（sfr_cpu_diagnostic）。"},
    {"The game could not be started.", "無法啟動遊戲。"},
    {"Ready to play.", "準備就緒。"},
    {"The game stopped unexpectedly", "遊戲意外停止"},
    {"Exit code %u. The last lines of game.log:", "結束代碼 %u。以下是 game.log 的最後幾行："},
    {"Back to settings", "回到設定"},
    {"Open game.log", "開啟 game.log"},
    {"The game is installed once, from your own disc.", "遊玩前需要先從你自己的光碟安裝一次遊戲。"},
    {"Install the game", "安裝遊戲"},
    {"Choose your Sonic Free Riders disc image (.iso) or a folder with the disc's files. "
     "They are copied beside the launcher and the game's code is prepared from them.",
     "請選擇 Sonic Free Riders 的光碟映像檔（.iso），或含有光碟檔案的資料夾。"
     "檔案會複製到啟動器旁，並從中準備遊戲程式。"},
    {"Choose disc image...", "選擇光碟映像檔..."},
    {"Choose folder...", "選擇資料夾..."},
    {"Install from a disc image or folder...", "從光碟映像檔或資料夾安裝..."},
    {"Disc image", "光碟映像檔"},
    {"Folder", "資料夾"},
    {"Sonic Free Riders (USA / Europe) found.", "已找到 Sonic Free Riders（美版／歐版）。"},
    {"It cannot be read.", "無法讀取。"},
    {"It is not an Xbox 360 game disc (default.xex is missing).", "這不是 Xbox 360 遊戲光碟（找不到 default.xex）。"},
    {"It is another game or version. Only the USA / Europe disc is supported.", "這是其他遊戲或版本。目前只支援美版／歐版光碟。"},
    {"%zu files, %.2f GB", "%zu 個檔案，%.2f GB"},
    {"Installs to", "安裝位置"},
    {"%.1f GB free, %.1f GB needed", "可用 %.1f GB，需要 %.1f GB"},
    {"There is not enough free space on this drive.", "此磁碟的可用空間不足。"},
    {"Install", "安裝"},
    {"Later", "稍後"},
    {"Checking the source...", "正在檢查來源..."},
    {"Copying the game files...", "正在複製遊戲檔案..."},
    {"Preparing the game's code...", "正在準備遊戲程式..."},
    {"Finishing...", "即將完成..."},
    {"About %d:%02d left", "約剩 %d:%02d"},
    {"Cancel", "取消"},
    {"The installation failed:", "安裝失敗："},
    {"The installation was cancelled. Nothing was changed.", "已取消安裝，沒有變更任何檔案。"},
    {"Installation complete", "安裝完成"},
    {"Sonic Free Riders is ready. You can change the settings at any time before playing.",
     "Sonic Free Riders 已準備就緒。開始遊戲前隨時可以調整設定。"},
    {"Settings", "設定"},
    {"Neither the shader tools (out/tools/shader-translator) nor a shaders.pack were found: the game will draw without shaders.",
     "找不到著色器工具（out/tools/shader-translator）或 shaders.pack，遊戲將無法正確繪製。"},
    {"Select", "選擇"},
    {"Back", "返回"},
    {"Quit", "離開"},
    {"Play", "開始遊戲"},
    {"Cancel", "取消"},
    {"Category", "切換分類"},
    {"Quit the launcher?", "要離開啟動器嗎？"},
    {"Your settings are kept for next time.", "設定會保留到下次使用。"},
    {"Stop the installation?", "要停止安裝嗎？"},
    {"The files copied so far are removed; an earlier installation stays as it was.",
     "已複製的檔案會被移除，先前的安裝維持原樣。"},
    {"Yes", "是"},
    {"No", "否"},
    {"Launcher sounds", "啟動器音效"},
    {"The short sounds this launcher makes when you move and choose.", "在啟動器中移動與選擇時的提示音。"},
    {"Graphics backend", "繪圖後端"},
    {"Choose Vulkan (default) or D3D12. Changes apply the next time you start the game.",
     "選擇 Vulkan（預設）或 D3D12，下次啟動遊戲時生效。"},
    {"Shader pack", "著色器包"},
    {"shaders.pack: the shaders compiled on a computer (python scripts/pack_shaders.py). The game needs it where it cannot compile shaders itself, as on phones.",
     "shaders.pack：在電腦上編好的著色器（python scripts/pack_shaders.py）。在無法自行編譯著色器的地方（例如手機）需要它。"},
    {"Choose shaders.pack...", "選擇 shaders.pack..."},
    {"Choose your Sonic Free Riders disc image (.iso). Its files are copied into the app's storage and the game's code is prepared from them.",
     "請選擇 Sonic Free Riders 的光碟映像檔（.iso）。檔案會複製到 app 的儲存空間，並從中準備遊戲程式。"},
    {"Install from a disc image...", "從光碟映像檔安裝..."},
    {"shaders.pack is missing: the game will draw without shaders. Choose one under Game files.",
     "找不到 shaders.pack，遊戲將無法正確繪製。請在「遊戲檔案」選擇一個。"},
    {"Launcher language", "啟動器語言"},
    {"The launcher's language. Choose the game's language separately below.", "啟動器的語言；遊戲語言可在下方另外設定。"},
    {"System language", "系統語言"},
    {"English", "English"},
    {"繁體中文", "繁體中文"},
    {"Touch controls", "觸控按鈕"},
    {"Buttons over the game for playing without a controller.", "在遊戲畫面上顯示按鈕，沒有手把也能遊玩。"},
    {"Tilt to steer", "傾斜轉彎"},
    {"In a race, turn the phone like a steering wheel.", "比賽中把手機當方向盤左右傾斜來轉彎。"},
    {"SETTINGS", "設定"},
    {"INSTALLER", "安裝"},
    {"INSTALLING", "安裝中"},
    {"STOPPED", "已停止"},
    {"Controls", "控制器與按鍵"},
    {"Player 1", "1P"},
    {"What the first player uses. The keyboard has always been there behind the controller; leaving it there costs nothing.",
     "第一位玩家用什麼操作。鍵盤一直都在手把後面備著，留著它沒有壞處。"},
    {"Player 2", "2P"},
    {"Configure the second input slot. This release does not add playable two-player modes or split screen.",
     "設定第二組輸入。本次發行不新增可遊玩的雙人模式或分割畫面。"},
    {"Controller and keyboard", "手把與鍵盤"},
    {"Controller", "手把"},
    {"Keyboard", "鍵盤"},
    {"Nobody", "沒有"},
    {"Buttons for", "設定按鍵"},
    {"Which set of buttons the list below changes.", "下面的列表要改哪一組按鍵。"},
    {"Player 1 keyboard", "1P 鍵盤"},
    {"Player 1 controller", "1P 手把"},
    {"Player 2 keyboard", "2P 鍵盤"},
    {"Player 2 controller", "2P 手把"},
    {"Press a key...", "請按一個鍵..."},
    {"Press a button...", "請按一個按鈕..."},
    {"none", "無"},
    {"Reset these", "還原這組"},
    {"Choose a button to change it, then press what you want it to be. The two players share one keyboard, so their keys must differ.",
     "點一個按鍵再按下你想用的鍵就會換掉。兩位玩家共用一個鍵盤，所以按鍵不能重複。"},
    {"The sticks stay the sticks: a controller's sticks are positions, not presses.",
     "類比搖桿維持原樣：搖桿讀的是位置，不是按下與否。"},
    {"Player 1 controller", "1P 用哪一支手把"},
    {"Player 2 controller", "2P 用哪一支手把"},
    {"Which controller this player holds. While it is plugged in it is theirs, wherever the computer lists it; unplugged, they wait for it rather than taking somebody else's.",
     "這位玩家拿哪一支手把。只要它插著就是這位玩家的，不管電腦把它排在第幾個；拔掉的話這位玩家就等它，不會去搶別人的。"},
    {"Whichever is first", "自動（依順序）"},
    {"not plugged in", "目前沒插著"},
    {"Avatar model", "Avatar 模型"},
    {"Choose a VRM or GLB, then select AVATAR in either player's character menu. Changes apply on the next game launch. Unsupported models are skipped; see game.log for details.",
     "選擇 VRM 或 GLB，並由任一玩家在角色選單選 AVATAR 即可使用。設定於下次啟動遊戲時套用。不支援的模型會略過，詳細資訊請查看 game.log。"},
    {"No custom model", "未選擇自訂模型"},
    {"Model file not found. Choose another file or clear this selection.", "找不到模型檔案，請選擇其他檔案或清除目前選擇。"},
    {"Could not import the model. Your previous selection is unchanged.", "無法匯入模型，已保留原本的選擇。"},
    {"Clear", "清除"},
    {"Game language", "遊戲語言"},
    {"Choose the game's language independently of the launcher. System language follows your OS; unsupported languages use English. Applies when the game starts.",
     "獨立選擇遊戲語言。「系統語言」依照作業系統設定，不支援時使用英文。於下次啟動遊戲時套用。"},
    {"Voice language", "語音語言"},
    {"The recorded voices, apart from the text: for example Japanese text (or a translation mod that replaces it) with English voices. The disc has English and Japanese voices; the few lines only one of them has stay silent in the other. Applies when the game starts.",
     "與文字分開選擇角色語音：例如日文文字（或取代日文的翻譯 MOD）配英文語音。光碟只有英文與日文語音，少數只有其中一種才有的台詞，在另一種語言下不會發聲。於下次啟動遊戲時套用。"},
    {"Same as the game", "跟隨遊戲語言"},
    {"Avatar models", "Avatar 模型"},
    {"Languages", "語言"},
    {"Movies", "影片"},
    {"Restore defaults", "還原設定"},
    {"Window", "視窗"},
    {"Rendering", "渲染畫質"},
    {"Advanced performance options", "進階效能選項"},
    {"Game audio", "遊戲音效"},
    {"Launcher audio", "啟動器提示音"},
    {"Player 1", "玩家 1"},
    {"Player 2", "玩家 2"},
    {"Button and key bindings", "按鍵綁定"},
    {"Touch and tilt", "觸控與傾斜"},
    {"Voice commands", "語音指令"},
    {"Resolution", "解析度"},
    {"Output size in windowed mode. Fullscreen uses the display size; Rendering resolution controls image detail.",
     "視窗模式下的輸出大小；全螢幕使用螢幕尺寸。畫面精細度由下方的遊戲渲染解析度決定。"},
    {"Diagnostics", "診斷紀錄"},
    {"Export diagnostic ZIP", "匯出診斷 ZIP"},
    {"After playing, save the latest game log, performance settings and device information. Export before starting another game. Logs may contain local file paths.",
     "遊玩後儲存最近一次的遊戲紀錄、效能設定與裝置資訊。請在下次啟動遊戲前匯出；紀錄可能包含本機檔案路徑。"},
    {"Mods", "模組"},
    {"Mods", "模組"},
    {"Put each mod in its own folder in the mods folder below, with a mod.ini (HedgeModManager's format). Files in a mod replace the game's files at the same paths. Mods higher in the list win.",
     "把每個模組放在下方 mods 資料夾裡各自的資料夾，並附上 mod.ini（HedgeModManager 的格式）。模組裡的檔案會取代遊戲中相同路徑的檔案；清單中越上面的模組優先。"},
    {"No mods found yet.", "目前還沒有模組。"},
    {"HedgeModManager (or another tool) manages the mods: its cpkredir.ini is beside the launcher. Use that tool, or delete cpkredir.ini to manage them here.",
     "模組由 HedgeModManager（或其他工具）管理：啟動器旁有它的 cpkredir.ini。請用該工具，或刪除 cpkredir.ini 改在這裡管理。"},
    {"Refresh", "重新整理"},
    {"Open mods folder", "開啟 mods 資料夾"},
    {"Up", "上移"},
    {"Down", "下移"},
}};

int language = 0;
const char* tr(Text text) { return texts[text][language]; }

// A paragraph broken into lines that fit width (pixels, from the cursor)
// the way Chinese is set: ImGui on its own breaks only at spaces, which
// sends a Chinese sentence after a Latin word onto a line of its own
// (text_wrap.h).
std::string wrapped(const char* text, float width) {
    return sfr::wrap_text(text, (std::max)(width, 1.0f),
                          [](const char* first, const char* last) { return ImGui::CalcTextSize(first, last).x; });
}
// The room left before a wrap position given as PushTextWrapPos takes it.
float room_to(float wrap_x) { return wrap_x - ImGui::GetCursorPosX(); }
float room() { return ImGui::GetContentRegionAvail().x; }

// Whether a loaded font has Chinese glyphs (a Linux desktop may have none).
bool chinese_available = true;

// The launcher's language setting: "auto" follows the system; Chinese only
// with a font for it.
void apply_language(const std::string& setting) {
    language = setting == "en" ? 0 : setting == "zh-TW" ? 1 : sfr::launcher::ui_language();
    if (!chinese_available) language = 0;
}
constexpr std::array<std::pair<const char*, Text>, 3> languages{{
    {"auto", LanguageSystem}, {"en", LanguageEnglish}, {"zh-TW", LanguageChinese}}};

// A disc image or folder dropped on the window (or the launcher started
// with one) becomes the install source.
std::optional<std::filesystem::path> dropped;
// The window's close button, answered by the launcher between frames.
bool close_clicked = false;

#ifdef _WIN32
// ---------------------------------------------------------------- D3D11
ID3D11Device* device = nullptr;
ID3D11DeviceContext* context = nullptr;
IDXGISwapChain* swap_chain = nullptr;
ID3D11RenderTargetView* target = nullptr;

bool create_device(HWND window) {
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level{};
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                                   D3D11_SDK_VERSION, &desc, &swap_chain, &device, &level, &context);
    if (FAILED(result))
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &desc, &swap_chain, &device, &level, &context);
    if (FAILED(result)) return false;
    ID3D11Texture2D* back = nullptr;
    swap_chain->GetBuffer(0, IID_PPV_ARGS(&back));
    device->CreateRenderTargetView(back, nullptr, &target);
    back->Release();
    return true;
}

// The back buffer as a 32-bit BMP (SFR_LAUNCHER_SHOTS, a test aid).
void save_back_buffer(const std::string& file) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging))) {
        context->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            const uint32_t width = desc.Width, height = desc.Height, row = width * 4;
            BITMAPFILEHEADER head{};
            BITMAPINFOHEADER info{};
            info.biSize = sizeof(info);
            info.biWidth = LONG(width);
            info.biHeight = -LONG(height);  // top row first
            info.biPlanes = 1;
            info.biBitCount = 32;
            head.bfType = 0x4D42;
            head.bfOffBits = sizeof(head) + sizeof(info);
            head.bfSize = head.bfOffBits + row * height;
            if (FILE* out = std::fopen(file.c_str(), "wb")) {
                std::fwrite(&head, sizeof(head), 1, out);
                std::fwrite(&info, sizeof(info), 1, out);
                std::vector<uint8_t> line(row);
                for (uint32_t y = 0; y < height; ++y) {
                    const auto* source = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
                    for (uint32_t x = 0; x < width; ++x) {  // RGBA to BGRA
                        line[x * 4] = source[x * 4 + 2];
                        line[x * 4 + 1] = source[x * 4 + 1];
                        line[x * 4 + 2] = source[x * 4];
                        line[x * 4 + 3] = 255;
                    }
                    std::fwrite(line.data(), 1, row, out);
                }
                std::fclose(out);
            }
            context->Unmap(staging, 0);
        }
        staging->Release();
    }
    back->Release();
}

void destroy_device() {
    if (target) target->Release();
    if (swap_chain) swap_chain->Release();
    if (context) context->Release();
    if (device) device->Release();
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return 1;
    if (message == WM_DROPFILES) {
        const auto drop = reinterpret_cast<HDROP>(wparam);
        std::wstring name(DragQueryFileW(drop, 0, nullptr, 0) + 1, L'\0');
        if (DragQueryFileW(drop, 0, name.data(), UINT(name.size()))) {
            name.resize(name.size() - 1);
            dropped = std::filesystem::path(name);
        }
        DragFinish(drop);
        return 0;
    }
    if (message == WM_SYSCOMMAND && (wparam & 0xFFF0) == SC_KEYMENU) return 0;  // Alt alone opens no menu
    if (message == WM_CLOSE) { close_clicked = true; return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}

// The icon, drawn by render_icon, as a Windows icon of the given size.
HICON make_icon(int size) {
    const auto pixels = sfr::render_icon(size);
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size;  // top row first
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP colour = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!colour) return nullptr;
    std::memcpy(bits, pixels.data(), pixels.size() * sizeof(uint32_t));
    const std::vector<uint8_t> empty_mask(size_t((size + 15) / 16 * 2) * size_t(size));
    HBITMAP mask = CreateBitmap(size, size, 1, 1, empty_mask.data());
    ICONINFO info{TRUE, 0, 0, mask, colour};
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(colour);
    DeleteObject(mask);
    return icon;
}

#endif

// ---------------------------------------------------------------- files
std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

// The last lines of the log (it ends with the reason the game stopped).
std::vector<std::string> log_tail(const fs::path& log, size_t count) {
    std::ifstream in(log, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const std::streamoff size = in.tellg();
    const std::streamoff start = (std::max)(std::streamoff(0), size - 16384);
    in.seekg(start);
    std::string text(size_t(size - start), '\0');
    in.read(text.data(), std::streamsize(text.size()));
    std::vector<std::string> lines;
    size_t begin = 0;
    while (begin < text.size()) {
        size_t end = text.find('\n', begin);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 220) line = line.substr(0, 220) + "...";
        if (!line.empty()) lines.push_back(std::move(line));
        begin = end + 1;
    }
    if (start > 0 && !lines.empty()) lines.erase(lines.begin());  // cut mid-line
    if (lines.size() > count) lines.erase(lines.begin(), lines.end() - std::ptrdiff_t(count));
    return lines;
}

// ---------------------------------------------------------------- look
struct Fonts { ImFont* body = nullptr; ImFont* title = nullptr; ImFont* heading = nullptr; ImFont* tiny = nullptr; };

Fonts load_fonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    // Only the glyphs the launcher shows (and ASCII for paths), so the
    // Chinese font does not fill a huge atlas.
    static ImVector<ImWchar> ranges;
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    for (const auto& text : texts) { builder.AddText(text[0]); builder.AddText(text[1]); }
    builder.AddText("%0123456789x\xE2\x97\x8F\xE2\x97\x8B");  // and the status dots
    builder.BuildRanges(&ranges);

    const sfr::launcher::FontFiles files = sfr::launcher::font_files();
    const auto first = [](const std::vector<fs::path>& candidates) -> fs::path {
        std::error_code error;
        for (const auto& path : candidates)
            if (fs::is_regular_file(path, error)) return path;
        return {};
    };
    const auto load = [&](const std::vector<fs::path>& latin, const std::vector<fs::path>& chinese, float size) -> ImFont* {
        ImFont* font = nullptr;
        if (const fs::path path = first(latin); !path.empty())
            font = io.Fonts->AddFontFromFileTTF(utf8(path).c_str(), size * scale, nullptr, ranges.Data);
        if (!font) {
            ImFontConfig config;
            config.SizePixels = size * scale;
            font = io.Fonts->AddFontDefault(&config);
        }
        if (const fs::path path = first(chinese); !path.empty()) {
            ImFontConfig merge;
            merge.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(utf8(path).c_str(), size * scale, &merge, ranges.Data);
        }
        return font;
    };
    Fonts fonts;
    fonts.body = load(files.body, files.chinese, 19.0f);
    fonts.heading = load(files.heading, files.chinese_bold, 23.0f);
    fonts.title = load(files.title, {}, 36.0f);
    fonts.tiny = load(files.heading, files.chinese_bold, 15.0f);
#ifdef __ANDROID__
    // The characters beyond Latin (Chinese, the status dots) come from
    // Android's own text renderer (launcher_platform.h).
    std::vector<unsigned int> wide;
    for (const auto& text : texts)
        for (const char* at : {text[0], text[1]})
            while (*at) {
                unsigned int codepoint = 0;
                at += ImTextCharFromUtf8(&codepoint, at, nullptr);
                if (codepoint >= 0x2000 && std::find(wide.begin(), wide.end(), codepoint) == wide.end()) wide.push_back(codepoint);
            }
    for (unsigned int codepoint : {0x25CFu, 0x25CBu})
        if (std::find(wide.begin(), wide.end(), codepoint) == wide.end()) wide.push_back(codepoint);
    sfr::launcher::add_system_glyphs(fonts.body, 19.0f * scale, false, wide);
    sfr::launcher::add_system_glyphs(fonts.heading, 23.0f * scale, true, wide);
    sfr::launcher::add_system_glyphs(fonts.tiny, 15.0f * scale, true, wide);
    sfr::launcher::draw_system_glyphs();
#endif
    // Built now so the Chinese glyphs can be checked for.
    if (!io.Fonts->IsBuilt()) io.Fonts->Build();
    chinese_available = fonts.body && fonts.body->FindGlyphNoFallback(0x4E2D) != nullptr;  // 中
    return fonts;
}

constexpr ImVec4 accent{0.16f, 0.52f, 1.00f, 1.0f};
constexpr ImVec4 accent_bright{0.40f, 0.72f, 1.00f, 1.0f};
constexpr ImVec4 cyan{0.35f, 0.92f, 1.00f, 1.0f};
constexpr ImVec4 dim_text{0.66f, 0.72f, 0.84f, 1.0f};
constexpr ImVec4 warning_text{1.0f, 0.55f, 0.45f, 1.0f};

void apply_style(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 0;
    style.ChildRounding = 14;
    style.FrameRounding = 9;
    style.PopupRounding = 12;
    style.GrabRounding = 9;
    style.ScrollbarRounding = 9;
    style.FramePadding = ImVec2(14, 8);
    style.ItemSpacing = ImVec2(12, 10);
    style.WindowPadding = ImVec2(0, 0);
    style.ScrollbarSize = 12;
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 1;
    style.PopupBorderSize = 1;
    auto* c = style.Colors;
    c[ImGuiCol_Text] = ImVec4(0.95f, 0.97f, 1.0f, 1.0f);
    c[ImGuiCol_TextDisabled] = dim_text;
    c[ImGuiCol_ChildBg] = ImVec4(0.04f, 0.07f, 0.16f, 0.72f);
    c[ImGuiCol_PopupBg] = ImVec4(0.05f, 0.08f, 0.18f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.45f, 0.62f, 1.0f, 0.18f);
    c[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.17f, 0.32f, 0.85f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.17f, 0.25f, 0.46f, 0.95f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.30f, 0.55f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.14f, 0.20f, 0.38f, 0.9f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.30f, 0.56f, 1.0f);
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = ImVec4(0.16f, 0.52f, 1.0f, 0.30f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.16f, 0.52f, 1.0f, 0.42f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.16f, 0.52f, 1.0f, 0.55f);
    c[ImGuiCol_SliderGrab] = accent_bright;
    c[ImGuiCol_SliderGrabActive] = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_CheckMark] = accent_bright;
    // Focus is shown by our own glow and row highlights.
    c[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.02f, 0.08f, 0.62f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    style.ScaleAllSizes(scale);
}

float approach(float value, float target, float rate) {
    return value + (target - value) * (std::min)(1.0f, ImGui::GetIO().DeltaTime * rate);
}

float ease_out(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

ImU32 color(ImVec4 c, float alpha) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

// Our own backdrop: a deep blue gradient, light streaks racing past, and
// wide rings turning slowly on the right.
void draw_backdrop(ImDrawList* draw, ImVec2 size, float time, float scale) {
    draw->AddRectFilledMultiColor(ImVec2(0, 0), size, IM_COL32(9, 22, 64, 255), IM_COL32(28, 14, 70, 255),
                                  IM_COL32(4, 8, 22, 255), IM_COL32(3, 12, 34, 255));
    for (int i = 0; i < 3; ++i) {
        const float radius = (260.0f + 120.0f * i) * scale;
        const ImVec2 centre(size.x * 0.86f + std::sin(time * 0.15f + i) * 18 * scale,
                            size.y * 0.42f + std::cos(time * 0.12f + i * 2) * 14 * scale);
        draw->AddCircle(centre, radius, IM_COL32(90, 170, 255, 16 - i * 4), 96, (10.0f - i * 3) * scale);
        // A brighter arc sweeping round each ring.
        const float start = time * (0.35f - 0.1f * i) + i * 2.1f;
        draw->PathArcTo(centre, radius, start, start + 0.9f, 32);
        draw->PathStroke(IM_COL32(120, 210, 255, 34 - i * 8), 0, (3.0f - i * 0.7f) * scale);
    }
    // A wide band across the lower half, tilted like a track.
    const float band_y = size.y * 0.70f;
    draw->AddQuadFilled(ImVec2(0, band_y + 70 * scale), ImVec2(size.x, band_y - 150 * scale),
                        ImVec2(size.x, band_y - 60 * scale), ImVec2(0, band_y + 160 * scale), IM_COL32(40, 110, 255, 26));
    draw->AddQuadFilled(ImVec2(0, band_y + 172 * scale), ImVec2(size.x, band_y - 48 * scale),
                        ImVec2(size.x, band_y - 42 * scale), ImVec2(0, band_y + 178 * scale), IM_COL32(120, 190, 255, 60));
    for (int i = 0; i < 48; ++i) {
        // Fixed pseudo-random lane, length and speed per streak.
        const uint32_t seed = uint32_t(i) * 2654435761u;
        const float lane = float(seed % 1000) / 1000.0f;
        const float length = (80.0f + float((seed >> 10) % 260)) * scale;
        const float speed = (260.0f + float((seed >> 20) % 900)) * scale;
        const float span = size.x + length * 2;
        const float x = size.x + length - std::fmod(time * speed + float(seed % 7919), span);
        const float y = lane * size.y;
        const float thick = (1.0f + float((seed >> 6) % 3)) * scale;
        const int alpha = 10 + int((seed >> 14) % 38);
        const float slant = -length * 0.18f;
        draw->AddQuadFilled(ImVec2(x, y), ImVec2(x + length, y + slant), ImVec2(x + length, y + slant + thick),
                            ImVec2(x, y + thick), IM_COL32(170, 210, 255, alpha));
    }
}

// A glow that breathes around a focused control.
void focus_glow(ImVec2 min, ImVec2 max, float rounding, float strength, float scale) {
    if (strength <= 0.01f) return;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float pulse = 0.75f + 0.25f * std::sin(float(ImGui::GetTime()) * 5.0f);
    for (int ring = 3; ring >= 1; --ring) {
        const float grow = ring * 2.5f * scale;
        draw->AddRect(ImVec2(min.x - grow, min.y - grow), ImVec2(max.x + grow, max.y + grow),
                      color(cyan, strength * pulse * (0.28f - ring * 0.07f)), rounding + grow, 0, 2.0f * scale);
    }
    draw->AddRect(min, max, color(cyan, strength * pulse), rounding, 0, 2.0f * scale);
}

// Puts the keyboard and pad focus on the next item, with the cursor shown
// (Enter or A then acts at once) and no remembered column from earlier
// moves, so Up and Down go to what is straight above or below.
void focus_next() {
    ImGui::SetKeyboardFocusHere();
    ImGui::SetNavCursorVisible(true);
    ImGuiWindow* window = ImGui::GetCurrentWindow()->RootWindowForNav;
    window->NavPreferredScoringPosRel[0] = window->NavPreferredScoringPosRel[1] = ImVec2(FLT_MAX, FLT_MAX);
}

bool nav_visible() { return GImGui->NavCursorVisible; }

bool item_disabled() { return (GImGui->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0; }

// How strongly the last item shows focus, eased per item.
float focus_amount(bool on) {
    float& amount = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID() ^ 0x5A17u, 0.0f);
    amount = approach(amount, on ? 1.0f : 0.0f, 14.0f);
    return amount;
}

sfr::UiSounds* sounds = nullptr;
// The width of the settings panel's content, which lives in the main
// window (so keyboard and pad navigation reach it) rather than a child.
float content_width = 600.0f;
// Set when LB/RB changes the settings category: the new category's first
// control takes the focus.
bool focus_first_control = false;
void play(sfr::UiSound sound) { if (sounds) sounds->play(sound); }

// An on/off switch that slides; keyboard and pad focus it like a button.
bool toggle(const char* id, bool* value) {
    const float height = ImGui::GetFrameHeight(), width = height * 1.9f;
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    if (pressed) {
        *value = !*value;
        play(*value ? sfr::UiSound::toggle_on : sfr::UiSound::toggle_off);
    }
    float& knob = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), *value ? 1.0f : 0.0f);
    knob = approach(knob, *value ? 1.0f : 0.0f, 16.0f);
    const ImVec4 off = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
    const ImVec4 track(off.x + (accent.x - off.x) * knob, off.y + (accent.y - off.y) * knob,
                       off.z + (accent.z - off.z) * knob, 1.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(position, ImVec2(position.x + width, position.y + height), ImGui::GetColorU32(track), height * 0.5f);
    if (ImGui::IsItemHovered())
        draw->AddRect(position, ImVec2(position.x + width, position.y + height), IM_COL32(255, 255, 255, 70), height * 0.5f);
    const float radius = height * 0.5f - 3.0f * ImGui::GetStyle().FramePadding.y / 8.0f;
    const ImVec2 knob_at(position.x + height * 0.5f + (width - height) * knob, position.y + height * 0.5f);
    draw->AddCircleFilled(ImVec2(knob_at.x, knob_at.y + 1.5f), radius, IM_COL32(0, 0, 0, 60));
    draw->AddCircleFilled(knob_at, radius, IM_COL32(255, 255, 255, 255));
    return pressed;
}

// A focused slider moves with left and right (pad or keyboard), a step at
// a time, without first being activated with A as ImGui otherwise asks.
bool slider_steps(int* value, int low, int high, int step) {
    if (!ImGui::IsItemFocused() || ImGui::IsItemActive()) return false;
    int delta = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, true) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) delta -= step;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) delta += step;
    const int next = std::clamp(*value + delta, low, high);
    if (next == *value) return false;
    *value = next;
    return true;
}

// One setting: its name on the left, the control on the right, an
// explanation under it. The row lights up while its control has focus.
template<class Control>
void setting_row(const char* label, const char* hint, float control_width, float scale, Control control) {
    ImGui::PushID(label);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);
    const float width = content_width;
    const ImVec2 row_min = ImGui::GetCursorScreenPos();
    const float start_y = ImGui::GetCursorPosY();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(width - control_width);
    ImGui::SetCursorPosY(start_y);
    if (focus_first_control) {
        focus_next();
        focus_first_control = false;
    }
    control();
    const bool focused = ImGui::IsItemFocused() && nav_visible();
    const float lit = focus_amount(focused);
    if (hint) {
        ImGui::PushStyleColor(ImGuiCol_Text, dim_text);
        const float wrap_x = ImGui::GetCursorPosX() + width - control_width - ImGui::GetStyle().ItemSpacing.x * 2;
        ImGui::PushTextWrapPos(wrap_x);
        ImGui::TextUnformatted(wrapped(hint, room_to(wrap_x)).c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    const ImVec2 row_max(row_min.x + width, ImGui::GetCursorScreenPos().y);
    if (lit > 0.01f) {
        draw->ChannelsSetCurrent(0);
        const float pad = 10 * scale;
        const ImVec2 a(row_min.x - pad, row_min.y - pad * 0.6f), b(row_max.x + pad, row_max.y - pad * 0.2f);
        draw->AddRectFilledMultiColor(a, b, color(accent, 0.30f * lit), color(accent, 0.06f * lit),
                                      color(accent, 0.06f * lit), color(accent, 0.30f * lit));
        draw->AddRectFilled(a, ImVec2(a.x + 4 * scale, b.y), color(cyan, lit), 2 * scale);
    }
    draw->ChannelsMerge();
    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y));
    ImGui::PopID();
}

bool big_button(const char* label, ImVec2 size, bool primary, float scale) {
    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent_bright);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.55f, 0.80f, 1.0f, 1.0f));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, size.y * 0.5f);
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    if (primary) ImGui::PopStyleColor(3);
    const float lit = focus_amount(ImGui::IsItemFocused() && nav_visible());
    focus_glow(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), size.y * 0.5f, lit, scale);
    if (primary) {
        // A light sliding across the primary button now and then.
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const float cycle = std::fmod(float(ImGui::GetTime()), 3.2f) / 0.9f;
        if (cycle < 1.0f && !item_disabled()) {
            const float x = a.x + (b.x - a.x + 80 * scale) * cycle - 40 * scale;
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->PushClipRect(a, b, true);
            draw->AddQuadFilled(ImVec2(x, a.y), ImVec2(x + 26 * scale, a.y), ImVec2(x + 6 * scale, b.y), ImVec2(x - 20 * scale, b.y),
                                IM_COL32(255, 255, 255, 50));
            draw->PopClipRect();
        }
    }
    if (pressed) play(sfr::UiSound::confirm);
    return pressed;
}

// A smaller button with the same focus glow.
bool small_button(const char* label, float scale) {
    const bool pressed = ImGui::Button(label);
    focus_glow(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetStyle().FrameRounding,
               focus_amount(ImGui::IsItemFocused() && nav_visible()), scale);
    if (pressed) play(sfr::UiSound::confirm);
    return pressed;
}

// ---------------------------------------------------------------- pad
bool sony_in_use = false;

void feed_pad(const sfr::GamepadState* pad);

// SFR_LAUNCHER_NAV_TRACE=1: which item has the keyboard and pad focus, its
// window and screen rectangle, and whether the cursor shows, on stderr each
// time one changes (with SFR_LAUNCHER_INPUT_SCRIPT, to test navigation).
void trace_navigation() {
    static const bool trace = std::getenv("SFR_LAUNCHER_NAV_TRACE") != nullptr;
    if (!trace) return;
    static ImGuiID last = 0;
    static bool last_visible = false;
    ImGuiContext& g = *GImGui;
    if (g.NavId == last && g.NavCursorVisible == last_visible) return;
    last = g.NavId;
    last_visible = g.NavCursorVisible;
    ImRect r;
    if (g.NavWindow) {
        const ImVec2 o(g.NavWindow->Pos.x - g.NavWindow->Scroll.x, g.NavWindow->Pos.y - g.NavWindow->Scroll.y);
        r = ImRect(g.NavWindow->NavRectRel[0].Min.x + o.x, g.NavWindow->NavRectRel[0].Min.y + o.y,
                   g.NavWindow->NavRectRel[0].Max.x + o.x, g.NavWindow->NavRectRel[0].Max.y + o.y);
    }
    std::fprintf(stderr, "NAV frame=%d id=%08x visible=%d window=%s rect=%.0f,%.0f-%.0f,%.0f\n", ImGui::GetFrameCount(), last,
                 int(last_visible), g.NavWindow ? g.NavWindow->Name : "-", r.Min.x, r.Min.y, r.Max.x, r.Max.y);
}

// SFR_LAUNCHER_INPUT_SCRIPT="a@2+0.2,down@3,..." drives the launcher with a
// scripted controller for unattended tests, in place of any real one:
// button@seconds[+duration] (0.25 s by default), seconds counted as 60
// frames. Buttons: a b x y start back up down left right lb rb lt rt.
bool feed_script_pad() {
    static const char* script = std::getenv("SFR_LAUNCHER_INPUT_SCRIPT");
    if (!script || !*script) return false;
    namespace b = sfr::gamepad_button;
    static const std::pair<const char*, uint16_t> names[] = {
        {"a", b::a}, {"b", b::b}, {"x", b::x}, {"y", b::y}, {"start", b::start}, {"back", b::back},
        {"up", b::dpad_up}, {"down", b::dpad_down}, {"left", b::dpad_left}, {"right", b::dpad_right},
        {"lb", b::left_shoulder}, {"rb", b::right_shoulder}};
    const double seconds = ImGui::GetFrameCount() / 60.0;
    sfr::GamepadState pad{};
    for (std::string_view rest = script; !rest.empty();) {
        const size_t comma = rest.find(',');
        const std::string entry(rest.substr(0, comma));
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        const size_t at = entry.find('@');
        if (at == std::string::npos) continue;
        const std::string name = entry.substr(0, at);
        const size_t plus = entry.find('+', at);
        const double start = std::atof(entry.substr(at + 1, plus == std::string::npos ? std::string::npos : plus - at - 1).c_str());
        const double duration = plus == std::string::npos ? 0.25 : std::atof(entry.substr(plus + 1).c_str());
        if (seconds < start || seconds >= start + duration) continue;
        if (name == "lt") pad.left_trigger = 255;
        else if (name == "rt") pad.right_trigger = 255;
        else for (const auto& [n, mask] : names) if (name == n) pad.buttons |= mask;
    }
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad;
    feed_pad(&pad);
    return true;
}

// PlayStation controllers are not XInput devices: feed them to ImGui's
// gamepad navigation ourselves when no XInput pad is connected.
void feed_sony_pad() {
    if (feed_script_pad()) return;
    ImGuiIO& io = ImGui::GetIO();
    if (io.BackendFlags & ImGuiBackendFlags_HasGamepad) return;
    const auto pad = sfr::sony::latest();
    if (!pad) return;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    sony_in_use = true;
    feed_pad(&*pad);
}

void feed_pad(const sfr::GamepadState* pad) {
    ImGuiIO& io = ImGui::GetIO();
    namespace b = sfr::gamepad_button;
    const auto button = [&](ImGuiKey key, uint16_t mask) { io.AddKeyEvent(key, (pad->buttons & mask) != 0); };
    button(ImGuiKey_GamepadStart, b::start);
    button(ImGuiKey_GamepadBack, b::back);
    button(ImGuiKey_GamepadFaceDown, b::a);
    button(ImGuiKey_GamepadFaceRight, b::b);
    button(ImGuiKey_GamepadFaceLeft, b::x);
    button(ImGuiKey_GamepadFaceUp, b::y);
    button(ImGuiKey_GamepadDpadLeft, b::dpad_left);
    button(ImGuiKey_GamepadDpadRight, b::dpad_right);
    button(ImGuiKey_GamepadDpadUp, b::dpad_up);
    button(ImGuiKey_GamepadDpadDown, b::dpad_down);
    button(ImGuiKey_GamepadL1, b::left_shoulder);
    button(ImGuiKey_GamepadR1, b::right_shoulder);
    button(ImGuiKey_GamepadL3, b::left_thumb);
    button(ImGuiKey_GamepadR3, b::right_thumb);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, pad->left_trigger > 64, float(pad->left_trigger) / 255.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, pad->right_trigger > 64, float(pad->right_trigger) / 255.0f);
    const auto stick = [&](ImGuiKey key, float value) {
        constexpr float dead = 8000.0f;
        const float amount = std::clamp((value - dead) / (32767.0f - dead), 0.0f, 1.0f);
        io.AddKeyAnalogEvent(key, amount > 0.1f, amount);
    };
    stick(ImGuiKey_GamepadLStickLeft, -float(pad->thumb_lx));
    stick(ImGuiKey_GamepadLStickRight, float(pad->thumb_lx));
    stick(ImGuiKey_GamepadLStickUp, float(pad->thumb_ly));
    stick(ImGuiKey_GamepadLStickDown, -float(pad->thumb_ly));
}

// The Windows virtual key an ImGui key stands for. Bindings are kept in
// Windows numbering wherever they travel (input_bindings.h), so a key chosen
// here means the same key to the game on every platform.
int virtual_key_of(ImGuiKey key) {
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z) return 'A' + (key - ImGuiKey_A);
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9) return '0' + (key - ImGuiKey_0);
    if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9) return 0x60 + (key - ImGuiKey_Keypad0);
    if (key >= ImGuiKey_F1 && key <= ImGuiKey_F12) return 0x70 + (key - ImGuiKey_F1);
    switch (key) {
    case ImGuiKey_Tab: return 0x09;
    case ImGuiKey_LeftArrow: return 0x25;
    case ImGuiKey_UpArrow: return 0x26;
    case ImGuiKey_RightArrow: return 0x27;
    case ImGuiKey_DownArrow: return 0x28;
    case ImGuiKey_PageUp: return 0x21;
    case ImGuiKey_PageDown: return 0x22;
    case ImGuiKey_Home: return 0x24;
    case ImGuiKey_End: return 0x23;
    case ImGuiKey_Insert: return 0x2D;
    case ImGuiKey_Delete: return 0x2E;
    case ImGuiKey_Backspace: return 0x08;
    case ImGuiKey_Space: return 0x20;
    case ImGuiKey_Enter: case ImGuiKey_KeypadEnter: return 0x0D;
    case ImGuiKey_Escape: return 0x1B;
    case ImGuiKey_LeftShift: case ImGuiKey_RightShift: return 0x10;
    case ImGuiKey_LeftCtrl: case ImGuiKey_RightCtrl: return 0x11;
    case ImGuiKey_LeftAlt: case ImGuiKey_RightAlt: return 0x12;
    case ImGuiKey_Semicolon: return 0xBA;
    case ImGuiKey_Equal: return 0xBB;
    case ImGuiKey_Comma: return 0xBC;
    case ImGuiKey_Minus: return 0xBD;
    case ImGuiKey_Period: return 0xBE;
    case ImGuiKey_Slash: return 0xBF;
    case ImGuiKey_GraveAccent: return 0xC0;
    case ImGuiKey_LeftBracket: return 0xDB;
    case ImGuiKey_Backslash: return 0xDC;
    case ImGuiKey_RightBracket: return 0xDD;
    case ImGuiKey_Apostrophe: return 0xDE;
    case ImGuiKey_KeypadMultiply: return 0x6A;
    case ImGuiKey_KeypadAdd: return 0x6B;
    case ImGuiKey_KeypadSubtract: return 0x6D;
    case ImGuiKey_KeypadDecimal: return 0x6E;
    case ImGuiKey_KeypadDivide: return 0x6F;
    default: return 0;
    }
}

// The key, or the pad control, pressed this frame -- what a player means
// when a control is waiting to be bound.
int captured_key() {
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_GamepadStart; ++key)
        if (ImGui::IsKeyPressed(ImGuiKey(key), false))
            if (const int virtual_key = virtual_key_of(ImGuiKey(key))) return virtual_key;
    return 0;
}

// Which prompts the button guide shows: the last device used.
enum class InputKind { keyboard, xbox, playstation };
enum class Prompt { confirm, back, start, tabs };

InputKind last_input(InputKind previous) {
    for (int key = ImGuiKey_GamepadStart; key <= ImGuiKey_GamepadRStickDown; ++key)
        if (ImGui::IsKeyPressed(ImGuiKey(key), false)) return sony_in_use ? InputKind::playstation : InputKind::xbox;
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_GamepadStart; ++key)
        if (ImGui::IsKeyPressed(ImGuiKey(key), false)) return InputKind::keyboard;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return InputKind::keyboard;
    return previous;
}

// One prompt icon, drawn: pad face buttons, a menu button, or a key cap.
// Returns its width.
float draw_prompt(ImDrawList* draw, ImVec2 at, float size, InputKind kind, Prompt prompt, ImFont* font, float alpha) {
    const float r = size * 0.5f;
    const ImVec2 c(at.x + r, at.y + r);
    const ImU32 ink = IM_COL32(255, 255, 255, int(235 * alpha));
    if (prompt == Prompt::tabs) {
        // The shoulder buttons (or Q and E) as two small caps.
        const char* names[2] = {kind == InputKind::keyboard ? "Q" : kind == InputKind::xbox ? "LB" : "L1",
                                kind == InputKind::keyboard ? "E" : kind == InputKind::xbox ? "RB" : "R1"};
        float x = at.x;
        for (const char* name : names) {
            const ImVec2 text = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, name);
            const float w = (std::max)(size, text.x + size * 0.6f);
            const bool key = kind == InputKind::keyboard;
            draw->AddRectFilled(ImVec2(x, at.y), ImVec2(x + w, at.y + size),
                                key ? IM_COL32(222, 232, 250, int(235 * alpha)) : IM_COL32(60, 70, 96, int(235 * alpha)), size * 0.3f);
            draw->AddText(font, font->FontSize, ImVec2(x + (w - text.x) * 0.5f, at.y + (size - text.y) * 0.5f),
                          key ? IM_COL32(20, 30, 60, int(255 * alpha)) : ink, name);
            x += w + size * 0.2f;
        }
        return x - at.x - size * 0.2f;
    }
    if (kind == InputKind::keyboard) {
        const char* key = prompt == Prompt::confirm ? "Enter" : prompt == Prompt::back ? "Esc" : "Enter";
        const ImVec2 text = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, key);
        const float w = (std::max)(size, text.x + size * 0.6f);
        draw->AddRectFilled(ImVec2(at.x, at.y + 2), ImVec2(at.x + w, at.y + size + 2), IM_COL32(0, 0, 0, int(90 * alpha)), size * 0.22f);
        draw->AddRectFilled(at, ImVec2(at.x + w, at.y + size), IM_COL32(222, 232, 250, int(235 * alpha)), size * 0.22f);
        draw->AddText(font, font->FontSize, ImVec2(at.x + (w - text.x) * 0.5f, at.y + (size - text.y) * 0.5f),
                      IM_COL32(20, 30, 60, int(255 * alpha)), key);
        return w;
    }
    if (prompt == Prompt::start) {
        // A pill with the three lines of a menu button.
        const float w = size * 1.5f;
        draw->AddRectFilled(at, ImVec2(at.x + w, at.y + size), IM_COL32(60, 70, 96, int(235 * alpha)), r);
        for (int i = -1; i <= 1; ++i)
            draw->AddLine(ImVec2(at.x + w * 0.33f, c.y + i * size * 0.18f), ImVec2(at.x + w * 0.67f, c.y + i * size * 0.18f), ink, size * 0.09f);
        return w;
    }
    if (kind == InputKind::xbox) {
        const bool confirm = prompt == Prompt::confirm;
        draw->AddCircleFilled(c, r, confirm ? IM_COL32(64, 170, 70, int(245 * alpha)) : IM_COL32(210, 60, 55, int(245 * alpha)), 32);
        const char* letter = confirm ? "A" : "B";
        const ImVec2 text = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, letter);
        draw->AddText(font, font->FontSize, ImVec2(c.x - text.x * 0.5f, c.y - text.y * 0.5f), ink, letter);
        return size;
    }
    // PlayStation: the cross confirms, the circle goes back.
    draw->AddCircleFilled(c, r, IM_COL32(40, 44, 58, int(245 * alpha)), 32);
    if (prompt == Prompt::confirm) {
        const float k = r * 0.42f;
        const ImU32 blue = IM_COL32(130, 170, 255, int(255 * alpha));
        draw->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), blue, size * 0.1f);
        draw->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), blue, size * 0.1f);
    } else {
        draw->AddCircle(c, r * 0.45f, IM_COL32(255, 105, 110, int(255 * alpha)), 24, size * 0.1f);
    }
    return size;
}

// ---------------------------------------------------------------- app
enum class Page { Settings, Stopped, Install, Installing, Installed };
enum Tab { GeneralTab, DisplayTab, SoundTab, ControlsTab, MotionTab, AvatarTab, ModsTab, FilesTab, TabCount };
enum class Question { none, quit, cancel_install, quit_during_install };

// An installation running on its own thread; the page reads its progress.
struct Installation {
    std::thread worker;
    std::atomic<bool> cancel{false}, finished{false};
    std::mutex lock;
    sfr::InstallStage stage = sfr::InstallStage::checking;
    uint64_t done = 0, total = 0;
    std::string item, error;
    bool cancelled = false;
    std::chrono::steady_clock::time_point copy_started{};
};

uint64_t free_bytes(const fs::path& directory) {
    std::error_code error;
    const auto space = fs::space(directory, error);
    return error ? 0 : space.available;
}

double gigabytes(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0 * 1024.0); }

// Layout, in unscaled pixels of the 1120 x 700 window.
constexpr float top_bar = 88, bottom_bar = 96, margin = 44;

struct Launcher {
    // Hides the launcher while the game runs, shows it again after.
    std::function<void(bool)> show_window = [](bool) {};
    fs::path directory, settings_file, log_file, runtime_root;
    sfr::LauncherSettings settings;
    bool avatar_model_import_failed = false;
    sfr::UiSounds ui_sounds;
    Fonts fonts;
    float scale = 1.0f;
    Page page = Page::Settings;
    int tab = GeneralTab;
    float tab_fade = 1.0f;
    // The host's cameras, listed while the camera setting is on.
    std::vector<std::string> cameras;
    std::string cameras_listed_for = "off";
    // Trying the chosen camera: a camera that opens does not always send
    // anything (a phone camera whose phone is elsewhere, a capture card with
    // nothing plugged in), and the game can only stand there looking puzzled.
    // The answer takes seconds, so it is found on a thread of its own.
    enum class CameraTrial { none, looking, pictures, silent, closed };
    std::atomic<CameraTrial> camera_trial{CameraTrial::none};
    std::jthread camera_trial_worker;
    // Why the preview could not open the Kinect, kept after its window
    // closes: which step is missing, the SDK, the sensor (unplugged, or its
    // power adapter not in), or starting it.
    std::string kinect_failure;
    // The camera choice under the pointer while its list is open, or -1.
    int camera_hovered = -1;
    // The preview window, when open, holds the Kinect (one session a
    // process); it has the tilt buttons.
    std::unique_ptr<sfr::KinectPreviewWindow> kinect_preview;
    // The launcher slides in (appear rises to 1) and out before the game
    // starts (leaving, appear falls to 0).
    float appear = 0.0f, page_fade = 0.0f;
    bool leaving = false;
    bool quit_now = false;  // the window closes after this frame
    bool focus_start = true;
    const char* status = nullptr;
    bool status_error = false;
    uint32_t exit_code = 0;
    std::vector<std::string> stopped_lines;
    std::unique_ptr<sfr::launcher::GameProcess> game;
    uint32_t desktop_width = 1920, desktop_height = 1080;
    InputKind input = InputKind::keyboard;
    ImGuiID last_nav = 0;
    Question question = Question::none;
    float question_fade = 0.0f;
    bool question_focus = false;
    // The install pages.
    fs::path source;
    sfr::SourceSummary summary;
    uint64_t available = 0;
    std::unique_ptr<Installation> installation;
    std::string install_error;
    bool install_cancelled = false;
    float shown_progress = 0.0f;
    double installed_at = 0.0;

    fs::path game_directory() const { return directory / "game"; }

    // The checkout's shader tools, or a shaders.pack beside the launcher.
    bool shaders_available() const {
        std::error_code error;
        return !runtime_root.empty() || !sfr::shader_tool_environment(directory).empty() ||
               fs::is_regular_file(directory / "shaders.pack", error);
    }

    bool files_ready() const {
        return sfr::is_image_directory(settings.image_directory) && sfr::is_asset_directory(settings.asset_directory);
    }

    void set_page(Page next) {
        page = next;
        page_fade = 0.0f;
        focus_start = true;
    }

    // ------------------------------------------------ game
    void play() {
        sfr::save_launcher_settings(settings_file, settings);
        std::error_code error;
        const fs::path program = sfr::launcher::game_program(directory);
        if (!program.empty() && !fs::is_regular_file(program, error)) {
            status = tr(MissingGame); status_error = true;
            ::play(sfr::UiSound::error);
            return;
        }
        if (!files_ready()) {
            status = tr(MissingFiles); status_error = true;
            tab = FilesTab; tab_fade = 0.0f;
            ::play(sfr::UiSound::error);
            return;
        }
        leaving = true;  // the game starts once the launcher has slid away
    }

    void launch_now() {
        leaving = false;
        // The game opens the Kinect itself.
        kinect_preview.reset();
        sfr::save_launcher_settings(settings_file, settings);
        game = sfr::launcher::start_game(settings, directory, log_file);
        if (!game) {
            status = tr(LaunchFailed); status_error = true;
            ::play(sfr::UiSound::error);
            return;
        }
        show_window(false);
    }

    // The game ended: a closed window ends the launcher too; anything else
    // brings it back with the reason.
    bool game_ended() {
        exit_code = game->exit_code().value_or(0);
        game.reset();
        stopped_lines = log_tail(log_file, 14);
        bool closed = exit_code == 0;
        for (const auto& line : stopped_lines)
            if (line.find("STOP window-closed") != std::string::npos) closed = true;
        appear = 0.0f;
        if (closed) {
            if (sfr::launcher::quit_with_game()) return false;
            set_page(Page::Settings);
        } else {
            set_page(Page::Stopped);
            ::play(sfr::UiSound::error);
        }
        show_window(true);
        return true;
    }

    // ------------------------------------------------ back and questions
    void ask(Question which) {
        question = which;
        question_fade = 0.0f;
        question_focus = true;
        ::play(sfr::UiSound::move);
    }

    // B or Escape.
    void back() {
        switch (page) {
        case Page::Settings: ask(Question::quit); break;
        case Page::Install:
            if (files_ready()) { set_page(Page::Settings); ::play(sfr::UiSound::back); }
            else ask(Question::quit);
            break;
        case Page::Installing:
            if (!installation->cancel) ask(Question::cancel_install);
            break;
        case Page::Stopped:
        case Page::Installed:
            status = nullptr;
            set_page(Page::Settings);
            ::play(sfr::UiSound::back);
            break;
        }
    }

    // The window's close button: at once, unless it would cut an install short.
    void close_requested() {
        if (installation) ask(Question::quit_during_install);
        else quit_now = true;
    }

    void answer(bool yes) {
        const Question asked = question;
        question = Question::none;
        ::play(yes ? sfr::UiSound::confirm : sfr::UiSound::back);
        focus_start = true;
        if (!yes) return;
        if (asked == Question::quit) quit_now = true;
        else if (asked == Question::cancel_install && installation) installation->cancel = true;
        else if (asked == Question::quit_during_install) quit_now = true;  // abandon_install cleans up
    }

    void question_box(ImVec2 size) {
        if (question == Question::none) return;
        if (!ImGui::IsPopupOpen("##question")) ImGui::OpenPopup("##question");
        question_fade = approach(question_fade, 1.0f, 12.0f);
        const Text title = question == Question::quit ? QuitTitle : CancelInstallTitle;
        const Text text = question == Question::quit ? QuitText : CancelInstallText;
        const float width = 520 * scale;
        ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y * 0.5f + (1.0f - ease_out(question_fade)) * 30 * scale),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(width, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, question_fade);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(32 * scale, 26 * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16 * scale);  // a modal takes the window style
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(cyan.x, cyan.y, cyan.z, 0.45f));
        if (ImGui::BeginPopupModal("##question", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 at = ImGui::GetWindowPos();
            draw->AddRectFilled(ImVec2(at.x + 20 * scale, at.y), ImVec2(at.x + width - 20 * scale, at.y + 3 * scale), color(cyan, 1.0f));
            ImGui::PushFont(fonts.heading);
            ImGui::TextUnformatted(tr(title));
            ImGui::PopFont();
            ImGui::PushTextWrapPos(width - 32 * scale);
            ImGui::TextColored(dim_text, "%s", wrapped(tr(text), room_to(width - 32 * scale)).c_str());
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 14 * scale));
            const float button_width = 140 * scale, button_height = 44 * scale;
            ImGui::SetCursorPosX(width - 32 * scale - button_width * 2 - 12 * scale);
            ImGui::PushFont(fonts.heading);
            bool yes = false, no = false;
            if (big_button(tr(Yes), ImVec2(button_width, button_height), false, scale)) yes = true;
            ImGui::SameLine(0, 12 * scale);
            if (question_focus && ImGui::GetFrameCount() > 2) {
                // "No" first: a stray press never quits or cancels.
                focus_next();
                question_focus = false;
            }
            if (big_button(tr(No), ImVec2(button_width, button_height), true, scale)) no = true;
            ImGui::PopFont();
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) no = true;
            if (yes || no) {
                ImGui::CloseCurrentPopup();
                answer(yes);
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(4);
    }

    // ------------------------------------------------ chrome
    // The bars at the top and bottom, sliding in with appear.
    // ------------------------------------------------ language
    void set_language(const char* code) {
        if (settings.language == code) return;
        settings.language = code;
        apply_language(settings.language);
        sfr::save_launcher_settings(settings_file, settings);
        ::play(sfr::UiSound::confirm);
    }

    // The choice in General (reachable with keys and pads).
    void language_row() {
        const float combo_width = 300 * scale;
        setting_row(tr(LanguageLabel), tr(LanguageHint), combo_width, scale, [&] {
            ImGui::SetNextItemWidth(combo_width);
            const auto current = std::find_if(languages.begin(), languages.end(),
                                              [&](const auto& item) { return settings.language == item.first; });
            if (ImGui::BeginCombo("##language", tr(current != languages.end() ? current->second : LanguageSystem))) {
                for (const auto& [code, name] : languages) {
                    const bool chosen = settings.language == code;
                    const bool usable = chinese_available || std::string_view(code) != "zh-TW";
                    if (ImGui::Selectable(tr(name), chosen, usable ? 0 : ImGuiSelectableFlags_Disabled)) set_language(code);
                    if (chosen) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        });
    }

    float language_button_width() const { return 104 * scale; }

    // The top bar's right corner: a globe and the language, a menu of the
    // three choices (the mouse's and touch's way; keys use the Display tab).
    void language_button(ImVec2 size) {
        const float shown = ease_out(appear);
        const float width = language_button_width(), height = 36 * scale;
        const ImVec2 at(size.x - margin * scale - width + (1.0f - shown) * 60 * scale,
                        top_bar * scale * shown - top_bar * scale + 24 * scale);
        ImGui::SetCursorScreenPos(at);
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        const bool pressed = ImGui::InvisibleButton("##language-button", ImVec2(width, height));
        ImGui::PopItemFlag();
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 end(at.x + width, at.y + height);
        draw->AddRectFilled(at, end, hovered ? IM_COL32(40, 70, 130, 230) : IM_COL32(20, 34, 70, 200), height * 0.5f);
        draw->AddRect(at, end, color(cyan, hovered ? 0.9f : 0.45f), height * 0.5f, 0, 1.5f * scale);
        // The globe: a circle, its equator and a meridian.
        const float r = height * 0.28f;
        const ImVec2 globe(at.x + height * 0.5f + 2 * scale, at.y + height * 0.5f);
        const ImU32 ink = IM_COL32(220, 235, 255, 240);
        draw->AddCircle(globe, r, ink, 24, 1.6f * scale);
        draw->AddLine(ImVec2(globe.x - r, globe.y), ImVec2(globe.x + r, globe.y), ink, 1.2f * scale);
        draw->AddEllipse(globe, ImVec2(r * 0.45f, r), ink, 0.0f, 20, 1.2f * scale);
        const char* short_name = language == 1 ? "中文" : "EN";
        const ImVec2 text = fonts.tiny->CalcTextSizeA(fonts.tiny->FontSize, FLT_MAX, 0, short_name);
        draw->AddText(fonts.tiny, fonts.tiny->FontSize,
                      ImVec2(globe.x + r + 10 * scale + (end.x - globe.x - r - 10 * scale - text.x) * 0.5f - 6 * scale,
                             at.y + (height - text.y) * 0.5f), ink, short_name);
        if (pressed) {
            ImGui::OpenPopup("##languages");
            ::play(sfr::UiSound::move);
        }
        ImGui::SetNextWindowPos(ImVec2(end.x, end.y + 6 * scale), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * scale, 10 * scale));
        if (ImGui::BeginPopup("##languages")) {
            for (const auto& [code, name] : languages) {
                const bool chosen = settings.language == code;
                const bool usable = chinese_available || std::string_view(code) != "zh-TW";
                if (ImGui::Selectable(tr(name), chosen, usable ? 0 : ImGuiSelectableFlags_Disabled, ImVec2(200 * scale, ImGui::GetFrameHeight())))
                    set_language(code);
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

    void bars(ImVec2 size, const char* label, bool breathing, float right_reserved = 0.0f) {
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        const float t = float(ImGui::GetTime());
        const float shown = ease_out(appear);
        const float top = top_bar * scale * shown, bottom = size.y - bottom_bar * scale * shown;
        // Top bar.
        draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x, top), IM_COL32(3, 8, 24, 235), IM_COL32(8, 6, 30, 235),
                                      IM_COL32(6, 16, 44, 225), IM_COL32(6, 16, 44, 225));
        for (float y = 2; y < top; y += 3 * scale) draw->AddLine(ImVec2(0, y), ImVec2(size.x, y), IM_COL32(120, 180, 255, 7));
        const auto edge = [&](float y, bool down) {
            draw->AddRectFilledMultiColor(ImVec2(0, y - 1 * scale), ImVec2(size.x, y + 1 * scale), color(cyan, 0.9f * shown),
                                          color(accent, 0.15f * shown), color(accent, 0.15f * shown), color(cyan, 0.9f * shown));
            const float glow = 14 * scale * (down ? 1.0f : -1.0f);
            draw->AddRectFilledMultiColor(ImVec2(0, y), ImVec2(size.x * 0.6f, y + glow), color(cyan, 0.18f * shown),
                                          color(cyan, 0.0f), color(cyan, 0.0f), color(cyan, 0.0f));
        };
        edge(top, true);
        // Chevrons streaming right across the top bar's right side.
        for (int i = 0; i < 9; ++i) {
            const float phase = std::fmod(t * 0.9f + i / 9.0f, 1.0f);
            const float x = size.x - 330 * scale + phase * 300 * scale, y = top - 44 * scale;
            const float a = std::sin(phase * 3.14159f) * 0.35f * shown;
            const float h = 10 * scale;
            draw->AddTriangleFilled(ImVec2(x, y - h), ImVec2(x + 9 * scale, y), ImVec2(x, y + h), color(cyan, a));
            draw->AddTriangleFilled(ImVec2(x - 6 * scale, y - h), ImVec2(x + 3 * scale, y), ImVec2(x - 6 * scale, y + h), IM_COL32(6, 16, 44, 255));
        }
        // The logo: FREE RIDERS in black italic, RECOMPILED beside it.
        const float title_x = margin * scale - (1.0f - shown) * 60 * scale;
        const float title_y = top - top_bar * scale + 18 * scale;
        const char* logo = "FREE RIDERS";
        draw->AddText(fonts.title, fonts.title->FontSize, ImVec2(title_x + 3 * scale, title_y + 3 * scale), IM_COL32(0, 0, 0, int(120 * shown)), logo);
        draw->AddText(fonts.title, fonts.title->FontSize, ImVec2(title_x, title_y), color(ImVec4(1, 1, 1, 1), shown), logo);
        const ImVec2 logo_size = fonts.title->CalcTextSizeA(fonts.title->FontSize, FLT_MAX, 0, logo);
        draw->AddText(fonts.tiny, fonts.tiny->FontSize, ImVec2(title_x + logo_size.x + 12 * scale, title_y + logo_size.y - fonts.tiny->FontSize - 8 * scale),
                      color(cyan, shown), "RECOMPILED");
        // The page's name on the right, breathing while installing.
        const float breath = breathing ? 0.55f + 0.45f * (0.5f + 0.5f * std::sin(t * 3.0f)) : 1.0f;
        const ImVec2 label_size = fonts.heading->CalcTextSizeA(fonts.heading->FontSize, FLT_MAX, 0, label);
        draw->AddText(fonts.heading, fonts.heading->FontSize,
                      ImVec2(size.x - margin * scale - right_reserved - (right_reserved > 0 ? 18 * scale : 0.0f) - label_size.x +
                                 (1.0f - shown) * 60 * scale, title_y + 8 * scale),
                      color(accent_bright, shown * breath), label);
        // Bottom bar.
        draw->AddRectFilledMultiColor(ImVec2(0, bottom), size, IM_COL32(6, 16, 44, 225), IM_COL32(6, 16, 44, 225),
                                      IM_COL32(3, 8, 24, 240), IM_COL32(8, 6, 30, 240));
        for (float y = bottom + 2; y < size.y; y += 3 * scale) draw->AddLine(ImVec2(0, y), ImVec2(size.x, y), IM_COL32(120, 180, 255, 7));
        edge(bottom, false);
    }

    // What the buttons do on this page, drawn at the bottom left.
    void guide(ImVec2 size, std::initializer_list<std::pair<Prompt, Text>> entries) {
#ifdef __ANDROID__
        if (input == InputKind::keyboard) return;  // touch: the buttons speak for themselves
#endif
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        const float shown = ease_out(appear);
        const float icon = 26 * scale;
        float x = margin * scale;
        const float y = size.y - bottom_bar * scale * shown + (bottom_bar * scale - icon) * 0.5f;
        for (const auto& [prompt, text] : entries) {
            if (prompt == Prompt::start && input == InputKind::keyboard) continue;
            x += draw_prompt(draw, ImVec2(x, y), icon, input, prompt, fonts.tiny, shown) + 8 * scale;
            const ImVec2 text_size = fonts.body->CalcTextSizeA(fonts.body->FontSize, FLT_MAX, 0, tr(text));
            draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(x, y + (icon - text_size.y) * 0.5f), color(ImVec4(1, 1, 1, 0.9f), shown), tr(text));
            x += text_size.x + 26 * scale;
        }
    }

    // Right-aligned action buttons in the bottom bar; returns the pressed one.
    int actions(ImVec2 size, std::initializer_list<std::pair<const char*, bool>> buttons, int focus, bool disabled_last = false) {
        const float height = 50 * scale, gap = 14 * scale;
        std::vector<float> widths;
        float total = 0;
        ImGui::PushFont(fonts.heading);
        for (const auto& [label, primary] : buttons) {
            widths.push_back((std::max)(primary ? 210 * scale : 150 * scale, ImGui::CalcTextSize(label).x + 50 * scale));
            total += widths.back();
        }
        total += gap * float(buttons.size() - 1);
        const float shown = ease_out(appear);
        float x = size.x - margin * scale - total;
        const float y = size.y - bottom_bar * scale * shown + (bottom_bar * scale - height) * 0.5f;
        int pressed = -1, index = 0;
        for (const auto& [label, primary] : buttons) {
            ImGui::SetCursorPos(ImVec2(x, y));
            const bool last = index == int(buttons.size()) - 1;
            if (last && disabled_last) ImGui::BeginDisabled();
            if (index == focus && focus_start && ImGui::GetFrameCount() > 2 && !(last && disabled_last)) {
                focus_next();
                focus_start = false;
            }
            if (big_button(label, ImVec2(widths[index], height), primary, scale)) pressed = index;
            if (last && disabled_last) ImGui::EndDisabled();
            x += widths[index] + gap;
            ++index;
        }
        ImGui::PopFont();
        return pressed;
    }

    // The page's content area, sliding in from the right as it appears.
    float content_top() const { return (top_bar + 26) * scale; }
    float content_bottom(ImVec2 size) const { return size.y - (bottom_bar + 18) * scale; }
    float slide() const { return (1.0f - ease_out(page_fade)) * 36 * scale; }

    void page_heading(const char* title, ImVec4 tint = ImVec4(1, 1, 1, 1)) {
        ImGui::SetCursorPos(ImVec2(margin * scale + slide(), content_top()));
        ImGui::PushFont(fonts.heading);
        ImGui::TextColored(tint, "%s", title);
        ImGui::PopFont();
        const ImVec2 min = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(min.x, ImGui::GetItemRectMax().y + 4 * scale),
                                                  ImVec2(min.x + 48 * scale * ease_out(page_fade), ImGui::GetItemRectMax().y + 7 * scale),
                                                  color(cyan, 1.0f), 2 * scale);
        ImGui::Dummy(ImVec2(0, 8 * scale));
    }

    // ------------------------------------------------ settings
    // The categories: clicked with the mouse, or stepped through with LB/RB
    // (L1/R1, Q/E, Page Up/Down); Up and Down stay among the options.
    void tab_list(ImVec2 size) {
        const float left = ImGui::GetCursorPosX();
        ImGui::PushFont(fonts.heading);
        constexpr Text names[TabCount] = {TabGeneral, TabDisplay, TabSound, TabControls, TabMotion, TabAvatar, TabMods, TabFiles};
        for (int i = 0; i < TabCount; ++i) {
            ImGui::SetCursorPosX(left);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float height = ImGui::GetTextLineHeight() + 22 * scale;
            ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1, 1, 1, 0.06f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(1, 1, 1, 0.10f));
            ImGui::PushStyleColor(ImGuiCol_Text, i == tab ? ImVec4(1, 1, 1, 1) : dim_text);
            char id[64];
            std::snprintf(id, sizeof id, "      %s##tab%d", tr(names[i]), i);
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            const bool clicked = ImGui::Selectable(id, i == tab, 0, ImVec2(size.x, height));
            ImGui::PopItemFlag();
            if (clicked) {
                select_tab(i);
            }
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar();
            if (i == tab) {
                float& marker = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID("marker"), at.y);
                marker = approach(marker, at.y, 18.0f);
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x + 4 * scale, marker + 10 * scale),
                                                          ImVec2(at.x + 9 * scale, marker + height - 10 * scale),
                                                          color(cyan, 1.0f), 3 * scale);
            }
        }
        ImGui::PopFont();
    }

    void select_tab(int next) {
        if (tab == next) return;
        tab = next;
        tab_fade = 0.0f;
        focus_first_control = true;
        capturing = -1;
        ::play(sfr::UiSound::move);
    }

    void settings_section(Text label) {
        ImGui::Dummy(ImVec2(0, 4 * scale));
        ImGui::PushStyleColor(ImGuiCol_Text, cyan);
        ImGui::SeparatorText(tr(label));
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 4 * scale));
    }

    void display_settings() {
        std::vector<sfr::WindowSize> sizes = sfr::common_window_sizes();
        const auto has = [&](uint32_t w, uint32_t h) {
            return std::any_of(sizes.begin(), sizes.end(), [&](const auto& s) { return s.width == w && s.height == h; });
        };
        if (!has(desktop_width, desktop_height)) sizes.push_back({desktop_width, desktop_height});
        if (!has(settings.window_width, settings.window_height)) sizes.push_back({settings.window_width, settings.window_height});
        std::sort(sizes.begin(), sizes.end(), [](const auto& a, const auto& b) { return a.width * a.height < b.width * b.height; });
        const auto name = [&](const sfr::WindowSize& s) {
            char text[96];
            if (s.width == desktop_width && s.height == desktop_height)
                std::snprintf(text, sizeof text, "%u x %u  (%s)", s.width, s.height, tr(DesktopSize));
            else std::snprintf(text, sizeof text, "%u x %u", s.width, s.height);
            return std::string(text);
        };
        const float combo_width = 300 * scale;
        const float switch_width = ImGui::GetFrameHeight() * 1.9f;
        settings_section(SectionResolution);
#ifndef __ANDROID__  // fullscreen phones have no adjustable output window
        setting_row(tr(WindowSize), tr(WindowResolutionHint), combo_width, scale, [&] {
            ImGui::SetNextItemWidth(combo_width);
            if (ImGui::BeginCombo("##size", name({settings.window_width, settings.window_height}).c_str())) {
                for (const auto& s : sizes) {
                    const bool chosen = s.width == settings.window_width && s.height == settings.window_height;
                    if (ImGui::Selectable(name(s).c_str(), chosen)) {
                        settings.window_width = s.width;
                        settings.window_height = s.height;
                        ::play(sfr::UiSound::confirm);
                    }
                    if (chosen) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        });
#else
        (void)name;
#endif
        const auto render_name = [&](uint32_t percent) {
            char text[96];
            if (percent == 100)
                std::snprintf(text, sizeof text, "1280 x 720  (%s)", tr(NativeResolution));
            else std::snprintf(text, sizeof text, "%u x %u", 1280 * percent / 100, 720 * percent / 100);
            return std::string(text);
        };
        setting_row(tr(RenderResolution), tr(RenderResolutionHint), combo_width, scale, [&] {
            ImGui::SetNextItemWidth(combo_width);
            if (ImGui::BeginCombo("##render_scale", render_name(settings.render_scale).c_str())) {
                for (const uint32_t percent : {50u, 75u, 100u, 150u, 200u}) {
                    const bool chosen = percent == settings.render_scale;
                    if (ImGui::Selectable(render_name(percent).c_str(), chosen)) {
                        settings.render_scale = percent;
                        ::play(sfr::UiSound::confirm);
                    }
                    if (chosen) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        });
#ifndef __ANDROID__
        settings_section(SectionWindow);
        setting_row(tr(Fullscreen), tr(FullscreenHint), switch_width, scale, [&] { toggle("##full", &settings.fullscreen); });
#endif
        settings_section(SectionRendering);
        setting_row(tr(VSync), tr(VSyncHint), switch_width, scale, [&] { toggle("##vsync", &settings.vsync); });
#ifdef _WIN32  // elsewhere the game always draws with Vulkan
        setting_row(tr(GraphicsBackendLabel), tr(GraphicsBackendHint), combo_width, scale, [&] {
            ImGui::SetNextItemWidth(combo_width);
            if (ImGui::BeginCombo("##graphics_backend", settings.vulkan ? "Vulkan" : "D3D12")) {
                for (const bool vulkan : {true, false}) {
                    const bool chosen = settings.vulkan == vulkan;
                    if (ImGui::Selectable(vulkan ? "Vulkan" : "D3D12", chosen)) {
                        settings.vulkan = vulkan;
                        ::play(sfr::UiSound::confirm);
                    }
                    if (chosen) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        });
#endif
        ImGui::Dummy(ImVec2(0, 10 * scale));
        if (ImGui::CollapsingHeader(tr(SectionPerformance))) performance_settings();
    }

    void sound_settings() {
        settings_section(SectionGameAudio);
        const float switch_width = ImGui::GetFrameHeight() * 1.9f;
        setting_row(tr(Sound), tr(SoundHint), switch_width, scale, [&] { toggle("##audio", &settings.audio); });
        const float slider_width = 300 * scale;
        ImGui::BeginDisabled(!settings.audio);
        setting_row(tr(Volume), nullptr, slider_width, scale, [&] {
            int volume = int(settings.volume);
            ImGui::SetNextItemWidth(slider_width);
            bool moved = ImGui::SliderInt("##volume", &volume, 0, 100, "%d%%");
            moved |= slider_steps(&volume, 0, 100, 5);
            if (moved) settings.volume = uint32_t(volume);
        });
        ImGui::EndDisabled();
        settings_section(SectionLauncherAudio);
        setting_row(tr(UiSoundsLabel), tr(UiSoundsHint), switch_width, scale, [&] {
            if (toggle("##ui", &settings.ui_sounds)) {
                ui_sounds.enabled = settings.ui_sounds;
                if (settings.ui_sounds) ::play(sfr::UiSound::toggle_on);
            }
        });
    }

    void general_settings() {
        settings_section(SectionLanguage);
        language_row();
        setting_row(tr(GameLanguageLabel), tr(GameLanguageHint), 220 * scale, scale, [&] {
            const char* current = tr(LanguageSystem);
            for (const auto& item : sfr::game_languages)
                if (item.code == settings.game_language) current = item.name;
            ImGui::SetNextItemWidth(220 * scale);
            if (ImGui::BeginCombo("##game_language", current)) {
                if (ImGui::Selectable(tr(LanguageSystem), settings.game_language == "auto"))
                    settings.game_language = "auto";
                for (const auto& item : sfr::game_languages) {
                    const bool selected = item.code == settings.game_language;
                    if (ImGui::Selectable(item.name, selected)) settings.game_language = item.code;
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        });
        setting_row(tr(VoiceLanguageLabel), tr(VoiceLanguageHint), 220 * scale, scale, [&] {
            static constexpr std::array<std::pair<const char*, const char*>, 2> voices{{
                {"en", "English"}, {"ja", "日本語"}}};
            const char* current = tr(VoiceFollowsGame);
            for (const auto& [code, name] : voices)
                if (settings.voice_language == code) current = name;
            ImGui::SetNextItemWidth(220 * scale);
            if (ImGui::BeginCombo("##voice_language", current)) {
                if (ImGui::Selectable(tr(VoiceFollowsGame), settings.voice_language == "auto"))
                    settings.voice_language = "auto";
                for (const auto& [code, name] : voices) {
                    const bool selected = settings.voice_language == code;
                    if (ImGui::Selectable(name, selected)) settings.voice_language = code;
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        });
        settings_section(SectionPlayback);
        const float switch_width = ImGui::GetFrameHeight() * 1.9f;
        setting_row(tr(SkipMovies), tr(SkipMoviesHint), switch_width, scale, [&] { toggle("##movies", &settings.skip_movies); });
        settings_section(SectionReset);
        reset_settings();
    }

    // ------------------------------------------------ controls
    // Two players' bindings; the four sets on the page are each player's
    // keys and the same player's pad (input_bindings.h).
    sfr::InputBindings bindings[2];
    bool bindings_read = false;
    int binding_set = 0;   // 0: 1P keys, 1: 1P pad, 2: 2P keys, 3: 2P pad
    int capturing = -1;    // the action waiting for a press, or -1
    bool capture_frame = false;  // stays true through the frame that accepts a binding
    sfr::PadButtonCapture pad_capture;

    void read_bindings() {
        if (bindings_read) return;
        bindings_read = true;
        for (uint32_t player = 0; player < 2; ++player) {
            bindings[player] = sfr::default_bindings(player);
            sfr::read_keys(player ? settings.player2_keys : settings.player1_keys, bindings[player]);
            sfr::read_pad(player ? settings.player2_pad : settings.player1_pad, bindings[player]);
        }
    }
    void write_bindings() {
        settings.player1_keys = sfr::format_keys(bindings[0]);
        settings.player1_pad = sfr::format_pad(bindings[0]);
        settings.player2_keys = sfr::format_keys(bindings[1]);
        settings.player2_pad = sfr::format_pad(bindings[1]);
    }

    // What each control is called here: an Xbox pad's own names, because
    // that is what the game reads and what is printed on a pad.
    static const char* action_label(sfr::InputAction action) {
        static const std::array<std::array<const char*, 2>, sfr::input_action_count> names{{
            {"A", "A"}, {"B", "B"}, {"X", "X"}, {"Y", "Y"},
            {"LB", "LB"}, {"RB", "RB"}, {"LT", "LT"}, {"RT", "RT"},
            {"Start", "Start"}, {"Back", "Back"},
            {"Left stick press", "左搖桿按下"}, {"Right stick press", "右搖桿按下"},
            {"D-pad up", "十字鍵 上"}, {"D-pad down", "十字鍵 下"},
            {"D-pad left", "十字鍵 左"}, {"D-pad right", "十字鍵 右"},
            {"Left stick up", "左搖桿 上"}, {"Left stick down", "左搖桿 下"},
            {"Left stick left", "左搖桿 左"}, {"Left stick right", "左搖桿 右"},
            {"Right stick up", "右搖桿 上"}, {"Right stick down", "右搖桿 下"},
            {"Right stick left", "右搖桿 左"}, {"Right stick right", "右搖桿 右"}}};
        return names[size_t(action)][language];
    }

    // One row of the list: the control's name, and a button showing what it
    // is bound to. Choosing the button waits for a press.
    void binding_row(sfr::InputAction action, bool keys, sfr::InputBindings& player) {
        const float button_width = 200 * scale;
        const bool waiting = capturing == int(action);
        char shown[64];
        if (waiting) std::snprintf(shown, sizeof shown, "%s", tr(keys ? PressKey : PressButton));
        else if (keys) std::snprintf(shown, sizeof shown, "%s", player[action] ? sfr::key_name(player[action]).c_str() : tr(Unbound));
        else {
            const sfr::InputAction source = player.from(action);
            std::snprintf(shown, sizeof shown, "%s",
                          size_t(source) < sfr::input_action_count ? action_label(source) : tr(Unbound));
        }
        setting_row(action_label(action), nullptr, button_width, scale, [&] {
            char id[96];
            std::snprintf(id, sizeof id, "%s##bind%d", shown, int(action));
            if (ImGui::Button(id, ImVec2(button_width, 0))) {
                capturing = waiting ? -1 : int(action);
                if (!keys && capturing >= 0) pad_capture.reset(sfr::poll_pad_buttons());
            }
            focus_glow(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetStyle().FrameRounding,
                       focus_amount(ImGui::IsItemFocused() && nav_visible()), scale);
        });
    }

    void controls_settings() {
#ifdef __ANDROID__
        settings_section(SectionTouch);
        const float switch_width = ImGui::GetFrameHeight() * 1.9f;
        setting_row(tr(TouchLabel), tr(TouchHint), switch_width, scale, [&] { toggle("##touch", &settings.touch_controls); });
        setting_row(tr(TiltLabel), tr(TiltHint), switch_width, scale, [&] { toggle("##tilt", &settings.tilt); });
#endif
        read_bindings();
        const float combo_width = 300 * scale;
        const auto choice = [&](const char* id, Text label, Text hint, std::string& value,
                                std::initializer_list<std::pair<const char*, Text>> options) {
            setting_row(tr(label), hint == TextCount ? nullptr : tr(hint), combo_width, scale, [&] {
                const char* shown = options.begin()->second ? tr(options.begin()->second) : "";
                for (const auto& [name, text] : options)
                    if (value == name) shown = tr(text);
                ImGui::SetNextItemWidth(combo_width);
                if (ImGui::BeginCombo(id, shown)) {
                    for (const auto& [name, text] : options) {
                        const bool chosen = value == name;
                        if (ImGui::Selectable(tr(text), chosen)) {
                            value = name;
                            ::play(sfr::UiSound::confirm);
                        }
                        if (chosen) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            });
        };
        // Which controller each player holds, by name. The list is what is
        // plugged in now, and a name chosen earlier stays on the list even
        // while its controller is away, so unplugging it does not silently
        // choose another.
        const auto pads = sfr::connected_pads();
        const auto gamepad_row = [&](const char* id, Text label, std::string& chosen, bool enabled) {
            ImGui::BeginDisabled(!enabled);
            setting_row(tr(label), tr(GamepadHint), combo_width, scale, [&] {
                char shown[160];
                const uint64_t selected = sfr::pad_with_name(chosen, pads);
                const auto device = std::find_if(pads.begin(), pads.end(),
                                                [&](const sfr::PadDevice& pad) { return pad.id == selected; });
                const bool here = device != pads.end();
                if (chosen.empty()) std::snprintf(shown, sizeof shown, "%s", tr(GamepadAutomatic));
                else if (here) std::snprintf(shown, sizeof shown, "%s", device->name.c_str());
                else std::snprintf(shown, sizeof shown, "%s (%s)", chosen.c_str(), tr(GamepadMissing));
                ImGui::SetNextItemWidth(combo_width);
                if (ImGui::BeginCombo(id, shown)) {
                    if (ImGui::Selectable(tr(GamepadAutomatic), chosen.empty())) {
                        chosen.clear();
                        ::play(sfr::UiSound::confirm);
                    }
                    for (const sfr::PadDevice& pad : pads) {
                        ImGui::PushID(int(pad.id));
                        if (ImGui::Selectable(pad.name.c_str(), pad.id == selected)) {
                            chosen = pad.identity.empty() ? pad.name : pad.identity;
                            ::play(sfr::UiSound::confirm);
                        }
                        ImGui::PopID();
                    }
                    if (!chosen.empty() && !here && ImGui::Selectable(shown, true)) {}
                    ImGui::EndCombo();
                }
            });
            ImGui::EndDisabled();
        };
        settings_section(SectionPlayer1);
        choice("##p1device", PlayerOneDevice, PlayerOneDeviceHint, settings.player1_device,
               {{"both", DeviceBoth}, {"gamepad", DeviceGamepad}, {"keyboard", DeviceKeyboard}});
        gamepad_row("##p1gamepad", PlayerOneGamepad, settings.player1_gamepad, settings.player1_device != "keyboard");
        settings_section(SectionPlayer2);
        choice("##p2device", PlayerTwoDevice, PlayerTwoDeviceHint, settings.player2_device,
               {{"gamepad", DeviceGamepad}, {"keyboard", DeviceKeyboard}, {"off", DeviceOff}});
        gamepad_row("##p2gamepad", PlayerTwoGamepad, settings.player2_gamepad, settings.player2_device == "gamepad");
        settings_section(SectionBindings);

        // Which set the list below changes.
        setting_row(tr(BindingSet), tr(BindingSetHint), combo_width, scale, [&] {
            const Text labels[4] = {SetOneKeyboard, SetOnePad, SetTwoKeyboard, SetTwoPad};
            ImGui::SetNextItemWidth(combo_width);
            if (ImGui::BeginCombo("##bindingset", tr(labels[binding_set]))) {
                for (int index = 0; index < 4; ++index) {
                    if (ImGui::Selectable(tr(labels[index]), index == binding_set)) {
                        binding_set = index;
                        capturing = -1;
                        ::play(sfr::UiSound::confirm);
                    }
                    if (index == binding_set) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        });
        const bool keys = binding_set % 2 == 0;
        sfr::InputBindings& player = bindings[binding_set / 2];
        ImGui::PushStyleColor(ImGuiCol_Text, dim_text);
        ImGui::PushTextWrapPos(content_width);
        ImGui::TextUnformatted(wrapped(tr(keys ? BindingsHint : SticksFixed), room_to(content_width)).c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 6 * scale));

        // A pad's sticks are positions rather than presses, so only the
        // buttons are offered for one.
        const size_t listed = keys ? sfr::input_action_count : size_t(sfr::InputAction::left_stick_up);
        for (size_t index = 0; index < listed; ++index) binding_row(sfr::InputAction(index), keys, player);

        if (small_button(tr(ResetBindings), scale)) {
            const sfr::InputBindings fresh = sfr::default_bindings(uint32_t(binding_set / 2));
            if (keys) player.key = fresh.key;
            else player.source = fresh.source;
            capturing = -1;
        }
        // A press while one is waiting binds it. Nav is off meanwhile, so
        // that the same press does not also move the focus.
        if (capture_frame && capturing >= 0 && capturing < int(sfr::input_action_count)) {
            const sfr::InputAction action = sfr::InputAction(capturing);
            if (keys) {
                if (const int key = captured_key()) {
                    player[action] = key;
                    capturing = -1;
                    ::play(sfr::UiSound::confirm);
                }
            } else if (const auto source = pad_capture.update(sfr::poll_pad_buttons())) {
                player.from(action) = *source;
                capturing = -1;
                ::play(sfr::UiSound::confirm);
            }
        }
        write_bindings();
    }

    // ------------------------------------------------ mods (mod_loader.h)
    sfr::ModList mod_list;
    bool mods_read = false;

    void mods_settings() {
        if (!mods_read) {
            std::error_code error;
            fs::create_directories(directory / "mods", error);
            mod_list = sfr::scan_mods(directory);
            mods_read = true;
        }
        settings_section(SectionMods);
        ImGui::PushStyleColor(ImGuiCol_Text, dim_text);
        ImGui::TextWrapped("%s", wrapped(tr(ModsHint), room()).c_str());
        ImGui::TextWrapped("%s", utf8(mod_list.folder).c_str());
        ImGui::PopStyleColor();
        if (small_button(tr(ModsRefresh), scale)) mods_read = false;
#ifndef __ANDROID__
        ImGui::SameLine();
        if (small_button(tr(ModsOpenFolder), scale)) sfr::launcher::open_url(utf8(mod_list.folder).c_str());
#endif
        if (mod_list.external) {
            ImGui::TextWrapped("%s", wrapped(tr(ModsExternal), room()).c_str());
            ImGui::TextWrapped("%s", utf8(mod_list.external_db).c_str());
            return;
        }
        if (mod_list.mods.empty()) {
            ImGui::TextWrapped("%s", tr(ModsEmpty));
            return;
        }
        bool changed = false;
        const float switch_width = ImGui::GetFrameHeight() * 1.9f;
        for (size_t i = 0; i < mod_list.mods.size(); ++i) {
            auto& mod = mod_list.mods[i];
            std::string hint = mod.author;
            if (!mod.version.empty()) hint += (hint.empty() ? "" : "  ") + std::string("v") + mod.version;
            if (!mod.description.empty()) hint += (hint.empty() ? "" : "\n") + mod.description;
            ImGui::PushID(int(i));
            setting_row(mod.title.c_str(), hint.c_str(), switch_width + 150 * scale, scale, [&] {
                if (toggle("##enabled", &mod.enabled)) changed = true;
                ImGui::SameLine();
                ImGui::BeginDisabled(i == 0);
                if (small_button(tr(MoveUp), scale)) {
                    std::swap(mod_list.mods[i], mod_list.mods[i - 1]);
                    changed = true;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(i + 1 == mod_list.mods.size());
                if (small_button(tr(MoveDown), scale)) {
                    std::swap(mod_list.mods[i], mod_list.mods[i + 1]);
                    changed = true;
                }
                ImGui::EndDisabled();
            });
            ImGui::PopID();
        }
        if (changed) sfr::save_mods(directory, mod_list);
    }

    void avatar_model_settings() {
        ImGui::PushID("avatar_model");
        ImGui::PushFont(fonts.heading);
        ImGui::TextUnformatted(tr(AvatarModel));
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, dim_text);
        ImGui::TextWrapped("%s", wrapped(tr(AvatarModelHint), room()).c_str());
        ImGui::PopStyleColor();
        if (focus_first_control) {
            focus_next();
            focus_first_control = false;
        }
        if (small_button(tr(Browse), scale)) {
            const auto start = settings.avatar_model.empty() ? directory : sfr::resolved_avatar_model(settings, directory);
            if (auto chosen = sfr::launcher::pick_path(start, false, sfr::launcher::FileFilter::AvatarModel)) {
#ifdef __ANDROID__
                // A document descriptor belongs to this launcher process.
                // Keep an app-local copy that GameActivity can reopen.
                const auto model = directory / "avatar-model.vrm";
                avatar_model_import_failed = !sfr::launcher::copy_picked_file(*chosen, model);
                if (!avatar_model_import_failed) settings.avatar_model = model;
#else
                settings.avatar_model = *chosen;
#endif
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(settings.avatar_model.empty() && !avatar_model_import_failed);
        if (small_button(tr(Clear), scale)) {
            settings.avatar_model.clear();
            avatar_model_import_failed = false;
        }
        ImGui::EndDisabled();
#ifndef __ANDROID__
        // Also allow a typed path on desktops without a native picker.
        char path[4096]{};
        utf8(settings.avatar_model).copy(path, sizeof path - 1);
        ImGui::SetNextItemWidth(content_width);
        if (ImGui::InputTextWithHint("##model_path", tr(AvatarModelNone), path, sizeof path))
            settings.avatar_model = fs::path(std::u8string(path, path + std::strlen(path)));
        if (ImGui::IsItemHovered() && !settings.avatar_model.empty())
            ImGui::SetTooltip("%s", utf8(sfr::resolved_avatar_model(settings, directory)).c_str());
#else
        ImGui::TextWrapped("%s", settings.avatar_model.empty() ? tr(AvatarModelNone) : utf8(settings.avatar_model).c_str());
#endif
        std::error_code error;
        const bool missing = !settings.avatar_model.empty() && !fs::is_regular_file(sfr::resolved_avatar_model(settings, directory), error);
        if (missing || avatar_model_import_failed) {
            ImGui::PushStyleColor(ImGuiCol_Text, warning_text);
            ImGui::TextWrapped("%s", wrapped(tr(avatar_model_import_failed ? AvatarModelImportFailed : AvatarModelMissing), room()).c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, 10 * scale));
        ImGui::PopID();
    }

    void motion_settings() {
        const float switch_width = ImGui::GetFrameHeight() * 1.9f;
        const float camera_width = 150 * scale;
        // The hint says what the chosen setting does, or while the list is
        // open the one under the pointer (from the frame before).
        static constexpr const char* camera_values[] = {"off", "picture", "motion", "kinect"};
        static constexpr Text camera_hints[] = {CameraHintOff, CameraHintPicture, CameraHint, CameraHintKinect};
        int camera_chosen = 0;
        for (int i = 0; i < 4; ++i) if (settings.camera == camera_values[i]) camera_chosen = i;
        const int camera_described = camera_hovered >= 0 ? camera_hovered : camera_chosen;
        setting_row(tr(CameraLabel), tr(camera_hints[camera_described]), camera_width, scale, [&] {
            const Text labels[] = {CameraOff, CameraPicture, CameraMotion, CameraKinect};
            // A real sensor only where there is a runtime to reach it.
            const int choices = sfr::KinectSensor::supported() ? 4 : 3;
            const int chosen = (std::min)(camera_chosen, choices - 1);
            camera_hovered = -1;
            ImGui::SetNextItemWidth(camera_width);
            if (ImGui::BeginCombo("##camera", tr(labels[chosen]))) {
                for (int i = 0; i < choices; ++i) {
                    if (ImGui::Selectable(tr(labels[i]), chosen == i)) settings.camera = camera_values[i];
                    if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) camera_hovered = i;
                }
                ImGui::EndCombo();
            }
        });
        // Which camera: the hosts's list, read once and again whenever the
        // camera is turned on, since one may have been plugged in since.
        // A Kinect is the host's only one: no list, only whether it answers.
        if (settings.camera == "kinect") {
            // What the sensor sees, in a window of its own: whether it works,
            // its cameras and skeletons, and its tilt.
            if (kinect_preview) {
                if (const std::string why = kinect_preview->failure(); !why.empty()) kinect_failure = why;
                if (kinect_preview->closed()) kinect_preview.reset();
            }
            const Text label = kinect_preview ? KinectPreviewClose : KinectPreviewOpen;
            const float button = (std::max)(ImGui::CalcTextSize(tr(KinectPreviewOpen)).x,
                                            ImGui::CalcTextSize(tr(KinectPreviewClose)).x) + ImGui::GetStyle().FramePadding.x * 4;
            setting_row(tr(KinectRow), tr(KinectRowHint), button, scale, [&] {
                if (ImGui::Button(tr(label), ImVec2(button, 0))) {
                    if (kinect_preview) kinect_preview.reset();
                    else {
                        kinect_failure.clear();
                        kinect_preview = sfr::KinectPreviewWindow::open(language == 1);
                    }
                }
            });
            // Anything short of a working sensor: what to install and check.
            if (!kinect_failure.empty()) {
                const Text why = kinect_failure == "no-runtime" || kinect_failure == "incomplete-runtime" ? KinectNoRuntime
                                 : kinect_failure == "no-sensor"                                           ? KinectNoSensor
                                                                                                           : KinectFailed;
                const float padding = ImGui::GetStyle().FramePadding.x * 4;
                const float v1_width = ImGui::CalcTextSize(tr(KinectDownload)).x + padding;
                const float v2_width = ImGui::CalcTextSize(tr(KinectDownloadV2)).x + padding;
                setting_row(tr(why), tr(KinectInstallHint),
                            v1_width + v2_width + ImGui::GetStyle().ItemSpacing.x, scale, [&] {
                    if (ImGui::Button(tr(KinectDownload), ImVec2(v1_width, 0)))
                        sfr::launcher::open_url("https://www.microsoft.com/download/details.aspx?id=40278");
                    ImGui::SameLine();
                    if (ImGui::Button(tr(KinectDownloadV2), ImVec2(v2_width, 0)))
                        sfr::launcher::open_url("https://www.microsoft.com/download/details.aspx?id=44561");
                });
            }
        } else if (settings.camera != "off") {
            if (cameras_listed_for != settings.camera) {
                cameras = sfr::CameraCapture::devices();
                cameras_listed_for = settings.camera;
            }
            const float test_width = ImGui::CalcTextSize(tr(CameraTesting)).x + ImGui::GetStyle().FramePadding.x * 4;
            const float device_width = 230 * scale + test_width;
            // The camera is remembered by name, so the row shows the saved
            // name even when that camera is not here today.
            const char* current = settings.camera_device.empty()
                                      ? (cameras.empty() ? tr(CameraNone) : cameras.front().c_str())
                                      : settings.camera_device.c_str();
            const Text trial[] = {CameraTest, CameraTesting, CameraWorks, CameraSilent, CameraClosed};
            setting_row(tr(CameraDevice), tr(CameraDeviceHint), device_width, scale, [&] {
                ImGui::SetNextItemWidth(device_width - test_width - ImGui::GetStyle().ItemSpacing.x);
                if (ImGui::BeginCombo("##camera_device", current)) {
                    for (const auto& camera : cameras)
                        if (ImGui::Selectable(camera.c_str(), camera == settings.camera_device)) {
                            settings.camera_device = camera;
                            camera_trial.store(CameraTrial::none);
                        }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                const auto state = camera_trial.load();
                ImGui::BeginDisabled(cameras.empty() || state == CameraTrial::looking);
                if (ImGui::Button(tr(trial[int(state)]), ImVec2(test_width, 0))) {
                    camera_trial.store(CameraTrial::looking);
                    camera_trial_worker = std::jthread([this, wanted = std::string(current)] {
                        auto camera = sfr::CameraCapture::open(640, 480, wanted);
                        if (!camera) { camera_trial.store(CameraTrial::closed); return; }
                        sfr::CameraFrame frame;
                        for (int attempt = 0; attempt < 300 && !frame.number; ++attempt)
                            if (!camera->next(frame)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        camera_trial.store(frame.number ? CameraTrial::pictures : CameraTrial::silent);
                    });
                }
                ImGui::EndDisabled();
            });
            if (settings.camera == "motion") {
                setting_row(tr(CameraMirror), tr(CameraMirrorHint), switch_width, scale,
                            [&] { toggle("##camera_mirror", &settings.camera_mirror); });
#ifdef _WIN32
                setting_row(tr(CameraDebug), tr(CameraDebugHint), switch_width, scale,
                            [&] { toggle("##camera_debug", &settings.camera_debug); });
#endif
            }
        }
        if (sfr::VoiceRecognizer::supported()) {
            settings_section(SectionVoice);
            setting_row(tr(VoiceLabel), tr(VoiceHint), switch_width, scale, [&] { toggle("##voice", &settings.voice); });
        }
    }

    void performance_settings() {
        const float switch_width = ImGui::GetFrameHeight() * 1.9f;
        setting_row(tr(VertexCache), tr(VertexCacheHint), switch_width, scale, [&] { toggle("##vertex", &settings.vertex_cache); });
        setting_row(tr(GpuPipeline), tr(GpuPipelineHint), switch_width, scale, [&] { toggle("##pipeline", &settings.gpu_pipeline); });
        const float slider_width = 220 * scale;
        setting_row(tr(RaceEvery), tr(RaceEveryHint), slider_width, scale, [&] {
            int every = int(settings.race_render_every);
            ImGui::SetNextItemWidth(slider_width);
            bool moved = ImGui::SliderInt("##every", &every, 1, 4);
            moved |= slider_steps(&every, 1, 4, 1);
            if (moved) settings.race_render_every = uint32_t(every);
        });
    }

    void reset_settings() {
        ImGui::Dummy(ImVec2(0, 6 * scale));
        if (small_button(tr(Defaults), scale)) {
            const auto image = settings.image_directory, assets = settings.asset_directory;
            settings = {};
            avatar_model_import_failed = false;
            settings.image_directory = image;
            settings.asset_directory = assets;
            ui_sounds.enabled = settings.ui_sounds;
            apply_language(settings.language);
            bindings_read = false;  // the Controls page reads them again
            capturing = -1;
        }
    }

    void directory_row(const char* label, const char* hint, fs::path& path, bool ready) {
        ImGui::PushID(label);
        ImGui::PushFont(fonts.heading);
        ImGui::TextUnformatted(label);
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ready ? ImVec4(0.45f, 0.90f, 0.55f, 1) : warning_text);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s  %s", ready ? "\xE2\x97\x8F" : "\xE2\x97\x8B", tr(ready ? Found : Missing));
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, dim_text);
        ImGui::TextUnformatted(hint);
        ImGui::PopStyleColor();
        // Only the status where paths cannot be browsed (Android: the
        // installer's folders, which the player has no reason to change).
        if (!sfr::launcher::can_pick_folders()) {
            ImGui::Dummy(ImVec2(0, 6 * scale));
            ImGui::PopID();
            return;
        }
        const float browse_width = ImGui::CalcTextSize(tr(Browse)).x + ImGui::GetStyle().FramePadding.x * 2;
        std::string text = utf8(path);
        ImGui::SetNextItemWidth(content_width - browse_width - ImGui::GetStyle().ItemSpacing.x);
        char buffer[1024]{};
        text.copy(buffer, sizeof buffer - 1);
        if (ImGui::InputText("##path", buffer, sizeof buffer))
            path = fs::path(std::u8string(buffer, buffer + std::strlen(buffer)));
        focus_glow(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetStyle().FrameRounding,
                   focus_amount(ImGui::IsItemFocused() && nav_visible()), scale);
        ImGui::SameLine();
        if (focus_first_control) {
            focus_next();
            focus_first_control = false;
        }
        if (sfr::launcher::can_pick_folders() && small_button(tr(Browse), scale))
            if (auto chosen = sfr::launcher::pick_path(path, true)) path = *chosen;
        ImGui::Dummy(ImVec2(0, 10 * scale));
        ImGui::PopID();
    }

    void file_settings() {
#ifdef __ANDROID__
        settings_section(SectionDiagnostics);
        ImGui::TextWrapped("%s", wrapped(tr(ExportDiagnosticsHint), room()).c_str());
        if (small_button(tr(ExportDiagnostics), scale)) sfr::launcher::export_diagnostics();
        ImGui::Dummy(ImVec2(0, 12 * scale));
#endif
        directory_row(tr(ImageDirectory), tr(ImageDirectoryHint), settings.image_directory,
                      sfr::is_image_directory(settings.image_directory));
        directory_row(tr(AssetDirectory), tr(AssetDirectoryHint), settings.asset_directory,
                      sfr::is_asset_directory(settings.asset_directory));
        ImGui::PushStyleColor(ImGuiCol_Text, dim_text);
        if (sfr::launcher::can_pick_folders()) ImGui::TextWrapped("%s", wrapped(tr(FilesHint), room()).c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 4 * scale));
        if (small_button(tr(sfr::launcher::can_pick_folders() ? InstallFromDisc : InstallFromImage), scale)) open_install_page();
        // Without the checkout's shader tools the game needs a pack.
        if (runtime_root.empty() && sfr::shader_tool_environment(directory).empty()) {
            ImGui::Dummy(ImVec2(0, 10 * scale));
            std::error_code error;
            const bool ready = fs::is_regular_file(directory / "shaders.pack", error);
            ImGui::PushFont(fonts.heading);
            ImGui::TextUnformatted(tr(ShaderPack));
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ready ? ImVec4(0.45f, 0.90f, 0.55f, 1) : warning_text);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s  %s", ready ? "\xE2\x97\x8F" : "\xE2\x97\x8B", tr(ready ? Found : Missing));
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, dim_text);
            ImGui::TextWrapped("%s", wrapped(tr(ShaderPackHint), room()).c_str());
            ImGui::PopStyleColor();
            if (small_button(tr(ChoosePack), scale)) {
                if (auto chosen = sfr::launcher::pick_path(directory, false)) {
                    const bool copied = sfr::launcher::copy_picked_file(*chosen, directory / "shaders.pack");
                    // This pack is the player's: the marker tells the Android
                    // activity to leave it alone, instead of replacing it with
                    // the APK's own at the next update.
                    if (copied) std::ofstream(directory / "shaders.pack.bundled", std::ios::trunc) << "chosen";
                    ::play(copied ? sfr::UiSound::confirm : sfr::UiSound::error);
                }
            }
        }
    }

    void settings_page(ImVec2 size) {
        // A status or warning goes under the panel, which shrinks for it.
        const char* note = status ? status : !files_ready() ? tr(MissingFiles) : !shaders_available() ? tr(runtime_root.empty() && !sfr::launcher::can_pick_folders() ? NoShaderPack : NoShaderTools) : nullptr;
        const float top = content_top(), bottom = content_bottom(size) - (note ? 30 * scale : 0.0f);
        const float list_width = 220 * scale, x = margin * scale + slide();
        ImGui::SetCursorPos(ImVec2(x, top));
        tab_list(ImVec2(list_width, bottom - top));

        const ImVec2 panel_at(x + list_width + 16 * scale, top);
        const ImVec2 panel_size(size.x - margin * scale * 2 - list_width - 16 * scale, bottom - top);
        tab_fade = approach(tab_fade, 1.0f, 12.0f);
        {
            // The panel, drawn: translucent, rounded, a lit edge along its top.
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 min(panel_at.x, panel_at.y), max(panel_at.x + panel_size.x, panel_at.y + panel_size.y);
            draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_ChildBg), 14 * scale);
            draw->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border), 14 * scale);
            draw->AddRectFilledMultiColor(ImVec2(min.x + 14 * scale, min.y), ImVec2(min.x + panel_size.x * 0.7f, min.y + 2 * scale),
                                          color(cyan, 0.8f), color(cyan, 0.0f), color(cyan, 0.0f), color(cyan, 0.8f));
        }
        const float pad_x = 30 * scale, pad_y = 24 * scale;
        // The rows scroll inside the panel: a tab with more of them than fit
        // used to draw straight through its rounded edge.
        const ImVec2 inside(panel_size.x - pad_x * 2, panel_size.y - pad_y * 2);
        ImGui::SetCursorPos(ImVec2(panel_at.x + pad_x + (1.0f - tab_fade) * 24 * scale, panel_at.y + pad_y));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * tab_fade);
        const ImGuiWindowFlags panel_flags = ImGuiWindowFlags_NoBackground |
            (tab == ControlsTab ? ImGuiWindowFlags_AlwaysVerticalScrollbar : ImGuiWindowFlags_None);
        ImGui::PushID(tab); // Each category retains its own scroll and widget state.
        // Flattened: the pad and the keyboard move between the rows and the
        // buttons around them as one page. A plain child is a single item to
        // them, entered only with A, and lit by no control while focused.
        ImGui::BeginChild("settings", inside, ImGuiChildFlags_NavFlattened, panel_flags);
        // Account for the scrollbar before laying out the rows.
        content_width = ImGui::GetContentRegionAvail().x;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_width);
        switch (tab) {
        case DisplayTab: display_settings(); break;
        case SoundTab: sound_settings(); break;
        case GeneralTab: general_settings(); break;
        case ControlsTab: controls_settings(); break;
        case MotionTab: motion_settings(); break;
        case AvatarTab: avatar_model_settings(); break;
        case ModsTab: mods_settings(); break;
        case FilesTab: file_settings(); break;
        }
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopID();
        ImGui::PopStyleVar();

        if (note) {
            ImGui::SetCursorPos(ImVec2(margin * scale, bottom + 8 * scale));
            ImGui::PushTextWrapPos(size.x - margin * scale);
            ImGui::TextColored(status && !status_error ? dim_text : !shaders_available() && !status && files_ready()
                                   ? ImVec4(1.0f, 0.75f, 0.35f, 1) : warning_text, "%s", note);
            ImGui::PopTextWrapPos();
        }
        guide(size, {{Prompt::confirm, GuideSelect}, {Prompt::tabs, GuideTabs}, {Prompt::back, GuideQuit}, {Prompt::start, GuidePlay}});
        const bool typing = ImGui::GetIO().WantTextInput;
        const int step = question != Question::none || typing || capture_frame ? 0
            : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) || ImGui::IsKeyPressed(ImGuiKey_PageUp, false) ? -1
            : ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) || ImGui::IsKeyPressed(ImGuiKey_PageDown, false) ? 1 : 0;
        if (step) {
            select_tab((tab + step + TabCount) % TabCount);
        }
        const int pressed = actions(size, {{tr(Quit), false}, {tr(StartGame), true}}, 1);
        if (pressed == 0) quit_now = true;
        if (pressed == 1) play();
        if (!capture_frame && ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) && question == Question::none) { ::play(sfr::UiSound::confirm); play(); }
    }

    void stopped_page(ImVec2 size) {
        page_heading(tr(Stopped), ImVec4(1.0f, 0.62f, 0.52f, 1));
        const float x = margin * scale + slide();
        ImGui::SetCursorPosX(x);
        ImGui::TextColored(dim_text, tr(StoppedDetail), exit_code);
        // The runtime names why it stopped on a line of its own.
        for (const auto& line : stopped_lines)
            if (line.rfind("STOP ", 0) == 0) {
                ImGui::SetCursorPosX(x);
                ImGui::PushTextWrapPos(size.x - margin * scale);
                ImGui::TextWrapped("%s", line.c_str());
                ImGui::PopTextWrapPos();
            }
        ImGui::SetCursorPosX(x);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20 * scale, 14 * scale));
        ImGui::BeginChild("log", ImVec2(size.x - margin * scale * 2, content_bottom(size) - ImGui::GetCursorPosY()),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 3 * scale));
        ImGui::PushStyleColor(ImGuiCol_Text, dim_text);
        for (const auto& line : stopped_lines) ImGui::TextUnformatted(line.c_str());
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        if (ImGui::IsWindowAppearing()) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        guide(size, {{Prompt::confirm, GuideSelect}, {Prompt::back, GuideBack}});
        const int pressed = sfr::launcher::can_open_log()
            ? actions(size, {{tr(OpenLog), false}, {tr(BackToSettings), true}}, 1)
            : actions(size, {{tr(BackToSettings), true}}, 0) + 1;
        if (pressed == 0 && sfr::launcher::can_open_log()) sfr::launcher::open_log(log_file);
        if (pressed == 1) { status = nullptr; set_page(Page::Settings); }
    }

    // ------------------------------------------------ install
    void open_install_page() {
        set_page(Page::Install);
        install_error.clear();
        install_cancelled = false;
    }

    void choose_source(bool folder) {
        const fs::path start = source.empty() ? directory : source;
        if (auto chosen = sfr::launcher::pick_path(start, folder)) use_source(*chosen);
    }

    void use_source(const fs::path& chosen) {
        source = chosen;
        summary = sfr::inspect_install_source(source);
        std::error_code error;
        fs::create_directories(game_directory(), error);
        available = free_bytes(error ? directory : game_directory());
        install_error.clear();
        install_cancelled = false;
        focus_start = true;
        ::play(source_installable() ? sfr::UiSound::confirm : sfr::UiSound::error);
    }

    bool source_installable() const {
        return summary.problem == sfr::SourceProblem::none && available >= sfr::install_space_needed(summary);
    }

    void start_install() {
        installation = std::make_unique<Installation>();
        Installation* running = installation.get();
        running->worker = std::thread([running, from = source, to = game_directory()] {
            try {
                sfr::install_game(from, to, [running](sfr::InstallStage stage, uint64_t done, uint64_t total,
                                                      const std::string& item) {
                    std::lock_guard hold(running->lock);
                    if (stage == sfr::InstallStage::copying && running->stage != stage)
                        running->copy_started = std::chrono::steady_clock::now();
                    running->stage = stage;
                    running->done = done;
                    running->total = total;
                    running->item = item;
                    return !running->cancel.load();
                });
            } catch (const sfr::InstallCancelled&) {
                std::lock_guard hold(running->lock);
                running->cancelled = true;
            } catch (const std::exception& e) {
                std::lock_guard hold(running->lock);
                running->error = e.what();
            }
            running->finished = true;
        });
        shown_progress = 0.0f;
        set_page(Page::Installing);
    }

    void finish_install() {
        installation->worker.join();
        const bool cancelled = installation->cancelled;
        const std::string error = installation->error;
        installation.reset();
        if (question == Question::cancel_install) question = Question::none;
        if (cancelled || !error.empty()) {
            set_page(Page::Install);
            install_cancelled = cancelled;
            install_error = error;
            ::play(cancelled ? sfr::UiSound::back : sfr::UiSound::error);
            return;
        }
        settings.image_directory = game_directory() / "image";
        settings.asset_directory = game_directory() / "assets";
        sfr::save_launcher_settings(settings_file, settings);
        set_page(Page::Installed);
        installed_at = ImGui::GetTime();
        ::play(sfr::UiSound::complete);
    }

    // Closing the launcher mid-install cancels it (the installer removes
    // its partial files).
    void abandon_install() {
        if (!installation) return;
        installation->cancel = true;
        installation->worker.join();
        installation.reset();
    }

    void install_page(ImVec2 size) {
        page_heading(tr(InstallTitle));
        const float x = margin * scale + slide();
        ImGui::SetCursorPosX(x);
        ImGui::PushTextWrapPos(size.x - margin * scale);
        ImGui::TextColored(dim_text, "%s", wrapped(tr(sfr::launcher::can_pick_folders() ? InstallIntro : InstallIntroFile),
                                                   room_to(size.x - margin * scale)).c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 6 * scale));
        ImGui::SetCursorPosX(x);
        const float pick_width = 260 * scale, pick_height = 46 * scale;
        if (source.empty() && focus_start && ImGui::GetFrameCount() > 2) {
            focus_next();
            focus_start = false;
        }
        if (big_button(tr(ChooseIso), ImVec2(pick_width, pick_height), false, scale)) choose_source(false);
        if (sfr::launcher::can_pick_folders()) {
            ImGui::SameLine(0, 16 * scale);
            if (big_button(tr(ChooseFolder), ImVec2(pick_width, pick_height), false, scale)) choose_source(true);
        }

        if (!source.empty()) {
            ImGui::Dummy(ImVec2(0, 8 * scale));
            ImGui::SetCursorPosX(x);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * scale, 16 * scale));
            ImGui::BeginChild("source", ImVec2(size.x - margin * scale * 2, 0),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY);
            const bool ok = summary.problem == sfr::SourceProblem::none;
            {
                const ImVec2 at = ImGui::GetWindowPos();
                const float h = ImGui::GetWindowSize().y;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x, at.y + 12 * scale), ImVec2(at.x + 4 * scale, at.y + h - 12 * scale),
                                                          color(ok ? ImVec4(0.45f, 0.90f, 0.55f, 1) : warning_text, 1.0f), 2 * scale);
            }
            ImGui::PushFont(fonts.heading);
            ImGui::TextUnformatted(tr(summary.kind == sfr::SourceKind::folder ? SourceFolder : SourceDisc));
            ImGui::PopFont();
            ImGui::PushStyleColor(ImGuiCol_Text, dim_text);
            ImGui::TextWrapped("%s", sfr::launcher::source_name(source).c_str());
            ImGui::PopStyleColor();
            const Text verdict = ok ? SourceReady
                : summary.problem == sfr::SourceProblem::wrong_game ? SourceWrongGame
                : summary.problem == sfr::SourceProblem::not_a_disc ? SourceNotADisc : SourceUnreadable;
            ImGui::TextColored(ok ? ImVec4(0.45f, 0.90f, 0.55f, 1) : warning_text, "%s  %s",
                               ok ? "\xE2\x97\x8F" : "\xE2\x97\x8B", tr(verdict));
            // Why, in the installer's words (e.g. the disc could not be read).
            if (!ok && !summary.detail.empty()) ImGui::TextColored(dim_text, "(%s)", summary.detail.c_str());
            if (ok) {
                ImGui::Text(tr(SourceFiles), summary.file_count, gigabytes(summary.total_bytes));
                ImGui::Dummy(ImVec2(0, 2 * scale));
                ImGui::TextColored(dim_text, "%s", tr(InstallDestination));
                ImGui::TextWrapped("%s", utf8(game_directory()).c_str());
                const uint64_t needed = sfr::install_space_needed(summary);
                ImGui::TextColored(available >= needed ? dim_text : warning_text, tr(FreeSpace),
                                   gigabytes(available), gigabytes(needed));
                if (available < needed) ImGui::TextColored(warning_text, "%s", tr(NotEnoughSpace));
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
        }
        if (install_cancelled || !install_error.empty()) {
            ImGui::Dummy(ImVec2(0, 4 * scale));
            ImGui::SetCursorPosX(x);
            ImGui::PushTextWrapPos(size.x - margin * scale);
            if (install_cancelled)
                ImGui::TextColored(dim_text, "%s", wrapped(tr(InstallCancelledText), room_to(size.x - margin * scale)).c_str());
            else ImGui::TextColored(warning_text, "%s %s", tr(InstallFailed), install_error.c_str());
            ImGui::PopTextWrapPos();
        }
        guide(size, {{Prompt::confirm, GuideSelect}, {Prompt::back, files_ready() ? GuideBack : GuideQuit}});
        const int pressed = actions(size, {{tr(Later), false}, {tr(Install), true}}, source.empty() ? -1 : 1, !source_installable());
        if (pressed == 0) set_page(Page::Settings);
        if (pressed == 1) start_install();
    }

    void installing_page(ImVec2 size) {
        sfr::InstallStage stage;
        uint64_t done, total;
        std::string item;
        std::chrono::steady_clock::time_point copy_started;
        {
            std::lock_guard hold(installation->lock);
            stage = installation->stage;
            done = installation->done;
            total = installation->total;
            item = installation->item;
            copy_started = installation->copy_started;
        }
        // Copying is most of the work; decoding the code takes the rest.
        float target = 0.0f;
        if (stage == sfr::InstallStage::copying) target = total ? 0.95f * float(double(done) / double(total)) : 0.0f;
        else if (stage == sfr::InstallStage::decoding) target = 0.96f;
        else if (stage == sfr::InstallStage::finishing) target = done ? 1.0f : 0.99f;
        shown_progress = approach(shown_progress, target, 10.0f);

        const Text label = stage == sfr::InstallStage::checking ? StageChecking
            : stage == sfr::InstallStage::copying ? StageCopying
            : stage == sfr::InstallStage::decoding ? StageDecoding : StageFinishing;
        page_heading(tr(label));
        ImGui::Dummy(ImVec2(0, 26 * scale));

        // The bar: a track of segments, the filled part lit with a light
        // sweeping through it and a bright head.
        const float x = margin * scale + slide();
        const float width = size.x - margin * scale * 2, height = 22 * scale;
        ImGui::SetCursorPosX(x);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(width, height));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float t = float(ImGui::GetTime());
        draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), IM_COL32(255, 255, 255, 18), height * 0.5f);
        const float filled = (std::max)(height, width * shown_progress);
        draw->AddRectFilled(at, ImVec2(at.x + filled, at.y + height), color(accent, 1.0f), height * 0.5f);
        draw->PushClipRect(at, ImVec2(at.x + filled, at.y + height), true);
        for (float s = at.x - height + std::fmod(t * 40 * scale, 18 * scale); s < at.x + filled; s += 18 * scale)
            draw->AddQuadFilled(ImVec2(s, at.y + height), ImVec2(s + 8 * scale, at.y + height), ImVec2(s + 8 * scale + height * 0.6f, at.y),
                                ImVec2(s + height * 0.6f, at.y), IM_COL32(255, 255, 255, 22));
        const float sweep = std::fmod(t * 0.6f, 1.0f) * (filled + 120 * scale) - 60 * scale;
        draw->AddRectFilledMultiColor(ImVec2(at.x + sweep, at.y), ImVec2(at.x + sweep + 60 * scale, at.y + height),
                                      IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 90),
                                      IM_COL32(255, 255, 255, 90), IM_COL32(255, 255, 255, 0));
        draw->PopClipRect();
        for (int ring = 3; ring >= 1; --ring)
            draw->AddCircleFilled(ImVec2(at.x + filled - height * 0.5f, at.y + height * 0.5f), height * 0.5f + ring * 4 * scale,
                                  color(cyan, 0.10f), 24);
        draw->AddCircleFilled(ImVec2(at.x + filled - height * 0.5f, at.y + height * 0.5f), height * 0.32f, IM_COL32(255, 255, 255, 230), 24);

        ImGui::Dummy(ImVec2(0, 10 * scale));
        ImGui::SetCursorPosX(x);
        ImGui::PushFont(fonts.title);
        ImGui::Text("%d%%", int(shown_progress * 100.0f + 0.5f));
        ImGui::PopFont();
        if (stage == sfr::InstallStage::copying && total) {
            ImGui::SameLine(0, 18 * scale);
            const float info_y = ImGui::GetCursorPosY() + 12 * scale;  // on the percentage's baseline
            ImGui::SetCursorPosY(info_y);
            ImGui::TextColored(dim_text, "%.2f / %.2f GB", gigabytes(done), gigabytes(total));
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - copy_started).count();
            if (seconds > 1.5 && done > 0) {
                const int left = int(seconds * double(total - done) / double(done) + 0.5);
                ImGui::SameLine(0, 24 * scale);
                ImGui::SetCursorPosY(info_y);
                ImGui::TextColored(dim_text, tr(Remaining), left / 60, left % 60);
            }
        }
        ImGui::SetCursorPosX(x);
        ImGui::TextColored(dim_text, "%s", item.c_str());

        guide(size, {{Prompt::back, GuideCancel}});
        ImGui::BeginDisabled(installation->cancel.load());
        const int pressed = actions(size, {{tr(Cancel), false}}, 0);
        ImGui::EndDisabled();
        if (pressed == 0) ask(Question::cancel_install);
        if (installation->finished) finish_install();
    }

    void installed_page(ImVec2 size) {
        // A tick in a ring that draws itself, and rings spreading out.
        const float since = float(ImGui::GetTime() - installed_at);
        const float x = margin * scale + slide();
        const ImVec2 centre(x + 34 * scale, content_top() + 60 * scale);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        for (int i = 0; i < 3; ++i) {
            const float p = std::clamp((since - i * 0.18f) / 1.2f, 0.0f, 1.0f);
            if (p > 0.0f && p < 1.0f)
                draw->AddCircle(centre, 30 * scale + p * 140 * scale, color(cyan, (1.0f - p) * 0.6f), 64, 3 * scale * (1.0f - p) + 1);
        }
        const float ring = ease_out(since / 0.5f);
        draw->PathArcTo(centre, 30 * scale, -1.5708f, -1.5708f + 6.2832f * ring, 48);
        draw->PathStroke(color(ImVec4(0.45f, 0.90f, 0.55f, 1), 1.0f), 0, 4 * scale);
        const float tick = ease_out((since - 0.3f) / 0.35f);
        if (tick > 0.0f) {
            const ImVec2 a(centre.x - 13 * scale, centre.y + 1 * scale), b(centre.x - 3 * scale, centre.y + 11 * scale),
                c(centre.x + 15 * scale, centre.y - 10 * scale);
            const float first = std::min(1.0f, tick * 2.0f), second = std::max(0.0f, tick * 2.0f - 1.0f);
            draw->AddLine(a, ImVec2(a.x + (b.x - a.x) * first, a.y + (b.y - a.y) * first), IM_COL32(255, 255, 255, 255), 4 * scale);
            if (second > 0.0f) draw->AddLine(b, ImVec2(b.x + (c.x - b.x) * second, b.y + (c.y - b.y) * second), IM_COL32(255, 255, 255, 255), 4 * scale);
        }
        ImGui::SetCursorPos(ImVec2(x + 90 * scale, content_top() + 34 * scale));
        ImGui::PushFont(fonts.heading);
        ImGui::TextUnformatted(tr(InstalledTitle));
        ImGui::PopFont();
        ImGui::SetCursorPosX(x + 90 * scale);
        ImGui::PushTextWrapPos(size.x - margin * scale);
        ImGui::TextColored(dim_text, "%s", wrapped(tr(InstalledText), room_to(size.x - margin * scale)).c_str());
        ImGui::PopTextWrapPos();
        guide(size, {{Prompt::confirm, GuideSelect}, {Prompt::back, GuideBack}});
        const int pressed = actions(size, {{tr(OpenSettings), false}, {tr(StartGame), true}}, 1);
        if (pressed == 0) { tab = GeneralTab; set_page(Page::Settings); }
        if (pressed == 1) { set_page(Page::Settings); play(); }
    }

    // ------------------------------------------------ frame
    void configure_navigation() {
        // Mouse tab changes, installer pages, and starting the game can hide
        // the waiting row. Cancel it before configuring the next frame's nav.
        // capture_frame stays latched through the frame that accepted a binding.
        if (page != Page::Settings || tab != ControlsTab || leaving) capturing = -1;
        ImGuiIO& io = ImGui::GetIO();
        // While a control is waiting to be bound, the keys and the pad mean
        // "this one" and nothing else: navigation would otherwise take the
        // same press and move the focus away.
        if (capturing >= 0) io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
        else io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    }

    void frame() {
        ImGuiIO& io = ImGui::GetIO();
        input = last_input(input);
        capture_frame = capturing >= 0;
        appear = leaving ? approach(appear, 0.0f, 9.0f) : approach(appear, 1.0f, 4.5f);
        page_fade = approach(page_fade, 1.0f, 7.0f);
        if (leaving && appear < 0.03f) launch_now();
        draw_backdrop(ImGui::GetBackgroundDrawList(), io.DisplaySize, float(ImGui::GetTime()), scale);

        const Text bar = page == Page::Settings ? BarSettings : page == Page::Stopped ? BarStopped
                       : page == Page::Installing ? BarInstalling : BarInstaller;
        bars(io.DisplaySize, tr(bar), page == Page::Installing, language_button_width());

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ease_out(appear) * (0.25f + 0.75f * ease_out(page_fade)));
        ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
        language_button(io.DisplaySize);
        switch (page) {
        case Page::Settings: settings_page(io.DisplaySize); break;
        case Page::Stopped: stopped_page(io.DisplaySize); break;
        case Page::Install: install_page(io.DisplaySize); break;
        case Page::Installing: installing_page(io.DisplaySize); break;
        case Page::Installed: installed_page(io.DisplaySize); break;
        }
        const bool asking = question != Question::none;
        question_box(io.DisplaySize);
        // B or Escape goes back when nothing else (an open list, a text
        // field, a question) takes it.
        // (and Android's back button)
        const bool back_pressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                                  ImGui::IsKeyPressed(ImGuiKey_AppBack, false);
        if (!capture_frame && back_pressed && !asking && question == Question::none && !leaving && !ImGui::IsAnyItemActive() &&
            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
            back();
        ImGui::End();
        ImGui::PopStyleVar();

        // A soft tick whenever the keyboard or pad moves the focus.
        const ImGuiID nav = GImGui->NavId;
        if (nav != last_nav && last_nav != 0 && nav != 0 && nav_visible() && !focus_start) ::play(sfr::UiSound::move);
        last_nav = nav;
    }
};

}

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    Launcher launcher;
    sounds = &launcher.ui_sounds;
    launcher.directory = sfr::launcher::launcher_directory();
    sfr::launcher::register_install(launcher.directory);
    launcher.settings_file = launcher.directory / L"settings.ini";
    launcher.log_file = launcher.directory / L"game.log";
    launcher.runtime_root = sfr::find_runtime_root(launcher.directory);
    launcher.settings = sfr::load_launcher_settings(launcher.settings_file);
    apply_language(launcher.settings.language);
    launcher.ui_sounds.enabled = launcher.settings.ui_sounds;
    if (launcher.settings.image_directory.empty())
        launcher.settings.image_directory = sfr::default_image_directory(launcher.directory);
    if (launcher.settings.asset_directory.empty())
        launcher.settings.asset_directory = sfr::default_asset_directory(launcher.directory);
    // First run (or the files are gone): the installer comes first.
    if (!launcher.files_ready()) launcher.page = Page::Install;
    launcher.desktop_width = uint32_t(GetSystemMetrics(SM_CXSCREEN));
    launcher.desktop_height = uint32_t(GetSystemMetrics(SM_CYSCREEN));

    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_CLASSDC;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    // The program's icon (src/app_icon.rc), or the drawn one without it.
    auto icon = [](int size) {
        if (HANDLE loaded = LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, 0))
            return static_cast<HICON>(loaded);
        return make_icon(size);
    };
    window_class.hIcon = icon(GetSystemMetrics(SM_CXICON));
    window_class.hIconSm = icon(GetSystemMetrics(SM_CXSMICON));
    window_class.lpszClassName = L"SfrLauncher";
    RegisterClassExW(&window_class);

    // Sized for the monitor's scale, centred on it.
    const POINT origin{0, 0};
    const HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    const UINT dpi = GetDpiForSystem();
    launcher.scale = float(dpi) / 96.0f;
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT bounds{0, 0, LONG(1120 * launcher.scale), LONG(700 * launcher.scale)};
    AdjustWindowRectExForDpi(&bounds, style, FALSE, 0, dpi);
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    const RECT& work = info.rcWork;
    HWND window = CreateWindowExW(0, window_class.lpszClassName, L"Free Riders Recompiled", style,
                                  work.left + (work.right - work.left - width) / 2,
                                  work.top + (work.bottom - work.top - height) / 2, width, height,
                                  nullptr, nullptr, instance, nullptr);
    if (!window || !create_device(window)) {
        MessageBoxW(nullptr, L"Direct3D 11 is not available.", L"Free Riders Recompiled", MB_ICONERROR);
        return 1;
    }
    sfr::launcher::set_owner_window(window);
    launcher.show_window = [window](bool shown) {
        ShowWindow(window, shown ? SW_SHOW : SW_HIDE);
        if (shown) SetForegroundWindow(window);
    };
    DragAcceptFiles(window, TRUE);
    // No input method: a Chinese or Japanese IME would take Q, E and the
    // other letter keys before the launcher sees them.
    ImmAssociateContextEx(window, nullptr, 0);
    if (int count = 0; LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count)) {
        if (count >= 2) dropped = fs::path(arguments[1]);
        LocalFree(arguments);
    }
    ShowWindow(window, show);
    UpdateWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    apply_style(launcher.scale);
    launcher.fonts = load_fonts(launcher.scale);
    apply_language(launcher.settings.language);  // Chinese needs a font for it
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(device, context);

    bool running = true;
    while (running) {
        if (launcher.game) {
            // Hidden while the game runs: sleep until it ends.
            HANDLE game = sfr::launcher::wait_handle(*launcher.game);
            if (MsgWaitForMultipleObjects(1, &game, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 &&
                !launcher.game_ended())
                running = false;
        }
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) running = false;
        }
        if (!running || launcher.game) continue;
        if (close_clicked) {
            close_clicked = false;
            launcher.close_requested();
            if (launcher.quit_now) {
                launcher.quit_now = false;
                DestroyWindow(window);
            }
            continue;
        }
        if (dropped && launcher.page != Page::Installing) {
            launcher.open_install_page();
            launcher.use_source(*dropped);
            dropped.reset();
        }
        if (IsIconic(window)) { Sleep(16); continue; }

        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        feed_sony_pad();
        launcher.configure_navigation();
        ImGui::NewFrame();
        launcher.frame();
        trace_navigation();
        ImGui::Render();
        constexpr float clear[4] = {0, 0, 0, 1};
        context->OMSetRenderTargets(1, &target, nullptr);
        context->ClearRenderTargetView(target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        // SFR_LAUNCHER_SHOTS=<directory> and SFR_LAUNCHER_SHOT_EVERY=<frames>:
        // the window every so many frames, as shot-<frame>.bmp (for unattended
        // tests, with SFR_LAUNCHER_INPUT_SCRIPT).
        if (static const char* shots = std::getenv("SFR_LAUNCHER_SHOTS"); shots && *shots) {
            static const int every = [] { const char* t = std::getenv("SFR_LAUNCHER_SHOT_EVERY"); return t ? std::max(1, std::atoi(t)) : 60; }();
            if (ImGui::GetFrameCount() % every == 0) save_back_buffer(std::string(shots) + "/shot-" + std::to_string(ImGui::GetFrameCount()) + ".bmp");
        }
        swap_chain->Present(1, 0);
        if (launcher.quit_now) {
            launcher.quit_now = false;
            DestroyWindow(window);
        }
    }
    launcher.abandon_install();
    sfr::save_launcher_settings(launcher.settings_file, launcher.settings);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroy_device();
    DestroyWindow(window);
    CoUninitialize();
    return 0;
}
#else
// Linux and Android: an SDL window drawn with SDL's renderer. On Android the
// window fills the screen and the layout scales to it; SDL_main is the entry.
int main(int argc, char** argv) {
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");  // back is the launcher's own Back
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL is not available: %s\n", SDL_GetError());
        return 1;
    }

    Launcher launcher;
    sounds = &launcher.ui_sounds;
    launcher.directory = sfr::launcher::launcher_directory();
    launcher.settings_file = launcher.directory / "settings.ini";
    launcher.log_file = launcher.directory / "game.log";
    launcher.runtime_root = sfr::find_runtime_root(launcher.directory);
    launcher.settings = sfr::load_launcher_settings(launcher.settings_file);
    apply_language(launcher.settings.language);
    launcher.ui_sounds.enabled = launcher.settings.ui_sounds;
    if (launcher.settings.image_directory.empty())
        launcher.settings.image_directory = sfr::default_image_directory(launcher.directory);
    if (launcher.settings.asset_directory.empty())
        launcher.settings.asset_directory = sfr::default_asset_directory(launcher.directory);
    if (!launcher.files_ready()) launcher.page = Page::Install;
    SDL_DisplayMode desktop{};
    if (SDL_GetDesktopDisplayMode(0, &desktop) == 0) {
        launcher.desktop_width = uint32_t(desktop.w);
        launcher.desktop_height = uint32_t(desktop.h);
    }
    if (argc >= 2) dropped = fs::path(argv[1]);

#ifdef __ANDROID__
    SDL_Window* window = SDL_CreateWindow("Free Riders Recompiled", 0, 0, 0, 0,
                                          SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI);
#else
    // Sized for the display's scale (as the Windows launcher is), centred.
    float dpi = 96.0f;
    if (SDL_GetDisplayDPI(0, nullptr, &dpi, nullptr) != 0 || dpi < 96.0f) dpi = 96.0f;
    launcher.scale = std::min(dpi / 96.0f, 2.0f);
    SDL_Window* window = SDL_CreateWindow("Free Riders Recompiled", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          int(1120 * launcher.scale), int(700 * launcher.scale), SDL_WINDOW_ALLOW_HIGHDPI);
#endif
    if (!window) {
        std::fprintf(stderr, "No launcher window: %s\n", SDL_GetError());
        return 1;
    }
    {
        // The launcher's icon, drawn (render_icon gives 0xAARRGGBB pixels).
        auto pixels = sfr::render_icon(64);
        if (SDL_Surface* icon = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), 64, 64, 32, 64 * 4, SDL_PIXELFORMAT_ARGB8888)) {
            SDL_SetWindowIcon(window, icon);
            SDL_FreeSurface(icon);
        }
    }
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        std::fprintf(stderr, "No renderer: %s\n", SDL_GetError());
        return 1;
    }
#ifdef __ANDROID__
    {
        // The whole screen: the 1120 x 700 layout scaled to fit it.
        int width = 0, height = 0;
        SDL_GetRendererOutputSize(renderer, &width, &height);
        launcher.scale = std::max(1.0f, std::min(float(width) / 1120.0f, float(height) / 700.0f));
    }
#endif
    launcher.show_window = [window](bool shown) {
#ifndef __ANDROID__  // there the game is another activity in front
        if (shown) { SDL_ShowWindow(window); SDL_RaiseWindow(window); }
        else SDL_HideWindow(window);
#else
        (void)window;
        (void)shown;
#endif
    };

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    apply_style(launcher.scale);
    launcher.fonts = load_fonts(launcher.scale);
    apply_language(launcher.settings.language);  // Chinese needs a font for it
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);
    {
        SDL_RendererInfo info{};
        SDL_GetRendererInfo(renderer, &info);
        unsigned char* pixels = nullptr;
        int atlas_width = 0, atlas_height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &atlas_width, &atlas_height);
        SDL_Log("launcher renderer=%s max_texture=%dx%d font_atlas=%dx%d scale=%.2f", info.name, info.max_texture_width,
                info.max_texture_height, atlas_width, atlas_height, launcher.scale);
    }

    bool running = true;
    while (running) {
        if (launcher.game) {
            // While the game runs: wait for it, keeping the window answered.
            SDL_Event event;
            while (SDL_PollEvent(&event)) sfr::launcher::handle_event(&event);
            if (launcher.game->exit_code()) {
                if (!launcher.game_ended()) running = false;
            } else {
                SDL_Delay(50);
            }
            continue;
        }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            sfr::launcher::handle_event(&event);
            if (event.type == SDL_QUIT ||
                (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE))
                close_clicked = true;
            if (event.type == SDL_DROPFILE) {
                dropped = fs::path(event.drop.file);
                SDL_free(event.drop.file);
            }
        }
        if (close_clicked) {
            close_clicked = false;
            launcher.close_requested();
            if (launcher.quit_now) running = false;
            continue;
        }
        if (dropped && launcher.page != Page::Installing) {
            launcher.open_install_page();
            launcher.use_source(*dropped);
            dropped.reset();
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) { SDL_Delay(16); continue; }

        // SDL still requires this flag to poll buttons for binding capture.
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        feed_sony_pad();
        launcher.configure_navigation();
        ImGui::NewFrame();
        launcher.frame();
        trace_navigation();
        ImGui::Render();
        SDL_RenderSetScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        // SFR_LAUNCHER_SCREENSHOT=<file.bmp>: the 120th frame, once the page
        // has slid in (a debugging aid).
        if (static const char* shot = std::getenv("SFR_LAUNCHER_SCREENSHOT"); shot && ImGui::GetFrameCount() == 120) {
            int width = 0, height = 0;
            SDL_GetRendererOutputSize(renderer, &width, &height);
            if (SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888)) {
                if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, surface->pixels, surface->pitch) == 0)
                    SDL_SaveBMP(surface, shot);
                SDL_FreeSurface(surface);
            }
        }
        SDL_RenderPresent(renderer);
        // SFR_LAUNCHER_AUTOPLAY=1: press Start at the 150th frame (for unattended tests).
        if (static const char* autoplay = std::getenv("SFR_LAUNCHER_AUTOPLAY");
            autoplay && *autoplay == '1' && ImGui::GetFrameCount() == 150 && launcher.page == Page::Settings)
            launcher.play();
        if (launcher.quit_now) running = false;
    }
    launcher.abandon_install();
    sfr::save_launcher_settings(launcher.settings_file, launcher.settings);

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
#ifdef __ANDROID__
    // A fresh process next time (the activity would otherwise reuse this one).
    _exit(0);
#endif
    return 0;
}
#endif
