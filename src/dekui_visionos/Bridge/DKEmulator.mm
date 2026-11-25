// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#import "dekui_visionos/Bridge/DKEmulator.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <execinfo.h>
#include <fcntl.h>
#include <filesystem>
#include <mach/mach.h>
#include <os/proc.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <chrono>
#include <unistd.h>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#include <QtCore/qnamespace.h>
#include <fmt/format.h>
#include "citra_qt/configuration/config.h"
#include "common/common_paths.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/log.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/arm/aot/runtime.h"
#include "core/jit_policy.h"
#include "core/perf_stats.h"
#include "core/frontend/applets/default_applets.h"
#include "core/loader/loader.h"
#include "dekui_frontend/bootstrap.h"
#include "dekui_frontend/emu_thread.h"
#include "dekui_frontend/input.h"
#import "dekui_visionos/Bridge/DKEmulator+SaveStates.h"
#include "mm3d/wide_view.h"
#include <imgui.h>
#include "mm3d/notice.h"
#include "dekui_frontend/poller.h"
#include "dekui_visionos/Bridge/emu_window_apple.h"
#include "dekui_visionos/Bridge/cloudkit_states.h"
#include "dekui_visionos/Bridge/icloud_shader_cache.h"
#include "dekui_visionos/Bridge/icloud_states.h"
#include "dekui_visionos/Bridge/software_keyboard.h"
#include "dekui_visionos/Bridge/stereo_frames.h"
#include "input_common/keyboard.h"
#include "input_common/main.h"
#include "mm3d/diagnostics.h"
#include "mm3d/player_settings.h"
#include "network/network.h"
#include "video_core/custom_textures/custom_tex_manager.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_vulkan/vk_external_present.h"
#ifdef CITRA_VISIONOS
#include <oaknut/apple_jit26.hpp>
#endif

