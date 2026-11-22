#include "avatar_notice.h"

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}
}

int main() {
    using sfr::AvatarNotice;
    {
        AvatarNotice notice;
        require(notice.line(0.0).empty(), "nothing before an Avatar is drawn");
        notice.drawn(1.0, AvatarNotice::Reason::none);
        require(notice.line(1.0).empty(), "an Avatar with its model says nothing");
    }
    {
        AvatarNotice notice;
        notice.drawn(10.0, AvatarNotice::Reason::no_model);
        require(notice.line(10.0) == "NO AVATAR MODEL. CHOOSE ONE IN THE LAUNCHER", "no model says where to choose one");
        require(!notice.line(10.4).empty(), "a present between two Avatar draws keeps the note");
        require(notice.line(11.0).empty(), "the note goes when the Avatar is no longer drawn");
        bool shown_late = false, shown_early = true;
        for (double t = 10.0; t <= 25.0; t += 1.0 / 60.0) {
            notice.drawn(t, AvatarNotice::Reason::no_model);
            if (t < 10.0 + AvatarNotice::shown_for - 0.1) shown_early = shown_early && !notice.line(t).empty();
            if (t > 10.0 + AvatarNotice::shown_for + 0.1) shown_late = shown_late || !notice.line(t).empty();
        }
        require(shown_early, "shown on every frame while the time lasts");
        require(!shown_late, "the note goes after it has been up for a while, though the race goes on");
        // Back from the race, then into another one: shown again.
        notice.drawn(30.0, AvatarNotice::Reason::no_model);
        require(!notice.line(30.0).empty(), "a new race shows the note again");
    }
    {
        AvatarNotice notice;
        for (double t = 0.0; t < 5.0; t += 1.0 / 60.0) {
            notice.drawn(t, AvatarNotice::Reason::no_model);
            require(!notice.line(t).empty(), "shown on every frame of the first seconds");
        }
        notice.drawn(5.0 + AvatarNotice::gap - 0.5, AvatarNotice::Reason::no_model);
        require(!notice.line(5.0 + AvatarNotice::gap - 0.5).empty(), "a short pause in the draws is the same race");
    }
    {
        AvatarNotice notice;
        notice.drawn(2.0, AvatarNotice::Reason::model_failed);
        require(notice.line(2.0) == "AVATAR MODEL DID NOT LOAD. SEE GAME.LOG", "a model that failed says so");
    }
    std::cout << "avatar_notice: ok\n";
}
