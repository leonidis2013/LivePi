#pragma once
#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>
#include <mutex>

// Offline update: validates an unpacked update package and swaps it into the install folder.
// The old program and web files are kept as LivePi.prev / web.prev so run.sh can roll back.
namespace Updater
{
    // Unpack an uploaded or downloaded zip into dest, refusing anything that could escape it.
    inline juce::String extractZipSafely (const juce::File& zipFile, const juce::File& dest)
    {
        using namespace juce;

        ZipFile zip (zipFile);
        const int n = zip.getNumEntries();
        if (n <= 0)
            return "That file is not a valid zip, or it is empty.";
        if (n > 20000)
            return "The zip has too many files.";

        int64 total = 0;
        for (int i = 0; i < n; ++i)
        {
            auto* e = zip.getEntry (i);
            const String name = e->filename;

            if (e->isSymbolicLink)
                return "The zip contains symbolic links, which are not allowed.";
            if (name.containsChar ('\\'))
                return "The zip uses Windows-style paths. Re-zip it with 7-Zip, macOS Archive Utility or the zip command.";
            if (name.startsWithChar ('/') || name.containsChar (':'))
                return "Unsafe path in zip: " + name;
            for (auto& part : StringArray::fromTokens (name, "/", ""))
                if (part == "..")
                    return "Unsafe path in zip: " + name;

            total += e->uncompressedSize;
        }

        if (total > (int64) 1500 * 1024 * 1024)
            return "The zip is too large when unpacked (limit 1.5 GB).";

        dest.createDirectory();
        for (int i = 0; i < n; ++i)
        {
            auto r = zip.uncompressEntry (i, dest, true);
            if (r.failed())
                return "Could not unpack the zip: " + r.getErrorMessage();
        }
        return {};
    }

    // Only one update may touch the install folder at a time
    inline std::mutex& applyLock()
    {
        static std::mutex m;
        return m;
    }

    // Returns an error message, or an empty string on success (versionOut is then set).
    inline juce::String apply (const juce::File& unpacked, const juce::File& installDir,
                               const juce::String& exeName, const juce::File& dataDir,
                               juce::String& versionOut)
    {
        using namespace juce;

        std::unique_lock<std::mutex> guard (applyLock(), std::try_to_lock);
        if (! guard.owns_lock())
            return "Another update is already being installed.";

        // Find <root>/LivePi with <root>/web/index.html next to it
        File root;
        for (auto& f : unpacked.findChildFiles (File::findFiles, true, exeName))
            if (f.getSiblingFile ("web").getChildFile ("index.html").existsAsFile())
            {
                root = f.getParentDirectory();
                break;
            }

        if (root == File())
            return "This zip does not look like a LivePi update (no program with a web folder).";

        auto newBin = root.getChildFile (exeName);
        auto manifest = JSON::parse (root.getChildFile ("manifest.json"));
        if (! manifest.isObject())
            return "The zip has no manifest.json. Use the livepi-build zip from GitHub.";

        // Must be a 64-bit little-endian ARM Linux program (e_machine 0xB7 = AArch64)
        {
            FileInputStream in (newBin);
            uint8 h[20] = {};
            if (in.failedToOpen() || in.read (h, 20) != 20 || h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F')
                return "The program inside the zip is not a Linux program.";
            if (h[4] != 2 || h[5] != 1 || (h[18] | (h[19] << 8)) != 0xB7)
                return "The program inside the zip is not built for 64-bit ARM (Raspberry Pi).";
        }

        if (juce::SHA256 (newBin).toHexString() != manifest["sha256"].toString().toLowerCase())
            return "The program inside the zip is damaged (checksum mismatch). Download it again.";

        const int have = installDir.getChildFile ("INSTALLER_LEVEL").loadFileAsString().trim().getIntValue();
        const int need = (int) manifest.getProperty ("installerLevel", 1);
        if (need > have)
            return "This update needs the newer installer. On the Pi, unzip it and run: sudo ./install-pi.sh <your-user>";

        if (! installDir.hasWriteAccess())
            return "LivePi cannot write to " + installDir.getFullPathName() + ". Re-run the installer once to fix permissions.";

        auto exe = installDir.getChildFile (exeName);
        auto prevBin = installDir.getChildFile (exeName + ".prev");
        auto web = installDir.getChildFile ("web");
        auto prevWeb = installDir.getChildFile ("web.prev");

        prevBin.deleteFile();
        prevWeb.deleteRecursively();

        auto restore = [&]
        {
            exe.deleteFile();
            prevBin.moveFileTo (exe);
            web.deleteRecursively();
            if (prevWeb.isDirectory())
                prevWeb.moveFileTo (web);
        };

        if (! exe.moveFileTo (prevBin))
            return "Could not back up the current program.";

        if (web.isDirectory() && ! web.moveFileTo (prevWeb))
        {
            prevBin.moveFileTo (exe);
            return "Could not back up the current web files.";
        }

        if (! newBin.copyFileTo (exe) || ! root.getChildFile ("web").copyDirectoryTo (web))
        {
            restore();
            return "Could not copy the new files into place.";
        }

        exe.setExecutePermission (true);

        // run.sh rolls back automatically if the new version keeps failing to start
        dataDir.getChildFile ("update-pending").replaceWithText ("0");
        dataDir.getChildFile ("rolled-back").deleteFile();

        versionOut = root.getChildFile ("VERSION").loadFileAsString().trim();
        if (versionOut.isEmpty())
            versionOut = manifest["build"].toString();
        return {};
    }
}