namespace {

constexpr std::size_t NumButtons = Settings::NativeButton::NumButtons;

NSString* ToNS(const std::string& text) {
    return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

// The keyboard engine uses Qt key codes so that keyboard bindings in
// qt-config.ini stay valid between the desktop app and this one.
int QtKeyFromHID(UIKeyboardHIDUsage usage) {
    if (usage >= UIKeyboardHIDUsageKeyboardA && usage <= UIKeyboardHIDUsageKeyboardZ) {
        return Qt::Key_A + int(usage - UIKeyboardHIDUsageKeyboardA);
    }
    if (usage >= UIKeyboardHIDUsageKeyboard1 && usage <= UIKeyboardHIDUsageKeyboard9) {
        return Qt::Key_1 + int(usage - UIKeyboardHIDUsageKeyboard1);
    }
    if (usage >= UIKeyboardHIDUsageKeyboardF1 && usage <= UIKeyboardHIDUsageKeyboardF12) {
        return Qt::Key_F1 + int(usage - UIKeyboardHIDUsageKeyboardF1);
    }
    switch (usage) {
    case UIKeyboardHIDUsageKeyboard0:
        return Qt::Key_0;
    case UIKeyboardHIDUsageKeyboardReturnOrEnter:
        return Qt::Key_Return;
    case UIKeyboardHIDUsageKeypadEnter:
        return Qt::Key_Enter;
    case UIKeyboardHIDUsageKeyboardEscape:
        return Qt::Key_Escape;
    case UIKeyboardHIDUsageKeyboardDeleteOrBackspace:
        return Qt::Key_Backspace;
    case UIKeyboardHIDUsageKeyboardDeleteForward:
        return Qt::Key_Delete;
    case UIKeyboardHIDUsageKeyboardTab:
        return Qt::Key_Tab;
    case UIKeyboardHIDUsageKeyboardSpacebar:
        return Qt::Key_Space;
    case UIKeyboardHIDUsageKeyboardHome:
        return Qt::Key_Home;
    case UIKeyboardHIDUsageKeyboardEnd:
        return Qt::Key_End;
    case UIKeyboardHIDUsageKeyboardLeftArrow:
        return Qt::Key_Left;
    case UIKeyboardHIDUsageKeyboardRightArrow:
        return Qt::Key_Right;
    case UIKeyboardHIDUsageKeyboardUpArrow:
        return Qt::Key_Up;
    case UIKeyboardHIDUsageKeyboardDownArrow:
        return Qt::Key_Down;
    case UIKeyboardHIDUsageKeyboardLeftShift:
    case UIKeyboardHIDUsageKeyboardRightShift:
        return Qt::Key_Shift;
    case UIKeyboardHIDUsageKeyboardLeftControl:
    case UIKeyboardHIDUsageKeyboardRightControl:
        return Qt::Key_Control;
    case UIKeyboardHIDUsageKeyboardLeftAlt:
    case UIKeyboardHIDUsageKeyboardRightAlt:
        return Qt::Key_Alt;
    case UIKeyboardHIDUsageKeyboardComma:
        return Qt::Key_Comma;
    case UIKeyboardHIDUsageKeyboardPeriod:
        return Qt::Key_Period;
    case UIKeyboardHIDUsageKeyboardSlash:
        return Qt::Key_Slash;
    case UIKeyboardHIDUsageKeyboardSemicolon:
        return Qt::Key_Semicolon;
    case UIKeyboardHIDUsageKeyboardHyphen:
        return Qt::Key_Minus;
    case UIKeyboardHIDUsageKeyboardEqualSign:
        return Qt::Key_Equal;
    default:
        return 0;
    }
}

Dekui::Input::Key DekuiKey(int qt_key) {
    using Dekui::Input::Key;
    switch (qt_key) {
    case Qt::Key_F1:
        return Key::F1;
    case Qt::Key_F2:
        return Key::F2;
    case Qt::Key_Backspace:
        return Key::Backspace;
    case Qt::Key_Delete:
        return Key::Delete;
    case Qt::Key_Left:
        return Key::Left;
    case Qt::Key_Right:
        return Key::Right;
    case Qt::Key_Home:
        return Key::Home;
    case Qt::Key_End:
        return Key::End;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return Key::Enter;
    case Qt::Key_Escape:
        return Key::Escape;
    case Qt::Key_Tab:
        return Key::Tab;
    default:
        if (qt_key >= Qt::Key_A && qt_key <= Qt::Key_Z) {
            return Key(int(Key::A) + (qt_key - Qt::Key_A));
        }
        return Key::Other;
    }
}

std::string KeyName(int qt_key) {
    if ((qt_key >= Qt::Key_A && qt_key <= Qt::Key_Z) ||
        (qt_key >= Qt::Key_0 && qt_key <= Qt::Key_9)) {
        return std::string(1, char(qt_key));
    }
    if (qt_key >= Qt::Key_F1 && qt_key <= Qt::Key_F12) {
        return fmt::format("F{}", qt_key - Qt::Key_F1 + 1);
    }
    switch (qt_key) {
    case Qt::Key_Up:
        return "Up";
    case Qt::Key_Down:
        return "Down";
    case Qt::Key_Left:
        return "Left";
    case Qt::Key_Right:
        return "Right";
    case Qt::Key_Space:
        return "Space";
    case Qt::Key_Return:
        return "Return";
    case Qt::Key_Enter:
        return "Enter";
    case Qt::Key_Tab:
        return "Tab";
    case Qt::Key_Shift:
        return "Shift";
    case Qt::Key_Control:
        return "Control";
    case Qt::Key_Alt:
        return "Option";
    case Qt::Key_Backspace:
        return "Delete";
    case Qt::Key_Comma:
        return ",";
    case Qt::Key_Period:
        return ".";
    case Qt::Key_Slash:
        return "/";
    case Qt::Key_Semicolon:
        return ";";
    case Qt::Key_Minus:
        return "-";
    case Qt::Key_Equal:
        return "=";
    default:
        return qt_key ? fmt::format("Key {:#x}", qt_key) : "";
    }
}

std::string ButtonText(const Common::ParamPackage& param) {
    if (param.Get("engine", "") == "keyboard") {
        return KeyName(param.Get("code", 0));
    }
    return InputCommon::ButtonToText(param);
}

std::string AnalogText(const Common::ParamPackage& param) {
    if (!param.Has("engine")) {
        return "";
    }
    if (param.Get("engine", "") == "analog_from_button") {
        // Per-direction bindings are listed in their own rows.
        return "By direction";
    }
    // A whole stick: "Left Stick X" and "Left Stick Y" name the "Left Stick".
    auto name = param.Get("name_x", "");
    if (name.size() > 2 && name.ends_with(" X")) {
        return name.substr(0, name.size() - 2);
    }
    if (!name.empty()) {
        return name + " / " + param.Get("name_y", "");
    }
    return fmt::format("Axes {} / {}", param.Get("axis_x", 0), param.Get("axis_y", 1));
}

constexpr std::array<const char*, 4> StickDirectionKeys{"up", "down", "left", "right"};

// Bindings to controllers carry the profile's mapping type, like Qt's ConfigureInput.
void ApplyMaptype(Common::ParamPackage& param) {
    if (param.Get("engine", "keyboard") != "sdl") {
        param.Erase("maptype");
        return;
    }
    switch (Settings::values.current_input_profile.maptype) {
    case Settings::InputMappingType::AllControllers:
        param.Set("maptype", "all");
        break;
    case Settings::InputMappingType::Guid:
        param.Set("maptype", "guid");
        break;
    default:
        param.Set("maptype", "guid+port");
        break;
    }
}

std::string StageMessage(VideoCore::LoadCallbackStage stage, const std::string& name,
                         std::size_t value, std::size_t total) {
    switch (stage) {
    case VideoCore::LoadCallbackStage::Prepare:
        return "Preparing";
    case VideoCore::LoadCallbackStage::Preload:
        return "Loading textures";
    case VideoCore::LoadCallbackStage::Decompile:
    case VideoCore::LoadCallbackStage::Build:
        // The pipeline builds of the disk shader cache take minutes when the
        // driver has none of them yet (first start, or a shader cache that
        // was just imported); say so, with a count.
        if (name == "Pipeline" && total) {
            return fmt::format("Building shaders: {} of {}\nThe first start after an update or "
                               "a shader cache import takes a few minutes.",
                               value, total);
        }
        return "Loading shaders";
    case VideoCore::LoadCallbackStage::Complete:
        return "Starting";
    }
    return "";
}

// MARK: Crash diagnostics

// visionOS relaunches the app after it crashes, and the relaunch rotates the
// crashed run's log away. Keep copies of previous logs, and on a fatal signal
// write the faulting thread's stack to user/log/crash-<time>.txt.
constexpr std::size_t KeptLogs = 8;

std::string TimeStamp(std::time_t time) {
    char text[32];
    std::tm parts{};
    localtime_r(&time, &parts);
    std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &parts);
    return text;
}

/// Copies the previous run's log into log/history before logging starts.
void KeepPreviousLog() {
    namespace fs = std::filesystem;
    std::error_code error;
    const fs::path directory = FileUtil::GetUserPath(FileUtil::UserPath::LogDir);
    const fs::path log = directory / LOG_FILE;
    struct stat info {};
    if (stat(log.c_str(), &info) != 0 || info.st_size == 0) {
        return;
    }
    const fs::path history = directory / "history";
    fs::create_directories(history, error);
    fs::copy_file(log, history / ("azahar_log-" + TimeStamp(info.st_mtime) + ".txt"),
                  fs::copy_options::overwrite_existing, error);
    std::vector<fs::path> kept;
    for (const auto& entry : fs::directory_iterator(history, error)) {
        kept.push_back(entry.path());
    }
    std::sort(kept.begin(), kept.end()); // Time-stamped names sort by age.
    for (std::size_t i = 0; i + KeptLogs < kept.size(); ++i) {
        fs::remove(kept[i], error);
    }
}

// Prepared before any crash: the handler may only use async-signal-safe calls.
char crash_path[1024];
char crash_header[256];

void WriteText(int fd, const char* text) {
    (void)!write(fd, text, std::strlen(text));
}

void WriteNumber(int fd, std::uintptr_t value, unsigned base) {
    char digits[2 + 64];
    char* end = digits + sizeof(digits);
    char* at = end;
    do {
        *--at = "0123456789abcdef"[value % base];
        value /= base;
    } while (value);
    if (base == 16) {
        *--at = 'x';
        *--at = '0';
    }
    (void)!write(fd, at, std::size_t(end - at));
}

void HandleFatalSignal(int signal_number, siginfo_t* info, void* context) {
    const int fd = open(crash_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        WriteText(fd, crash_header);
        WriteText(fd, "signal ");
        WriteNumber(fd, std::uintptr_t(signal_number), 10);
        WriteText(fd, " (");
        WriteText(fd, strsignal(signal_number));
        WriteText(fd, ") at address ");
        WriteNumber(fd, std::uintptr_t(info ? info->si_addr : nullptr), 16);
#if defined(__arm64__)
        if (const auto* state = static_cast<ucontext_t*>(context); state && state->uc_mcontext) {
            WriteText(fd, "\npc ");
            WriteNumber(fd, std::uintptr_t(state->uc_mcontext->__ss.__pc), 16);
            WriteText(fd, " lr ");
            WriteNumber(fd, std::uintptr_t(state->uc_mcontext->__ss.__lr), 16);
        }
#endif
        WriteText(fd, "\nthread stack:\n");
        void* frames[96];
        backtrace_symbols_fd(frames, backtrace(frames, 96), fd);
        close(fd);
    }
    // Let the system record and handle the crash as usual.
    signal(signal_number, SIG_DFL);
    raise(signal_number);
}

void InstallCrashHandler(bool jit) {
    const std::string path = FileUtil::GetUserPath(FileUtil::UserPath::LogDir) + "crash-" +
                             TimeStamp(std::time(nullptr)) + ".txt";
    std::snprintf(crash_path, sizeof(crash_path), "%s", path.c_str());
    std::snprintf(crash_header, sizeof(crash_header),
                  "Dekui crash (launched %s, CPU %s)\n", TimeStamp(std::time(nullptr)).c_str(),
                  jit ? "JIT" : "interpreter");
    struct sigaction action {};
    action.sa_sigaction = HandleFatalSignal;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    // SIGTRAP is not handled: the JIT26 handshake uses breakpoints.
    for (const int signal_number : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) {
        sigaction(signal_number, &action, nullptr);
    }
}

/// Once a second, records the app's memory footprint, the memory it may still
/// use before the system kills it, whether a debugger is attached, emulation
/// speed and frame rate, and the busiest threads' CPU use, to
/// user/log/memory.txt. Written unbuffered so the last sample survives a kill.
void StartVitalsLog(bool jit) {
    const std::string directory = FileUtil::GetUserPath(FileUtil::UserPath::LogDir);
    const std::string path = directory + "memory.txt";
    std::rename(path.c_str(), (directory + "memory.old.txt").c_str());
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return;
    }
    std::thread([fd, jit] {
        pthread_setname_np("DekuiVitals");
        const auto start = std::chrono::steady_clock::now();
        const std::string header =
            fmt::format("# seconds footprint_mb available_mb traced speed% game_fps "
                        "swap_ms gpu_ms | busiest threads cpu% (CPU {})\n",
                        jit ? "JIT" : "interpreter");
        (void)!write(fd, header.data(), header.size());
        std::map<u64, double> previous_cpu; // thread id -> CPU seconds
        auto previous_time = std::chrono::steady_clock::now();
        for (;;) {
            task_vm_info_data_t info{};
            mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
            const bool have_info = task_info(mach_task_self(), TASK_VM_INFO,
                                             reinterpret_cast<task_info_t>(&info), &count) ==
                                   KERN_SUCCESS;
            int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
            kinfo_proc process{};
            std::size_t size = sizeof(process);
            const bool traced = sysctl(mib, 4, &process, &size, nullptr, 0) == 0 &&
                                (process.kp_proc.p_flag & P_TRACED) != 0;

            auto& system = Core::System::GetInstance();
            Core::PerfStats::Results perf{};
            if (system.IsPoweredOn()) {
                perf = system.GetAndResetPerfStats();
            }

            // Per-thread CPU use since the previous sample.
            const auto now = std::chrono::steady_clock::now();
            const double wall = std::chrono::duration<double>(now - previous_time).count();
            previous_time = now;
            std::vector<std::pair<double, std::string>> busy;
            std::map<u64, double> current_cpu;
            thread_act_array_t threads = nullptr;
            mach_msg_type_number_t thread_count = 0;
            if (task_threads(mach_task_self(), &threads, &thread_count) == KERN_SUCCESS) {
                for (mach_msg_type_number_t i = 0; i < thread_count; ++i) {
                    thread_extended_info_data_t ext{};
                    mach_msg_type_number_t ext_count = THREAD_EXTENDED_INFO_COUNT;
                    thread_identifier_info_data_t id{};
                    mach_msg_type_number_t id_count = THREAD_IDENTIFIER_INFO_COUNT;
                    if (thread_info(threads[i], THREAD_EXTENDED_INFO,
                                    reinterpret_cast<thread_info_t>(&ext), &ext_count) ==
                            KERN_SUCCESS &&
                        thread_info(threads[i], THREAD_IDENTIFIER_INFO,
                                    reinterpret_cast<thread_info_t>(&id), &id_count) ==
                            KERN_SUCCESS) {
                        const double cpu = (ext.pth_user_time + ext.pth_system_time) / 1e9;
                        current_cpu[id.thread_id] = cpu;
                        if (const auto it = previous_cpu.find(id.thread_id);
                            it != previous_cpu.end() && wall > 0) {
                            busy.emplace_back((cpu - it->second) / wall * 100.0,
                                              ext.pth_name[0] ? ext.pth_name
                                                              : fmt::format("tid{}", id.thread_id));
                        }
                    }
                    mach_port_deallocate(mach_task_self(), threads[i]);
                }
                vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads),
                              thread_count * sizeof(thread_act_t));
            }
            previous_cpu = std::move(current_cpu);
            std::sort(busy.begin(), busy.end(), std::greater<>{});
            std::string top;
            for (std::size_t i = 0; i < busy.size() && i < 6; ++i) {
                top += fmt::format(" {}:{:.0f}", busy[i].second, busy[i].first);
            }

            const auto seconds =
                std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
            const std::string line = fmt::format(
                "{} {} {} {} {:.0f} {:.1f} {:.1f} {:.1f} |{}\n", seconds,
                have_info ? info.phys_footprint >> 20 : 0, os_proc_available_memory() >> 20,
                traced ? 1 : 0, perf.emulation_speed * 100.0, perf.game_fps,
                perf.time_swap * 1000.0, perf.time_gpu * 1000.0, top);
            (void)!write(fd, line.data(), line.size());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }).detach();
}

} // namespace

