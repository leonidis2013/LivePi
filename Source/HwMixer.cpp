#include "HwMixer.h"
#include "HostEngine.h"
#include <alsa/asoundlib.h>
#include <mutex>

using namespace juce;

namespace
{
    std::mutex fileLock;

    File savedFile() { return HostEngine::dataDir().getChildFile ("hwmixer.json"); }

    struct Mixer
    {
        snd_mixer_t* h = nullptr;

        explicit Mixer (int card)
        {
            char name[32];
            snprintf (name, sizeof (name), "hw:%d", card);
            if (snd_mixer_open (&h, 0) < 0) { h = nullptr; return; }
            if (snd_mixer_attach (h, name) < 0 || snd_mixer_selem_register (h, nullptr, nullptr) < 0 || snd_mixer_load (h) < 0)
            {
                snd_mixer_close (h);
                h = nullptr;
            }
        }
        ~Mixer() { if (h != nullptr) snd_mixer_close (h); }

        snd_mixer_elem_t* find (const String& control, int index) const
        {
            if (h == nullptr) return nullptr;
            snd_mixer_selem_id_t* sid = nullptr;
            snd_mixer_selem_id_alloca (&sid);
            snd_mixer_selem_id_set_name (sid, control.toRawUTF8());
            snd_mixer_selem_id_set_index (sid, (unsigned) index);
            return snd_mixer_find_selem (h, sid);
        }
    };

    String cardId (int card)
    {
        return File ("/proc/asound/card" + String (card) + "/id").loadFileAsString().trim();
    }

    String cardName (int card)
    {
        char* n = nullptr;
        String s;
        if (snd_card_get_name (card, &n) == 0 && n != nullptr) { s = n; free (n); }
        return s;
    }

    int cardIndexForId (const String& id)
    {
        int card = -1;
        while (snd_card_next (&card) == 0 && card >= 0)
            if (cardId (card) == id)
                return card;
        return -1;
    }

    var volumeInfo (snd_mixer_elem_t* e, bool capture)
    {
        long mn = 0, mx = 0, v = 0;
        if (capture) { snd_mixer_selem_get_capture_volume_range (e, &mn, &mx); snd_mixer_selem_get_capture_volume (e, SND_MIXER_SCHN_FRONT_LEFT, &v); }
        else         { snd_mixer_selem_get_playback_volume_range (e, &mn, &mx); snd_mixer_selem_get_playback_volume (e, SND_MIXER_SCHN_FRONT_LEFT, &v); }

        auto* o = new DynamicObject();
        o->setProperty ("min", (int) mn);
        o->setProperty ("max", (int) mx);
        o->setProperty ("value", (int) v);

        long dmn = 0, dmx = 0, dv = 0;
        const bool db = capture ? (snd_mixer_selem_get_capture_dB_range (e, &dmn, &dmx) == 0 && snd_mixer_selem_get_capture_dB (e, SND_MIXER_SCHN_FRONT_LEFT, &dv) == 0)
                                : (snd_mixer_selem_get_playback_dB_range (e, &dmn, &dmx) == 0 && snd_mixer_selem_get_playback_dB (e, SND_MIXER_SCHN_FRONT_LEFT, &dv) == 0);
        if (db && dmx > dmn)
        {
            o->setProperty ("dB", dv / 100.0);
            o->setProperty ("minDb", dmn / 100.0);
            o->setProperty ("maxDb", dmx / 100.0);
        }
        return var (o);
    }

    var controlsOf (int card)
    {
        Array<var> out;
        Mixer m (card);
        if (m.h == nullptr)
            return out;

        for (auto* e = snd_mixer_first_elem (m.h); e != nullptr; e = snd_mixer_elem_next (e))
        {
            if (! snd_mixer_selem_is_active (e))
                continue;
            const String name (snd_mixer_selem_get_name (e));
            const int index = (int) snd_mixer_selem_get_index (e);

            if (snd_mixer_selem_is_enumerated (e))
            {
                const int n = snd_mixer_selem_get_enum_items (e);
                if (n <= 0) continue;
                Array<var> items;
                for (int i = 0; i < n; ++i)
                {
                    char buf[64] = {};
                    snd_mixer_selem_get_enum_item_name (e, (unsigned) i, sizeof (buf) - 1, buf);
                    items.add (String (buf));
                }
                unsigned cur = 0;
                snd_mixer_selem_get_enum_item (e, SND_MIXER_SCHN_FRONT_LEFT, &cur);
                auto* o = new DynamicObject();
                o->setProperty ("control", name);
                o->setProperty ("index", index);
                o->setProperty ("kind", "enum");
                o->setProperty ("items", items);
                o->setProperty ("item", (int) cur);
                out.add (var (o));
                continue;
            }

            for (bool capture : { false, true })
            {
                // A volume shared by playback and capture is listed once, as playback
                const bool vol = capture ? (snd_mixer_selem_has_capture_volume (e) && ! snd_mixer_selem_has_common_volume (e))
                                         : snd_mixer_selem_has_playback_volume (e);
                const bool sw  = capture ? snd_mixer_selem_has_capture_switch (e) : snd_mixer_selem_has_playback_switch (e);
                if (! vol && ! sw)
                    continue;

                auto* o = new DynamicObject();
                o->setProperty ("control", name);
                o->setProperty ("index", index);
                o->setProperty ("kind", capture ? "capture" : "playback");
                if (vol)
                    o->setProperty ("volume", volumeInfo (e, capture));
                if (sw)
                {
                    int on = 0;
                    if (capture) snd_mixer_selem_get_capture_switch (e, SND_MIXER_SCHN_FRONT_LEFT, &on);
                    else         snd_mixer_selem_get_playback_switch (e, SND_MIXER_SCHN_FRONT_LEFT, &on);
                    o->setProperty ("on", on != 0);
                }
                out.add (var (o));
            }
        }
        return out;
    }

