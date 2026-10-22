#include "ios_audio_session.h"

#import <AVFoundation/AVFoundation.h>

#include <sstream>

namespace sfr {

std::string ios_audio_session_summary() {
    @autoreleasepool {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        std::ostringstream text;
        text << "category=" << (session.category ? session.category.UTF8String : "none") << " options=0x" << std::hex
             << static_cast<unsigned long>(session.categoryOptions) << std::dec
             << " other_audio=" << (session.isOtherAudioPlaying ? 1 : 0) << " rate=" << session.sampleRate
             << " volume=" << session.outputVolume << " outputs=";
        bool first = true;
        for (AVAudioSessionPortDescription* port in session.currentRoute.outputs) {
            text << (first ? "" : ",") << port.portType.UTF8String;
            first = false;
        }
        if (first) text << "none";
        return text.str();
    }
}

}  // namespace sfr
