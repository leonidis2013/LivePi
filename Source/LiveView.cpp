#include "LiveView.h"
#include <juce_graphics/juce_graphics.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#include <mutex>

// Shows plugin windows inside the web page without VNC: grabs the plugin's area of the virtual
// screen, sends it as a JPEG, and plays the browser's taps/clicks/keys back with XTest.
// libX11 and libXtst are loaded at run time, so LivePi still starts on a Pi without them
// (the live view then reports what is missing). Thread-safe: one private X connection behind a mutex.
struct LiveView::Impl
{
    ~Impl()
    {
        std::lock_guard<std::mutex> l (lock);
        if (dpy != nullptr && x.CloseDisplay != nullptr) x.CloseDisplay (dpy);
    }

    // Returns an empty string when ready, otherwise what is wrong.
    juce::String open (const juce::String& displayName)
    {
        std::lock_guard<std::mutex> l (lock);
        if (dpy != nullptr) return {};
        if (! loadLibs()) return problem;
        dpy = x.OpenDisplay (displayName.toRawUTF8());
        if (dpy == nullptr) { problem = "Cannot open the virtual screen " + displayName; return problem; }
        root = x.RootWin (dpy);
        problem.clear();
        return {};
    }

    bool canInput() const { return t.FakeButtonEvent != nullptr; }
    juce::String getProblem() const { return problem; }

    // Grabs `area` (screen pixels). Returns false if nothing could be grabbed.
    // If the pixels hash to `sinceHash` the JPEG is not encoded (frame.jpeg stays empty).
    bool grab (juce::Rectangle<int> area, int maxWidth, juce::uint64 sinceHash, Frame& frame)
    {
        std::lock_guard<std::mutex> l (lock);
        if (dpy == nullptr || area.isEmpty()) return false;

        XImage* img = x.GetImage (dpy, root, area.getX(), area.getY(), (unsigned) area.getWidth(), (unsigned) area.getHeight(), AllPlanes, ZPixmap);
        if (img == nullptr) return false;

        const int w = img->width, h = img->height;
        frame.width = w; frame.height = h;

        // FNV-1a over every 4th pixel of every 2nd row: fast, and any visible change touches it
        juce::uint64 hv = 1469598103934665603ull;
        for (int yy = 0; yy < h; yy += 2)
        {
            auto* row = (const juce::uint32*) (img->data + (size_t) yy * (size_t) img->bytes_per_line);
            for (int xx = (yy >> 1) & 3; xx < w; xx += 4)
            {
                hv ^= row[xx];
                hv *= 1099511628211ull;
            }
        }
        hv ^= (juce::uint64) w << 32 ^ (juce::uint64) h;
        frame.hash = hv;

        if (hv == sinceHash || img->bits_per_pixel != 32)
        {
            const bool usable = img->bits_per_pixel == 32;
            img->f.destroy_image (img);
            return usable;
        }

        juce::Image im (juce::Image::RGB, w, h, false);
        {
            juce::Image::BitmapData bd (im, juce::Image::BitmapData::writeOnly);
            for (int yy = 0; yy < h; ++yy)
            {
                auto* src = (const juce::uint8*) (img->data + (size_t) yy * (size_t) img->bytes_per_line);
                auto* dst = bd.getLinePointer (yy);
                for (int xx = 0; xx < w; ++xx, src += 4, dst += bd.pixelStride)
                    reinterpret_cast<juce::PixelRGB*> (dst)->setARGB (255, src[2], src[1], src[0]);
            }
        }
        img->f.destroy_image (img);

        if (maxWidth > 64 && maxWidth < w)
            im = im.rescaled (maxWidth, juce::jmax (1, h * maxWidth / w), juce::Graphics::mediumResamplingQuality);

        juce::JPEGImageFormat jpg;
        jpg.setQuality (0.72f);
        juce::MemoryOutputStream out (frame.jpeg, false);
        jpg.writeImageToStream (im, out);
        return true;
    }

    juce::uint32 pixelAt (int px, int py)
    {
        std::lock_guard<std::mutex> l (lock);
        if (dpy == nullptr) return 0;
        XImage* img = x.GetImage (dpy, root, px, py, 1, 1, AllPlanes, ZPixmap);
        if (img == nullptr) return 0;
        juce::uint32 v = 0;
        if (img->bits_per_pixel == 32) v = 0xff000000u | (*(const juce::uint32*) img->data & 0xffffffu);
        img->f.destroy_image (img);
        return v;
    }

    // Pointer: button 0 = move only, 1 left, 3 right; wheel: dy < 0 = up
    void pointer (juce::Point<int> screenPos, int button, bool down)
    {
        std::lock_guard<std::mutex> l (lock);
        if (dpy == nullptr || ! canInput()) return;
        t.FakeMotionEvent (dpy, -1, screenPos.x, screenPos.y, 0);
        if (button > 0) t.FakeButtonEvent (dpy, (unsigned) button, down ? True : False, 0);
        x.Flush (dpy);
    }

    void wheel (juce::Point<int> screenPos, int dy)
    {
        std::lock_guard<std::mutex> l (lock);
        if (dpy == nullptr || ! canInput()) return;
        t.FakeMotionEvent (dpy, -1, screenPos.x, screenPos.y, 0);
        const unsigned b = dy < 0 ? 4u : 5u;
        for (int i = 0; i < juce::jlimit (1, 10, std::abs (dy)); ++i)
        {
            t.FakeButtonEvent (dpy, b, True, 0);
            t.FakeButtonEvent (dpy, b, False, 0);
        }
        x.Flush (dpy);
    }