@interface DKEmulator ()
- (void)dispatchHost:(void (^)(id<DKEmulatorHost> host))block;
- (void)setStateOnMain:(DKEmulationState)state;
@end

namespace {

class Host final : public Dekui::Host {
public:
    explicit Host(DKEmulator* emulator_) : emulator{emulator_} {}

    bool ModalOpen() const override {
        return capturing;
    }
    void PersistConfig() override {
        if (config) {
            config->Save();
        }
    }
    void OpenTextureFolder(const std::string& path) override {
        FileUtil::CreateFullPath(path + "/");
        NSString* ns_path = ToNS(path);
        [emulator dispatchHost:^(id<DKEmulatorHost> host) {
          [host emulatorOpenTextureFolder:ns_path];
        }];
    }
    void ShowLicenses(const std::string& path) override {
        NSString* ns_path = ToNS(path);
        [emulator dispatchHost:^(id<DKEmulatorHost> host) {
          [host emulatorShowLicenses:ns_path];
        }];
    }
    void ShowAbout() override {
        ShowLicenses(Dekui::LicensesPath());
    }
    void ShowControls() override {
        [emulator dispatchHost:^(id<DKEmulatorHost> host) {
          [host emulatorShowControls];
        }];
    }
    void EditShortcut(Dekui::Shortcut) override {
        // Window shortcuts do not exist in the visionOS app.
    }
    void ToggleFullscreenRequested() override {
        // visionOS windows are resized by the user; there is no fullscreen mode.
    }
    void Quit() override {
        [emulator dispatchHost:^(id<DKEmulatorHost> host) {
          [host emulatorQuit];
        }];
    }
    // Hold ZR and press D-Pad Down / Up.
    void SaveNewState() override {
        MM3D::Notice::Show([emulator saveNewState] ? "State saved" : "Could not save a state now");
    }
    void LoadLatestState() override {
        if (![emulator loadLatestState]) {
            MM3D::Notice::Show("No save state to load");
        }
    }

