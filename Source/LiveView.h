#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <memory>

// Shows plugin windows inside the web page without VNC: grabs the plugin's area of the virtual
// screen as a JPEG, and plays the browser's taps/clicks/keys back with XTest.
// libX11 and libXtst are loaded at run time, so LiveHost still starts on a Pi without them.
// Thread-safe. (X11 headers stay in LiveView.cpp: their macros clash with JUCE names.)
class LiveView
{
public:
    LiveView();
    ~LiveView();

    juce::String open (const juce::String& displayName);   // empty = ready, else what is wrong
    bool canInput() const;

    struct Frame
    {
        juce::MemoryBlock jpeg;          // empty when the picture equals `sinceHash`
        juce::uint64 hash = 0;
        int width = 0, height = 0;       // plugin window size on the Pi (input coordinates)
    };
    bool grab (juce::Rectangle<int> screenArea, int maxWidth, juce::uint64 sinceHash, Frame& frame);

    void pointer (juce::Point<int> screenPos, int button, bool down);   // button 0 = move only
    void wheel (juce::Point<int> screenPos, int dy);                    // dy < 0 = up
    void key (const juce::String& k);                                   // a character or Enter/Backspace/...

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