    // A typed character or a named key (Enter, Backspace, Escape, Tab, ArrowLeft...).
    void key (const juce::String& k)
    {
        std::lock_guard<std::mutex> l (lock);
        if (dpy == nullptr || ! canInput() || k.isEmpty()) return;

        static const std::pair<const char*, KeySym> named[] = {
            { "Enter", XK_Return }, { "Backspace", XK_BackSpace }, { "Escape", XK_Escape }, { "Tab", XK_Tab },
            { "Delete", XK_Delete }, { "ArrowLeft", XK_Left }, { "ArrowRight", XK_Right }, { "ArrowUp", XK_Up },
            { "ArrowDown", XK_Down }, { "Home", XK_Home }, { "End", XK_End } };

        KeySym ks = NoSymbol;
        for (auto& n : named) if (k == n.first) ks = n.second;
        if (ks == NoSymbol && k.length() == 1)
        {
            const auto c = (juce::uint32) k[0];
            ks = c < 0x100 ? (KeySym) c : (KeySym) (0x01000000 | c);
        }
        if (ks == NoSymbol) return;

        const KeyCode kc = x.KeysymToKeycode (dpy, ks);
        if (kc == 0) return;
        const bool shift = x.KeycodeToKeysym (dpy, kc, 0) != ks && x.KeycodeToKeysym (dpy, kc, 1) == ks;
        const KeyCode sh = x.KeysymToKeycode (dpy, XK_Shift_L);
        if (shift && sh) t.FakeKeyEvent (dpy, sh, True, 0);
        t.FakeKeyEvent (dpy, kc, True, 0);
        t.FakeKeyEvent (dpy, kc, False, 0);
        if (shift && sh) t.FakeKeyEvent (dpy, sh, False, 0);
        x.Flush (dpy);
    }

    std::mutex lock;
    Display* dpy = nullptr;
    Window root = 0;
    juce::String problem;

    struct X11
    {
        Display* (*OpenDisplay) (const char*) = nullptr;
        int (*CloseDisplay) (Display*) = nullptr;
        Window (*RootWin) (Display*) = nullptr;
        XImage* (*GetImage) (Display*, Drawable, int, int, unsigned, unsigned, unsigned long, int) = nullptr;
        int (*Flush) (Display*) = nullptr;
        KeyCode (*KeysymToKeycode) (Display*, KeySym) = nullptr;
        KeySym (*KeycodeToKeysym) (Display*, KeyCode, int) = nullptr;
    } x;

    struct XTst
    {
        int (*FakeMotionEvent) (Display*, int, int, int, unsigned long) = nullptr;
        int (*FakeButtonEvent) (Display*, unsigned, Bool, unsigned long) = nullptr;
        int (*FakeKeyEvent) (Display*, unsigned, Bool, unsigned long) = nullptr;
    } t;

    template <typename F> static void sym (void* lib, F& fn, const char* name) { fn = reinterpret_cast<F> (dlsym (lib, name)); }

    bool loadLibs()
    {
        if (x.OpenDisplay != nullptr) return true;
        void* lx = dlopen ("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
        if (lx == nullptr) { problem = "libX11 is missing (sudo apt install libx11-6)"; return false; }
        sym (lx, x.OpenDisplay, "XOpenDisplay");
        sym (lx, x.CloseDisplay, "XCloseDisplay");
        sym (lx, x.RootWin, "XDefaultRootWindow");
        sym (lx, x.GetImage, "XGetImage");
        sym (lx, x.Flush, "XFlush");
        sym (lx, x.KeysymToKeycode, "XKeysymToKeycode");
        sym (lx, x.KeycodeToKeysym, "XKeycodeToKeysym");
        if (! x.OpenDisplay || ! x.RootWin || ! x.GetImage || ! x.Flush || ! x.KeysymToKeycode || ! x.KeycodeToKeysym)
        {
            x.OpenDisplay = nullptr;
            problem = "libX11 is incomplete";
            return false;
        }
        if (void* lt = dlopen ("libXtst.so.6", RTLD_NOW))
        {
            sym (lt, t.FakeMotionEvent, "XTestFakeMotionEvent");
            sym (lt, t.FakeButtonEvent, "XTestFakeButtonEvent");
            sym (lt, t.FakeKeyEvent, "XTestFakeKeyEvent");
            if (! t.FakeMotionEvent || ! t.FakeKeyEvent) t.FakeButtonEvent = nullptr;
        }
        return true;
    }
};

LiveView::LiveView() : impl (std::make_unique<Impl>()) {}
LiveView::~LiveView() = default;
juce::String LiveView::open (const juce::String& d)  { return impl->open (d); }
bool LiveView::canInput() const                       { return impl->canInput(); }
bool LiveView::grab (juce::Rectangle<int> a, int w, juce::uint64 s, Frame& f) { return impl->grab (a, w, s, f); }
void LiveView::pointer (juce::Point<int> p, int b, bool d) { impl->pointer (p, b, d); }
void LiveView::wheel (juce::Point<int> p, int dy)     { impl->wheel (p, dy); }
void LiveView::key (const juce::String& k)            { impl->key (k); }
juce::uint32 LiveView::pixelAt (int x, int y)         { return impl->pixelAt (x, y); }