    __weak DKEmulator* emulator;
    std::unique_ptr<QtConfig> config;
    bool capturing = false;
};

} // namespace

@implementation DKControllerInfo {
    NSString* _name;
    NSString* _identifier;
}
- (instancetype)initWithName:(NSString*)name identifier:(NSString*)identifier {
    if ((self = [super init])) {
        _name = name;
        _identifier = identifier;
    }
    return self;
}
- (NSString*)name {
    return _name;
}
- (NSString*)identifier {
    return _identifier;
}
@end

@implementation DKStereoFrame {
    id<MTLTexture> _texture;
    NSInteger _eyes;
    MM3D::WideView::Camera _camera;
    id<MTLTexture> _hud;
    id<MTLTexture> _hostHud;
@public
    int _slot;
}
- (instancetype)initWithStereoFrame:(const StereoFrames::Frame&)frame {
    if ((self = [super init])) {
        _texture = frame.texture;
        _eyes = frame.eyes;
        _slot = frame.slot;
        _camera = frame.camera;
        _hud = frame.hud;
        _hostHud = frame.host_hud;
    }
    return self;
}
- (id<MTLTexture>)texture {
    return _texture;
}
- (NSInteger)eyes {
    return _eyes;
}
- (BOOL)hasCamera {
    return _camera.valid;
}
- (simd_quatf)cameraRotation {
    return simd_quaternion(_camera.rotation[0], _camera.rotation[1], _camera.rotation[2],
                           _camera.rotation[3]);
}
- (float)tanHalfWidth {
    return _camera.tan_x;
}
- (float)tanHalfHeight {
    return _camera.tan_y;
}
- (id<MTLTexture>)hudTexture {
    return _hud;
}
- (id<MTLTexture>)hostHudTexture {
    return _hostHud;
}
- (BOOL)hasHeadLook {
    return _camera.head_look;
}
- (float)headLookYaw {
    return _camera.head_yaw;
}
- (float)headLookPitch {
    return _camera.head_pitch;
}
- (BOOL)isUnturned {
    return _camera.unturned;
}
- (BOOL)isMenu {
    return _camera.no_world;
}
@end

namespace {

StereoFrames stereo_frames;

void DeliverStereoFrame(void*, void* texture, u32, u32, u32 eyes, const void* metadata) {
    const auto* camera = static_cast<const MM3D::WideView::Camera*>(metadata);
    stereo_frames.Deliver((__bridge id<MTLTexture>)texture, eyes,
                          camera ? *camera : MM3D::WideView::Camera{});
}

void WaitForStereoFrameReads(void*, void* texture) {
    stereo_frames.WaitForReads((__bridge id<MTLTexture>)texture);
}

} // namespace

@implementation DKEmulator {
    std::unique_ptr<Host> _frontend_host;
    std::unique_ptr<Dekui::Poller> _poller;
    std::unique_ptr<EmuWindow_Apple_Vulkan> _window;
    std::thread _thread;
    std::atomic<bool> _stop;
    std::atomic<bool> _paused;
    std::mutex _pause_mutex;
    std::condition_variable _pause_cv;
    Dekui::Input::OverlayKeyboard _overlay_keyboard;
    Dekui::Input::TouchPointer _touch;
    std::vector<int> _pressed_keys;
    BOOL _prepared;
    DKEmulationState _state;
    CGSize _view_size;
    BOOL _stereo_presenting;
    BOOL _wide_presenting;
    BOOL _stereo_setting;
    unsigned _cloud_ticks;

    // Controller capture
    std::vector<std::unique_ptr<InputCommon::Polling::DevicePoller>> _pollers;
    std::optional<DKControl> _capture_control;
    std::optional<DKStickDirection> _capture_direction;
    void (^_capture_completion)(NSString* _Nullable);
    NSTimeInterval _capture_deadline;
}

+ (DKEmulator*)shared {
    static DKEmulator* shared = [[DKEmulator alloc] init];
    return shared;
}

+ (void)initializeJit {
#if TARGET_OS_TV
    // tvOS has no JIT enabler; the CPU interpreter runs AOT code instead.
    return;
#endif
    // The JIT26 handshake reports to stderr, which nothing reads on the
    // headset, and it runs before the emulator log exists. Keep a copy of its
    // messages in Documents/jit26.log.
    NSURL* documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
                                                             inDomains:NSUserDomainMask]
                           .firstObject;
    const char* path = [documents URLByAppendingPathComponent:@"jit26.log"].fileSystemRepresentation;
    const int file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const int saved_stderr = file >= 0 ? dup(STDERR_FILENO) : -1;
    if (saved_stderr >= 0) {
        fflush(stderr);
        dup2(file, STDERR_FILENO);
    }
#if defined(OAKNUT_APPLE_JIT26)
    std::fprintf(stderr, "[JIT26] debugger attached at launch: %s\n",
                 oaknut::apple_jit26::is_debugger_attached() ? "yes" : "no");
#endif
    Dekui::InitializeJit();
#if defined(OAKNUT_APPLE_JIT26)
    std::fprintf(stderr, "[JIT26] arena ready: %s\n",
                 oaknut::apple_jit26::is_ready() ? "yes" : "no");
#endif
    if (saved_stderr >= 0) {
        fflush(stderr);
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);
    }
    if (file >= 0) {
        close(file);
    }
}

- (BOOL)jitEnabled {
#if defined(OAKNUT_APPLE_JIT26)
    return oaknut::apple_jit26::is_ready();
#else
    // The simulator never has executable memory for the JIT.
    return NO;
#endif
}

static NSString* const AllowJitKey = @"DekuiAllowJIT";

- (BOOL)allowJIT {
    NSNumber* value = [NSUserDefaults.standardUserDefaults objectForKey:AllowJitKey];
    return value ? value.boolValue : YES;
}

- (void)setAllowJIT:(BOOL)allowJIT {
    [NSUserDefaults.standardUserDefaults setBool:allowJIT forKey:AllowJitKey];
    Core::JitPolicy::allowed = allowJIT;
}

- (BOOL)aotLinked {
    return Core::Aot::LinkedModule() != nullptr;
}

- (NSString*)cpuBackend {
    if (Settings::values.use_cpu_jit.GetValue()) {
        return @"JIT";
    }
    return self.aotLinked ? @"Ahead-of-time code" : @"Interpreter";
}

- (instancetype)init {
    if ((self = [super init])) {
#if TARGET_OS_TV
        // Apple TV GPUs before the A12 only have tier 1 Metal argument
        // buffers, with which MoltenVK's descriptor sets lose their textures
        // (untextured geometry). Bind resources directly instead. MoltenVK
        // reads this when the Vulkan instance is created.
        setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "0", 0);
#if defined(DEKUI_TVOS_ASYNC_MOLTENVK_SUBMITS)
        // Keep this behind a build switch: it changes MoltenVK queue scheduling
        // and must be compared independently from CPU/AOT optimization.
        setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "0", 0);
