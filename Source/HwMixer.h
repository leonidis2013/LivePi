#pragma once

#include <juce_core/juce_core.h>

// The audio interface's own level controls (ALSA mixer: input gain, output volume, switches and
// choices such as an input selector). Many HATs and USB interfaces have them; some, like the
// original Blokas Pisound, set their levels with analog knobs only, which software cannot read.
//
// LivePi remembers what you set (~/.livepi/hwmixer.json) and puts it back at start-up, because
// the OS does not always restore it for the user LivePi runs as.
// Safe to call from any thread. (ALSA headers stay in HwMixer.cpp.)
namespace HwMixer
{
    // Cards with their controls. A card is marked "inUse" when the device names contain its id or name.
    juce::var list (const juce::StringArray& deviceNames);

    // body: { card (id), control, index, kind: "playback"|"capture"|"enum", value?, on?, item? }
    juce::var set (const juce::var& body);

    // Put back the saved levels (call once at start-up).
    void restore();
}
