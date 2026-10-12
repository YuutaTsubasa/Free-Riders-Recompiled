#pragma once

namespace sfr {
// The menu's Kinect adjustment. The title asks the system for its Kinect
// troubleshooter (XamShowNuiTroubleshooterUI) or Kinect Guide; with a real
// Kinect in use the Kinect preview window opens over the game's sensor in
// their place (kinect_preview.h), and the system UI counts as shown until
// the window is closed. With the webcam the webcam's adjustment window opens
// over the game's camera instead (camera_tuner.h); with neither (pad only)
// nothing opens.
enum class KinectTuner { opened, already_open, unavailable };
KinectTuner open_kinect_tuner();
bool kinect_tuner_open();
// Closes it (a pad's Back), the game's window in front again.
void close_kinect_tuner();

// The adjustment window as the system's Kinect Guide overlay: opened, then
// XN_SYS_UI 1 told to the title until the window closes (0), the controls
// held meanwhile, as the console's overlay took them (diagnostic_main.cpp).
// `why` names the asker in the log.
KinectTuner show_system_guide(const char* why);
}