#endif
#endif
        _frontend_host = std::make_unique<Host>(self);
        _state = DKEmulationStateStopped;
    }
    return self;
}

- (void)dispatchHost:(void (^)(id<DKEmulatorHost> host))block {
    __weak DKEmulator* weak_self = self;
    dispatch_async(dispatch_get_main_queue(), ^{
      id<DKEmulatorHost> host = weak_self.host;
      if (host) {
          block(host);
      }
    });
}

- (void)setStateOnMain:(DKEmulationState)state {
    _state = state;
    [self.host emulatorStateChanged:state];
}

- (void)postState:(DKEmulationState)state {
    __weak DKEmulator* weak_self = self;
    dispatch_async(dispatch_get_main_queue(), ^{
      [weak_self setStateOnMain:state];
    });
}

// MARK: Setup

- (NSURL*)gameFolderURL {
#if TARGET_OS_TV
    // Matches MM3D::GameFolder::Root(): tvOS apps cannot write Documents.
    return [[NSFileManager.defaultManager URLsForDirectory:NSCachesDirectory
                                                 inDomains:NSUserDomainMask]
                .firstObject URLByAppendingPathComponent:@"Dekui"
                                             isDirectory:YES];
#else
    return [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
                                                inDomains:NSUserDomainMask]
        .firstObject;
#endif
}

- (DKSetupStatus)prepareWithMessage:(NSString* _Nullable*)message {
    if (_prepared) {
        return DKSetupStatusReady;
    }
    if (const auto error = Dekui::PrepareGameFolder()) {
        if (message) {
            *message = ToNS(error->message);
        }
        return error->kind == Dekui::SetupError::Kind::MissingGame ? DKSetupStatusMissingGame
                                                                    : DKSetupStatusFailed;
    }
    // Verifying the game is slow and may run off the main thread; the rest
    // (SDL's controller backend in particular) starts on the main thread.
    void (^start)(void) = ^{
      KeepPreviousLog();
      InstallCrashHandler(self.jitEnabled);
      StartVitalsLog(self.jitEnabled);
      Common::Log::Initialize();
      Common::Log::Start();
      LOG_INFO(Frontend, "Dekui SwiftUI frontend starting");
#if TARGET_OS_TV
#if defined(DEKUI_TVOS_ASYNC_MOLTENVK_SUBMITS)
      LOG_INFO(Frontend, "tvOS MoltenVK queue submissions: asynchronous");
#else
      LOG_INFO(Frontend, "tvOS MoltenVK queue submissions: synchronous");
#endif
#endif

      self->_frontend_host->config = std::make_unique<QtConfig>();
      Dekui::ApplyPlayerDefaults();
      Network::Init();
      InputCommon::Init();
      self->_poller = std::make_unique<Dekui::Poller>(
          Core::System::GetInstance(), *self->_frontend_host,
          Dekui::KeyboardDefaults{QtConfig::default_buttons, QtConfig::default_analogs});
      self->_prepared = YES;
    };
    if (NSThread.isMainThread) {
        start();
    } else {
        dispatch_sync(dispatch_get_main_queue(), start);
    }
    // Before any game starts: save states may live in iCloud.
    Dekui::CloudStates::Enable();
    // Shader caches built on a Mac, merged into this device's cache at boot.
    Dekui::CloudShaderCache::Fetch();
    // CloudKit carries states to and from devices without iCloud Drive (Apple TV).
#if !TARGET_OS_SIMULATOR
    // Unsigned simulator builds lack the iCloud entitlement, and CKContainer traps.
    Dekui::CloudKitStates::Start();
#endif
    return DKSetupStatusReady;
}

// MARK: Emulation

- (DKEmulationState)state {
    return _state;
}

- (void)startWithLayer:(CAMetalLayer*)layer size:(CGSize)size {
    if (!_prepared || _state != DKEmulationStateStopped) {
        return;
    }
    if (_thread.joinable()) {
        _thread.join();
    }
    _view_size = size;
    _window = std::make_unique<EmuWindow_Apple_Vulkan>((__bridge void*)layer);
    _window->Resize(unsigned(size.width), unsigned(size.height));
    __weak DKEmulator* weak_self = self;
    _window->on_first_frame = [weak_self] {
        DKEmulator* strong_self = weak_self;
        [strong_self postState:DKEmulationStateRunning];
        MM3D::PlayerSettings::startup_hint_requested.store(true);
    };
    _stop = false;
    _paused = false;
    Core::JitPolicy::allowed = self.allowJIT;
    [self setStateOnMain:DKEmulationStateLoading];

    const std::string path = Dekui::GamePath();
    if (const auto error = Dekui::ValidateBootPath(path)) {
        [self setStateOnMain:DKEmulationStateStopped];
        [self.host emulatorFailed:@"Dekui setup" message:ToNS(*error)];
        return;
    }
    // Per-application settings, as the Qt frontend loads them before boot.
    auto& system = Core::System::GetInstance();
    u64 title_id{};
    if (auto loader = Loader::GetLoader(path)) {
        loader->ReadProgramId(title_id);
    }
    if (title_id != 0) {
        QtConfig per_game_config(fmt::format("{:016X}", title_id),
                                 QtConfig::ConfigType::PerGameConfig);
    }
    Dekui::ApplyBootSettings();
    // Other 3D modes (anaglyph, interlaced...) are for flat displays. The
    // Apple TV shows one flat image: no stereo, even with a headset's settings.
    if (TARGET_OS_TV ||
        Settings::values.render_3d.GetValue() != Settings::StereoRenderOption::SideBySideFull) {
        Settings::values.render_3d.SetGlobal(true);
        Settings::values.render_3d = Settings::StereoRenderOption::Off;
    }
    _window->Resize(unsigned(size.width), unsigned(size.height));
    system.ApplySettings();
    Settings::LogSettings();
    MM3D::PlayerSettings::startup_hint_requested.store(false);
    Frontend::RegisterDefaultApplets(system);
    // Ask the player for names instead of answering with the console username.
    system.RegisterSoftwareKeyboard(DekuiMakeSoftwareKeyboard());

    _thread = std::thread([self, path] { [self runEmulation:path]; });
}

- (void)reportProgress:(VideoCore::LoadCallbackStage)stage
                  name:(const std::string&)name
                 value:(std::size_t)value
                 total:(std::size_t)total {
    const double fraction = total ? double(value) / double(total) : 0.0;
    NSString* message = ToNS(StageMessage(stage, name, value, total));
    [self dispatchHost:^(id<DKEmulatorHost> host) {
      [host emulatorLoadProgress:fraction message:message];
    }];
}

