#pragma once
#include <string>

namespace sfr {
// The note written over the frame when a local player races as AVATAR with
// no model of theirs to draw: no VRM chosen in the launcher, or one that did
// not load. The Xbox avatar system answers with an avatar of no parts
// (diagnostic_main.cpp, XamAvatarGetAssets), so the board races with nothing
// on it; the note says why, and where to choose a model, instead of leaving
// the player to wonder.
//
// Shown from the first frame such a rider is drawn (the Loading screen) for
// `shown_for` seconds; a rider not drawn for `gap` seconds is a new race, and
// shows it again. Times are seconds on any steady clock.
class AvatarNotice {
public:
    static constexpr double shown_for = 12.0;
    static constexpr double gap = 3.0;
    // How long after the last such draw the note still stands: presents may
    // outnumber the Avatar's draws (a skipped frame).
    static constexpr double linger = 0.5;

    enum class Reason { none, no_model, model_failed };

    // The Avatar was drawn at `now` without a model, for `reason`.
    void drawn(double now, Reason reason) {
        if (reason == Reason::none) return;
        if (reason_ == Reason::none || now - last_ > gap) first_ = now;
        last_ = now;
        reason_ = reason;
    }

    // The line to show at `now`, or empty.
    std::string line(double now) const {
        if (reason_ == Reason::none || now - last_ > linger || now - first_ > shown_for) return {};
        return reason_ == Reason::no_model ? "NO AVATAR MODEL. CHOOSE ONE IN THE LAUNCHER"
                                           : "AVATAR MODEL DID NOT LOAD. SEE GAME.LOG";
    }

private:
    Reason reason_ = Reason::none;
    double first_ = 0.0, last_ = 0.0;
};
}
