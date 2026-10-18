#include "host_thread_stats.h"
#include <iostream>
#include <stdexcept>
#include <string>

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    try {
        // A Tensor G4-like 4+3+1: per-core times grouped by speed, per frame.
        long tensor[64] = {191, 191, 191, 191, 741, 741, 741, 1024};
        double ms[64] = {};
        ms[1] = 12;
        ms[5] = 6;
        ms[6] = 6;
        ms[7] = 120;
        require(sfr::time_by_speed(ms, tensor, 4) == "191:3.0,741:3.0,1024:30.0", "times grouped by core speed");
        double none_ms[64] = {};
        require(sfr::time_by_speed(none_ms, tensor, 4) == "191:0.0,741:0.0,1024:0.0", "every speed listed, even unused");
        long unknown[64] = {};
        require(sfr::time_by_speed(none_ms, unknown, 1) == "none", "no speeds and no time: none");
        double some[64] = {};
        some[2] = 5;
        require(sfr::time_by_speed(some, unknown, 1) == "0:5.0", "time on cores of unknown speed: speed 0");

        // The window: a line every 120 frames, drawn and undrawn work apart.
        sfr::HostThreadWindow window;
        for (int frame = 1; frame < 120; ++frame)
            require(window.frame(frame, frame % 4 ? 10.0 : 30.0, frame % 4 == 0, frame % 4 ? 0.0 : 8.0, frame % 4 ? 0.0 : 2.0, tensor).empty(), "quiet between lines");
        const std::string line = window.frame(120, 30.0, true, 8.0, 2.0, tensor);
        require(line.rfind("HOST_THREADS frame=120 drawn=30 drawn_ms=30.00 undrawn_ms=10.00 draw_ms=8.00 record_ms=2.00 ", 0) == 0,
                "the line gives drawn frames and their work apart from undrawn ones");
        require(line.find(" render_on=") != std::string::npos && line.find(" mhz=") != std::string::npos &&
                line.back() == '\n', "the line ends with the render thread's cores and the clocks");
        require(window.frame(121, 10.0, false, 0.0, 0.0, tensor).empty(), "the next window starts again");
        std::cout << "host_thread_stats tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