- (void)runEmulation:(std::string)path {
    pthread_setname_np("DekuiEmulation");
    auto& system = Core::System::GetInstance();
    const auto result = system.Load(*_window, path);
    if (result != Core::System::ResultStatus::Success) {
        LOG_CRITICAL(Frontend, "Failed to load the game: {}", static_cast<u32>(result));
        NSString* details = ToNS(system.GetStatusDetails());
        [self dispatchHost:^(id<DKEmulatorHost> host) {
          [host emulatorFailed:@"Could not start the game"
                       message:details.length ? details : @"The game failed to load."];
        }];
        [self postState:DKEmulationStateStopped];
        return;
    }

    const auto progress = [self](VideoCore::LoadCallbackStage stage, std::size_t value,
                                 std::size_t total, const std::string& name) {
        [self reportProgress:stage name:name value:value total:total];
    };
    if (Settings::values.custom_textures && Settings::values.preload_textures) {
        system.CustomTexManager().PreloadTextures(_stop, progress);
    }
    u64 program_id{};
    system.GetAppLoader().ReadProgramId(program_id);
    system.GPU().ApplyPerProgramSettings(program_id);
    system.GPU().Renderer().Rasterizer()->LoadDefaultDiskResources(_stop, progress);
    progress(VideoCore::LoadCallbackStage::Complete, 0, 0, "");

    system.RegisterCoreLoopThreadId();
    Dekui::GameLanguage game_language{system};
    while (!_stop) {
        game_language.ApplyPending();
        if (_paused) {
            // visionOS may end a backgrounded (paused) app without a shutdown,
            // which is when the driver pipeline cache is normally written.
            if (auto* rasterizer = system.GPU().Renderer().Rasterizer()) {
                rasterizer->SaveDiskResources();
            }
            // Keep paused audio silent, as the Android frontend does. Mute
            // rather than set the volume to 0: the volume setting is saved
            // whenever any setting changes, also while paused.
            const bool muted = Settings::values.audio_muted;
            Settings::values.audio_muted = true;
            {
                std::unique_lock lock{_pause_mutex};
                _pause_cv.wait(lock, [self] { return !self->_paused || self->_stop; });
            }
            Settings::values.audio_muted = muted;
            continue;
        }
        const auto status = system.RunLoop();
        if (status == Core::System::ResultStatus::ShutdownRequested) {
            break;
        }
        if (status == Core::System::ResultStatus::ErrorSavestate ||
            status == Core::System::ResultStatus::ErrorSavestateBuildMismatch) {
            // The game keeps running from where it was.
            NSString* details = ToNS(system.GetStatusDetails());
            [self dispatchHost:^(id<DKEmulatorHost> host) {
              [host emulatorFailed:@"Save state failed"
                           message:details.length ? details : @"The save state could not be used."];
            }];
            continue;
        }
        if (status != Core::System::ResultStatus::Success) {
            LOG_CRITICAL(Frontend, "Emulation stopped: {}", static_cast<u32>(status));
            NSString* details = ToNS(system.GetStatusDetails());
            [self dispatchHost:^(id<DKEmulatorHost> host) {
              [host emulatorFailed:@"Emulation stopped" message:details];
            }];
            break;
        }
    }
    // A user may choose a language and quit directly from the paused overlay.
    game_language.ApplyPending();
    system.Shutdown();
    [self postState:DKEmulationStateStopped];
}

- (void)setViewSize:(CGSize)size {
    _view_size = size;
    if (_window) {
        _window->Resize(unsigned(size.width), unsigned(size.height));
    }
}

- (void)pause {
    if (_state != DKEmulationStateRunning) {
        return;
    }
    _paused = true;
    [self setStateOnMain:DKEmulationStatePaused];
}

- (void)resume {
    if (_state != DKEmulationStatePaused) {
        return;
    }
    {
        std::scoped_lock lock{_pause_mutex};
        _paused = false;
    }
    _pause_cv.notify_all();
    [self setStateOnMain:DKEmulationStateRunning];
}

- (void)restart {
    if (_state != DKEmulationStateRunning && _state != DKEmulationStatePaused) {
        return;
    }
    // Handled by RunLoop on the emulation thread: shut down and load again.
    Core::System::GetInstance().RequestReset();
    [self resume];
}

- (void)stop {
    {
        std::scoped_lock lock{_pause_mutex};
        _stop = true;
        _paused = false;
    }
    _pause_cv.notify_all();
    if (_thread.joinable()) {
        _thread.join();
    }
    _frontend_host->PersistConfig();
}

- (void)tick {
    if (!_prepared) {
        return;
    }
    _poller->Tick();
    [self pollCapture];
    // States saved on another device arrive as remote-only files.
    if (++_cloud_ticks % 1800 == 0) {
        Dekui::CloudStates::DownloadMissing();
        Dekui::CloudKitStates::Sync();
    }
    [self updateStereoPresentation];
}

- (void)updateStereoPresentation {
#if TARGET_OS_TV
    // Frames always go to the game view's layer: stereo and the wide view
    // hand them to RealityKit, which the Apple TV does not have. Settings
    // copied from the headset may ask for 3D.
    if (MM3D::PlayerSettings::stereo.load() != 0) {
        MM3D::PlayerSettings::set_stereo.store(0);
    }
    MM3D::WideView::enabled.store(false);
    return;
#endif
    // The immersive wide view also takes every frame, in 2D or 3D. Its render
    // target's shape depends on 3D too (EmuWindow_Apple_Vulkan::Resize).
    const BOOL wide = MM3D::WideView::enabled.load();
    const BOOL stereo_setting = MM3D::PlayerSettings::stereo.load() != 0;
    const BOOL stereo = stereo_setting || wide;
    if (stereo == _stereo_presenting && wide == _wide_presenting &&
        stereo_setting == _stereo_setting) {
        return;
    }
    _wide_presenting = wide;
    _stereo_setting = stereo_setting;
    _stereo_presenting = stereo;
    stereo_frames.Reset();
    Vulkan::ExternalPresent::SetSink(stereo ? DeliverStereoFrame : nullptr, nullptr,
                                     stereo ? WaitForStereoFrameReads : nullptr);
    // Each eye gets the whole view (see EmuWindow_Apple_Vulkan::Resize).
    if (_window) {
        _window->Resize(unsigned(_view_size.width), unsigned(_view_size.height));
    }
}

// MARK: Overlay and input

- (BOOL)dekuiSettingsAvailable {
    return MM3D::presentation_available.load();
}

- (BOOL)dekuiSettingsVisible {
    return MM3D::overlay_visible.load();
}

- (void)toggleDekuiSettings {
    if (MM3D::ToggleOverlayView(true) && _window) {
        _window->TouchReleased();
    }
}

- (void)overlayInput:(DKOverlayInput)input down:(BOOL)down {
    if ((!MM3D::overlay_visible.load() || _capture_control.has_value()) && down) {
        return;
    }
    static constexpr std::array keys{
        ImGuiKey_GamepadDpadUp,   ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft,
        ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight,
        ImGuiKey_GamepadL1,        ImGuiKey_GamepadR1,
    };
    if (input >= 0 && std::size_t(input) < keys.size()) {
        MM3D::KeyInput(keys[std::size_t(input)], down);
    }
}

- (void)pointerDownAt:(CGPoint)point {
    _touch.Begin(point.x, point.y);
}

- (void)pointerMovedTo:(CGPoint)point {
    _touch.Move(point.x, point.y);
}