    String apply (int card, const var& b)
    {
        Mixer m (card);
        if (m.h == nullptr)
            return "Could not open the interface's mixer";
        auto* e = m.find (b["control"].toString(), (int) b.getProperty ("index", 0));
        if (e == nullptr)
            return "No such control";

        const String kind = b["kind"].toString();
        if (kind == "enum")
        {
            if (! b.hasProperty ("item")) return {};
            const auto item = (unsigned) (int) b["item"];
            if (snd_mixer_selem_set_enum_item (e, SND_MIXER_SCHN_FRONT_LEFT, item) < 0)
                return "Could not change it";
            for (int ch = 1; ch <= (int) SND_MIXER_SCHN_LAST; ++ch)
                if (snd_mixer_selem_set_enum_item (e, (snd_mixer_selem_channel_id_t) ch, item) < 0)
                    break;
            return {};
        }

        const bool capture = kind == "capture";
        if (b.hasProperty ("value"))
        {
            long mn = 0, mx = 0;
            if (capture) snd_mixer_selem_get_capture_volume_range (e, &mn, &mx);
            else         snd_mixer_selem_get_playback_volume_range (e, &mn, &mx);
            const long v = jlimit (mn, mx, (long) (int) b["value"]);
            const int r = capture ? snd_mixer_selem_set_capture_volume_all (e, v) : snd_mixer_selem_set_playback_volume_all (e, v);
            if (r < 0) return "Could not change the level";
        }
        if (b.hasProperty ("on"))
        {
            const int on = (bool) b["on"] ? 1 : 0;
            const int r = capture ? snd_mixer_selem_set_capture_switch_all (e, on) : snd_mixer_selem_set_playback_switch_all (e, on);
            if (r < 0) return "Could not switch it";
        }
        return {};
    }

    String keyOf (const var& b)
    {
        return b["control"].toString() + "|" + String ((int) b.getProperty ("index", 0)) + "|" + b["kind"].toString();
    }
}

var HwMixer::list (const StringArray& deviceNames)
{
    Array<var> cards;
    int card = -1;
    while (snd_card_next (&card) == 0 && card >= 0)
    {
        const String id = cardId (card), name = cardName (card);
        bool inUse = false;
        for (auto& d : deviceNames)
            if (d.isNotEmpty() && ((id.isNotEmpty() && d.containsIgnoreCase (id)) || (name.isNotEmpty() && d.containsIgnoreCase (name))
                                   || d.containsIgnoreCase ("hw:" + String (card) + ",") || d.endsWithIgnoreCase ("hw:" + String (card))))
                inUse = true;

        auto* o = new DynamicObject();
        o->setProperty ("card", id);
        o->setProperty ("name", name.isNotEmpty() ? name : id);
        o->setProperty ("inUse", inUse);
        o->setProperty ("pisound", id.equalsIgnoreCase ("pisound"));
        o->setProperty ("controls", controlsOf (card));
        cards.add (var (o));
    }
    auto* r = new DynamicObject();
    r->setProperty ("ok", true);
    r->setProperty ("cards", cards);
    return var (r);
}

var HwMixer::set (const var& b)
{
    auto* r = new DynamicObject();
    const String id = b["card"].toString();
    const int card = cardIndexForId (id);
    String e = card < 0 ? String ("That interface is not connected") : apply (card, b);
    r->setProperty ("ok", e.isEmpty());
    if (e.isNotEmpty())
    {
        r->setProperty ("error", e);
        return var (r);
    }

    // Remember it (per interface), so it comes back after a restart
    std::lock_guard<std::mutex> l (fileLock);
    auto saved = JSON::parse (savedFile());
    if (! saved.isObject()) saved = var (new DynamicObject());
    auto c = saved[Identifier (id)];
    if (! c.isObject()) { c = var (new DynamicObject()); saved.getDynamicObject()->setProperty (id, c); }
    auto entry = c[Identifier (keyOf (b))];
    if (! entry.isObject()) { entry = var (new DynamicObject()); c.getDynamicObject()->setProperty (keyOf (b), entry); }
    for (auto p : { "control", "index", "kind", "value", "on", "item" })
        if (b.hasProperty (p))
            entry.getDynamicObject()->setProperty (p, b[p]);
    savedFile().replaceWithText (JSON::toString (saved, true));
    return var (r);
}

void HwMixer::restore()
{
    var saved;
    {
        std::lock_guard<std::mutex> l (fileLock);
        saved = JSON::parse (savedFile());
    }
    auto* cards = saved.getDynamicObject();
    if (cards == nullptr)
        return;
    for (auto& c : cards->getProperties())
    {
        const int card = cardIndexForId (c.name.toString());
        auto* entries = c.value.getDynamicObject();
        if (card < 0 || entries == nullptr)
            continue;
        for (auto& en : entries->getProperties())
        {
            auto err = apply (card, en.value);
            if (err.isNotEmpty())
                Logger::writeToLog ("Interface level " + en.name.toString() + ": " + err);
        }
    }
}
