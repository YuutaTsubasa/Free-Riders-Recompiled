#pragma once

#include <optional>
#include <string>

namespace sfr::launcher {

// The system's document picker (Files), shown over the launcher until the
// player picks a file or cancels. The file is opened in place: the app keeps
// the access it was given for as long as it runs, so the installer can read a
// disc image without a copy of it. It starts in `folder` when that is one
// (the app's Documents, where the Files app lets the player put the disc
// image). Null when cancelled.
std::optional<std::string> ios_pick_document(bool avatar_model, const std::string& folder);

}  // namespace sfr::launcher