- (void)pointerUpAt:(CGPoint)point {
    _touch.Move(point.x, point.y);
    _touch.End();
}

- (void)scrollAt:(CGPoint)point delta:(CGVector)delta {
    MM3D::Scroll(point.x, point.y, delta.dx, delta.dy);
}

- (BOOL)handleKey:(UIKey*)key down:(BOOL)down {
    const int code = QtKeyFromHID(key.keyCode);
    if (code == 0) {
        return NO;
    }
    if (_capture_control && down) {
        if (code != Qt::Key_Escape && (*_capture_control < DKControlCirclePad || _capture_direction)) {
            [self finishCapture:Common::ParamPackage{InputCommon::GenerateKeyboardParam(code)}];
        } else {
            [self cancelCapture];
        }
        return YES;
    }
    if (_window && Dekui::Input::OverlayToggleKey(DekuiKey(code), down, false, *_window)) {
        return YES;
    }
    const UIKeyModifierFlags flags = key.modifierFlags;
    Dekui::Input::Modifiers modifiers;
    modifiers.shift = flags & UIKeyModifierShift;
    modifiers.control = flags & UIKeyModifierControl;
    modifiers.alt = flags & UIKeyModifierAlternate;
    modifiers.command = flags & UIKeyModifierCommand;
    const char* text = key.characters.UTF8String ?: "";
    if (_overlay_keyboard.Key(code, DekuiKey(code), modifiers, down, false, text, false)) {
        return YES;
    }
    if (down) {
        _pressed_keys.push_back(code);
    } else {
        std::erase(_pressed_keys, code);
    }
    Dekui::Input::GameKey(code, down, false);
    return YES;
}

- (void)releaseAllKeys {
    _pressed_keys.clear();
    if (auto* keyboard = InputCommon::GetKeyboard()) {
        keyboard->ReleaseAllKeys();
    }
}

// MARK: Settings

static NSInteger Requested(const std::atomic<int>& request, const std::atomic<int>& current) {
    const int value = request.load();
    return value >= 0 ? value : current.load();
}

- (NSInteger)resolution {
    return Requested(MM3D::PlayerSettings::set_resolution, MM3D::PlayerSettings::resolution);
}
- (void)setResolution:(NSInteger)resolution {
    MM3D::PlayerSettings::set_resolution.store(int(resolution));
}
- (NSInteger)volume {
    return Requested(MM3D::PlayerSettings::set_volume, MM3D::PlayerSettings::volume);
}
- (void)setVolume:(NSInteger)volume {
    MM3D::PlayerSettings::set_volume.store(int(volume));
}
- (BOOL)vsync {
    return Requested(MM3D::PlayerSettings::set_vsync, MM3D::PlayerSettings::vsync) != 0;
}
- (void)setVsync:(BOOL)vsync {
    MM3D::PlayerSettings::set_vsync.store(vsync ? 1 : 0);
}
- (BOOL)diskShaderCache {
    return Requested(MM3D::PlayerSettings::set_cache, MM3D::PlayerSettings::cache) != 0;
}
- (void)setDiskShaderCache:(BOOL)enabled {
    MM3D::PlayerSettings::set_cache.store(enabled ? 1 : 0);
}
- (BOOL)hardwareShaders {
    return Requested(MM3D::PlayerSettings::set_hardware_shaders,
                     MM3D::PlayerSettings::hardware_shaders) != 0;
}
- (void)setHardwareShaders:(BOOL)enabled {
    MM3D::PlayerSettings::set_hardware_shaders.store(enabled ? 1 : 0);
}
- (BOOL)asyncShaders {
    return Requested(MM3D::PlayerSettings::set_async_shaders,
                     MM3D::PlayerSettings::async_shaders) != 0;
}
- (void)setAsyncShaders:(BOOL)enabled {
    MM3D::PlayerSettings::set_async_shaders.store(enabled ? 1 : 0);
}
- (BOOL)customTextures {
    return Requested(MM3D::PlayerSettings::set_textures, MM3D::PlayerSettings::textures) != 0;
}
- (void)setCustomTextures:(BOOL)enabled {
    MM3D::PlayerSettings::set_textures.store(enabled ? 1 : 0);
}
- (NSInteger)gameLanguage {
    return Requested(MM3D::PlayerSettings::set_game_language,
                     MM3D::PlayerSettings::game_language);
}
- (void)setGameLanguage:(NSInteger)language {
    if (MM3D::PlayerSettings::SupportedLanguage(int(language))) {
        MM3D::PlayerSettings::game_language_status.store(1);
        MM3D::PlayerSettings::set_game_language.store(int(language));
    }
}
- (NSInteger)gameLanguageStatus {
    return MM3D::PlayerSettings::game_language_status.load();
}

// MARK: Stereoscopic 3D

- (BOOL)stereoscopic {
    return Requested(MM3D::PlayerSettings::set_stereo, MM3D::PlayerSettings::stereo) != 0;
}
- (void)setStereoscopic:(BOOL)enabled {
    // A depth of 0 renders both eyes the same.
    if (enabled && self.depth3D == 0) {
        MM3D::PlayerSettings::set_depth_3d.store(100);
    }
    MM3D::PlayerSettings::set_stereo.store(enabled ? 1 : 0);
}
- (NSInteger)depth3D {
    return Requested(MM3D::PlayerSettings::set_depth_3d, MM3D::PlayerSettings::depth_3d);
}
- (void)setDepth3D:(NSInteger)depth {
    MM3D::PlayerSettings::set_depth_3d.store(int(std::clamp<NSInteger>(depth, 0, 100)));
}
- (DKStereoFrame*)acquireStereoFrame {
    StereoFrames::Frame frame;
    if (!_stereo_presenting || !stereo_frames.Acquire(frame)) {
        return nil;
    }
    return [[DKStereoFrame alloc] initWithStereoFrame:frame];
}
- (void)releaseStereoFrame:(DKStereoFrame*)frame {
    stereo_frames.Release(frame->_slot);
}

// MARK: Immersive wide view

- (BOOL)wideView {
    return MM3D::WideView::enabled.load();
}
- (void)setWideView:(BOOL)enabled {
    // updateStereoPresentation (next tick) routes frames and resizes the target.
    MM3D::WideView::enabled.store(enabled);
    if (!enabled) {
        MM3D::WideView::head_look.store(false);
    }
    // The game's eye separation is made for the 3DS screen; at the immersive
    // view's size, a quarter of it keeps distant scenery fusable.
    Settings::SetStereoDepthScale(enabled ? 0.25f : 1.0f);
}
- (void)setHeadLookYaw:(float)yaw pitch:(float)pitch anchorPitch:(float)anchorPitch
                    enabled:(BOOL)enabled {
    MM3D::WideView::head_yaw.store(yaw);
    MM3D::WideView::head_pitch.store(pitch);
    MM3D::WideView::anchor_pitch.store(anchorPitch);
    MM3D::WideView::head_look.store(enabled);
}
- (BOOL)firstPersonCamera {
    return MM3D::WideView::first_person.load();
}
- (void)logWideView:(NSString*)message {
    LOG_INFO(Frontend, "Wide view: {}", message.UTF8String);
}
- (void)logMessage:(NSString*)message {
    LOG_INFO(Frontend, "{}", message.UTF8String);
}
- (NSInteger)wideViewFov {
    return NSInteger(std::lround(MM3D::WideView::fov.load()));
}
- (void)setWideViewFov:(NSInteger)fov {
    MM3D::WideView::fov.store(std::clamp(float(fov), MM3D::WideView::MinFov, MM3D::WideView::MaxFov));
}
- (float)wideViewCentring {
    return MM3D::WideView::centring.load();
}
- (void)setWideViewCentring:(float)centring {
    MM3D::WideView::centring.store(std::clamp(centring, 0.f, 1.f));
}

