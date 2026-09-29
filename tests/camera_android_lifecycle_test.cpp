#include "camera_capture.h"
#include "ndk_mock.h"
#include <iostream>

int failures = 0;
void check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
int main() {
    using namespace mock_camera;
    sfr::CameraFrame frame;
    reset();
    {
        auto camera = sfr::CameraCapture::open(2, 2, "selected");
        check(camera && camera->next(frame), "normal capture produces a frame");
        error_during_close = true;
    }
    check(closes == 1 && !device, "normal shutdown closes device once");
    check(freed_request_in_callback == 0, "callbacks drain before request/context destruction");
    reset();
    {
        auto camera = sfr::CameraCapture::open(2, 2, "selected");
        closed();
    }
    check(invalid_session_calls == 0, "shutdown never touches an asynchronously deleted session");
    reset();
    {
        auto camera = sfr::CameraCapture::open(2, 2);
        queue_closed();
    }
    check(!pending_callback.joinable(), "shutdown drains the separate callback thread");
    check(invalid_session_calls == 0, "pending onClosed cannot race shutdown session access");
    reset(); retain_session_on_device_close = true;
    {
        auto camera = sfr::CameraCapture::open(2, 2);
    }
    check(!sessions.back()->alive, "shutdown releases session app reference surviving device close");
    for (bool error : {false, true}) {
        reset();
        auto camera = sfr::CameraCapture::open(2, 2, "selected");
        check(camera->next(frame), "capture works before interruption");
        const auto previous = frame.number;
        lost(error);
        fail_open = true;
        camera->next(frame);
        const int attempted = opens;
        for (int i = 0; i < 100; ++i) camera->next(frame);
        check(opens == attempted, "retries do not spin while camera is unavailable");
        ticks += 1000;
        camera->next(frame);
        check(opens > 1, "disconnection/error schedules a reopen attempt");
        fail_open = false;
        ticks += 1000;
        const bool recovered = camera->next(frame);
        check(recovered || camera->next(frame), "frames resume after camera becomes available");
        check(last_id == "selected", "recovery preserves selected camera id");
        check(frame.number > previous, "frame numbering continues across reconnect");
        camera.reset();
        check(invalid_session_calls == 0, "recovery never uses freed session");
    }
    reset();
    {
        auto camera = sfr::CameraCapture::open(2, 2);
        lost(true); fail_open = true;
        camera->next(frame);
    }
    check(!device && closes == 1, "shutdown during retry closes resources without reopening");
    reset(); defer_ready = true;
    {
        auto camera = sfr::CameraCapture::open(2, 2);
        check(camera && camera->next(frame), "initial capture does not wait for an onReady notification");
        closed();
    }
    check(invalid_session_calls == 0, "shutdown before first ready never uses deleted session");
    if (!failures) std::cout << "Android camera lifecycle checks passed\n";
    return failures ? 1 : 0;
}