// MARK: Controls

- (NSArray<DKControllerInfo*>*)connectedControllers {
    NSMutableArray* result = [NSMutableArray array];
    for (const auto& controller : InputCommon::GetConnectedControllers()) {
        // SDL publishes no product name; the Swift side names controllers
        // through the GameController framework.
        NSString* name = ToNS(fmt::format("Controller {}", controller.Get("port", 0) + 1));
        NSString* identifier = ToNS(controller.Serialize());
        [result addObject:[[DKControllerInfo alloc] initWithName:name identifier:identifier]];
    }
    return result;
}

- (NSString*)bindingForControl:(DKControl)control {
    const auto& profile = Settings::values.current_input_profile;
    if (control < DKControlCirclePad) {
        return ToNS(ButtonText(Common::ParamPackage{profile.buttons[control]}));
    }
    return ToNS(AnalogText(Common::ParamPackage{profile.analogs[control - DKControlCirclePad]}));
}

- (NSString*)bindingForStick:(DKControl)stick direction:(DKStickDirection)direction {
    const Common::ParamPackage analog{
        Settings::values.current_input_profile.analogs[stick - DKControlCirclePad]};
    if (analog.Get("engine", "") != "analog_from_button") {
        return @"";
    }
    return ToNS(ButtonText(Common::ParamPackage{analog.Get(StickDirectionKeys[direction], "")}));
}

- (void)captureBindingForStick:(DKControl)stick
                     direction:(DKStickDirection)direction
                    completion:(void (^)(NSString* _Nullable))completion {
    [self captureBindingForControl:stick completion:completion];
    _capture_direction = direction;
    // A direction takes a button, key or a single stick direction.
    for (auto& poller : _pollers) {
        poller->Stop();
    }
    _pollers = InputCommon::Polling::GetPollers(InputCommon::Polling::DeviceType::Button);
    for (auto& poller : _pollers) {
        poller->Start();
    }
}

- (void)captureBindingForControl:(DKControl)control
                      completion:(void (^)(NSString* _Nullable))completion {
    [self cancelCapture];
    _capture_direction.reset();
    const bool analog = control >= DKControlCirclePad;
    _pollers = InputCommon::Polling::GetPollers(analog ? InputCommon::Polling::DeviceType::Analog
                                                       : InputCommon::Polling::DeviceType::Button);
    for (auto& poller : _pollers) {
        poller->Start();
    }
    _capture_control = control;
    _capture_completion = [completion copy];
    // A stick needs movement on both axes, which takes longer than a press.
    _capture_deadline = NSDate.timeIntervalSinceReferenceDate + (analog ? 10.0 : 5.0);
    // Automatic configuration must not replace the profile mid-capture.
    _frontend_host->capturing = true;
    [self releaseAllKeys];
}

- (void)pollCapture {
    if (!_capture_control) {
        return;
    }
    if (NSDate.timeIntervalSinceReferenceDate > _capture_deadline) {
        [self cancelCapture];
        return;
    }
    for (auto& poller : _pollers) {
        auto params = poller->GetNextInput();
        if (params.Has("engine")) {
            [self finishCapture:params];
            return;
        }
    }
}

- (void)endCapture:(NSString* _Nullable)binding {
    for (auto& poller : _pollers) {
        poller->Stop();
    }
    _pollers.clear();
    _capture_control.reset();
    _capture_direction.reset();
    _frontend_host->capturing = false;
    auto completion = _capture_completion;
    _capture_completion = nil;
    if (completion) {
        completion(binding);
    }
}

- (void)finishCapture:(Common::ParamPackage)params {
    const DKControl control = *_capture_control;
    const auto direction = _capture_direction;
    ApplyMaptype(params);
    auto& profile = Settings::values.current_input_profile;
    if (control < DKControlCirclePad) {
        profile.buttons[control] = params.Serialize();
    } else if (direction) {
        auto& binding = profile.analogs[control - DKControlCirclePad];
        Common::ParamPackage analog{binding};
        if (analog.Get("engine", "") != "analog_from_button") {
            // Start from no directions, keeping the keyboard's slow-walk modifier.
            const auto& keys = QtConfig::default_analogs[control - DKControlCirclePad];
            analog = Common::ParamPackage{{"engine", "analog_from_button"},
                                          {"modifier", InputCommon::GenerateKeyboardParam(keys[4])},
                                          {"modifier_scale", "0.5"}};
        }
        analog.Set(StickDirectionKeys[*direction], params.Serialize());
        binding = analog.Serialize();
    } else {
        profile.analogs[control - DKControlCirclePad] = params.Serialize();
    }
    const int index = Settings::values.current_input_profile_index;
    if (index >= 0 && index < int(Settings::values.input_profiles.size())) {
        Settings::SaveProfile(index);
    }
    Core::System::GetInstance().ApplySettings();
    _frontend_host->PersistConfig();
    [self endCapture:direction ? [self bindingForStick:control direction:*direction]
                               : [self bindingForControl:control]];
}

- (void)cancelCapture {
    if (_capture_control) {
        [self endCapture:nil];
    }
}

- (void)resetControlsToAutomatic {
    // Restore the keyboard defaults; the next tick maps the connected
    // controller onto them as an automatic profile again.
    auto& profile = Settings::values.current_input_profile;
    profile.maptype = Settings::InputMappingType::GuidPort;
    for (std::size_t i = 0; i < NumButtons; ++i) {
        profile.buttons[i] = InputCommon::GenerateKeyboardParam(QtConfig::default_buttons[i]);
    }
    for (std::size_t i = 0; i < profile.analogs.size(); ++i) {
        const auto& keys = QtConfig::default_analogs[i];
        profile.analogs[i] = InputCommon::GenerateAnalogParamFromKeys(keys[0], keys[1], keys[2],
                                                                      keys[3], keys[4], .5f);
    }
    const int index = Settings::values.current_input_profile_index;
    if (index >= 0 && index < int(Settings::values.input_profiles.size())) {
        Settings::SaveProfile(index);
    }
    Core::System::GetInstance().ApplySettings();
    _frontend_host->PersistConfig();
}

@end
