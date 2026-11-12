// Stand-alone tool (not part of the viewer build): the hand presets made from the model of handcurl.cpp
// (Open_Hand, Peace, Rock, Gun) as preset files of app_settings/plus/poser/hand_presets.
//   cl /nologo /EHsc /std:c++17 /W4 /WX handcurl_genpresets_test.cpp handcurl.cpp
//   handcurl_genpresets_test.exe <folder>           writes the four files into the folder
//   handcurl_genpresets_test.exe --check <folder>   compares the files of the folder with the model (exit 1: they differ)
//   handcurl_genpresets_test.exe                    --check on the shipped files of the viewer that the environment variable
//                                                   SELFTEST_VIEWER names (tools/selftest runs every *_test.cpp this way)
// Both hands, 30 joints; the right hand is the mirror of the left (-x, y, -z). The version of each file is
// namedHandFileVersion (handcurl.cpp): raise it there when a hand changes, or the users' copies are not replaced.
// Where a program built here cannot be run, tools/hand_presets.py does both (a port of the model that reads the numbers
// of handcurl.cpp).
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "handcurl.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace plus::handcurl;

static const char* DIGITS[HAND_DIGITS] = {"Thumb", "Index", "Middle", "Ring", "Pinky"};
// A file holds six digits of a number ("%.6g"): below 10 that is within 5e-6 of the model's.
static const double TOLERANCE = 1e-5;

static std::string jointName(int side, int digit, int joint)
{
    return std::string("mHand") + DIGITS[digit] + char('1' + joint) + (side == 0 ? "Left" : "Right");
}

static void writeJoint(std::FILE* f, const char* name, const Raw& r)
{
    std::fprintf(f, "    <key>%s</key>\n        <map>\n        <key>enabled</key>\n            <boolean>1</boolean>\n", name);
    std::fprintf(f, "        <key>jointBaseRotationIsZero</key>\n            <boolean>1</boolean>\n");
    std::fprintf(f, "        <key>position</key>\n            <array>\n                <real>0</real>\n                <real>0</real>\n                <real>0</real>\n            </array>\n");
    std::fprintf(f, "        <key>rotation</key>\n            <array>\n                <real>%.6g</real>\n                <real>%.6g</real>\n                <real>%.6g</real>\n            </array>\n",
                 r.x + 0.f, r.y + 0.f, r.z + 0.f);
    std::fprintf(f, "        <key>scale</key>\n            <array>\n                <real>0</real>\n                <real>0</real>\n                <real>0</real>\n            </array>\n        </map>\n");
}

static int writeAll(const std::string& folder)
{
    for (int n = 0; n < NAMED_HAND_COUNT; ++n)
    {
        const NamedHand which = static_cast<NamedHand>(n);
        const HandRaw   hands[2] = {makeNamedHand(which), mirrorHand(makeNamedHand(which))};  // left, right
        const std::string path = folder + "/" + namedHandFileName(which) + ".xml";
        std::FILE*        f    = std::fopen(path.c_str(), "wb");
        if (!f)
        {
            std::printf("cannot write %s\n", path.c_str());
            return 1;
        }
        std::fprintf(f, "<llsd>\n    <map>\n");
        for (int side = 0; side < 2; ++side)
            for (int d = 0; d < HAND_DIGITS; ++d)
                for (int k = 0; k < 3; ++k)
                    writeJoint(f, jointName(side, d, k).c_str(), hands[side].joint[d][k]);
        std::fprintf(f, "    <key>startFromTeePose</key>\n        <map>\n        <key>value</key>\n            <boolean>1</boolean>\n        </map>\n");
        std::fprintf(f, "    <key>version</key>\n        <map>\n        <key>value</key>\n            <integer>%d</integer>\n        </map>\n    </map>\n</llsd>\n",
                     namedHandFileVersion(which));
        std::fclose(f);
        std::printf("wrote %s (version %d)\n", path.c_str(), namedHandFileVersion(which));
    }
    return 0;
}

static bool readFile(const std::string& path, std::string& text)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char   buffer[4096];
    size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
        text.append(buffer, got);
    std::fclose(f);
    return true;
}

/// The count numbers of the first <array> or the one <integer> after "<key>what</key>" that follows "<key>key</key>" in the
/// file's text (the files are the ones this tool writes: one key a line, no key twice). False when they are not there.
static bool numbersAfter(const std::string& text, const std::string& key, const std::string& what, const char* tag, int count, double* out)
{
    const size_t keyAt = text.find("<key>" + key + "</key>");
    if (keyAt == std::string::npos)
        return false;
    size_t       at        = text.find("<key>" + what + "</key>", keyAt);
    const size_t nextJoint = text.find("<key>mHand", keyAt + 1);  // the numbers must be this key's own, not a later joint's
    if (at == std::string::npos || (nextJoint != std::string::npos && at > nextJoint))
        return false;
    const std::string open  = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    for (int i = 0; i < count; ++i)
    {
        at = text.find(open, at);
        if (at == std::string::npos)
            return false;
        at += open.size();
        const size_t end = text.find(close, at);
        if (end == std::string::npos || end == at)
            return false;
        const std::string number = text.substr(at, end - at);
        char*             stop   = nullptr;
        out[i]                   = std::strtod(number.c_str(), &stop);
        if (stop != number.c_str() + number.size() || !std::isfinite(out[i]))
            return false;
        at = end;
    }
    return true;
}

/// Compares the four files of the folder with the model. Returns the number of differences (a file or a number that
/// cannot be read counts as one: nothing is passed over).
static int checkAll(const std::string& folder)
{
    int failures = 0;
    for (int n = 0; n < NAMED_HAND_COUNT; ++n)
    {
        const NamedHand   which    = static_cast<NamedHand>(n);
        const HandRaw     hands[2] = {makeNamedHand(which), mirrorHand(makeNamedHand(which))};  // left, right
        const std::string path     = folder + "/" + namedHandFileName(which) + ".xml";
        std::string       text;
        if (!readFile(path, text))
        {
            std::printf("FAIL cannot read %s\n", path.c_str());
            ++failures;
            continue;
        }
        for (int side = 0; side < 2; ++side)
            for (int d = 0; d < HAND_DIGITS; ++d)
                for (int k = 0; k < 3; ++k)
                {
                    const std::string name = jointName(side, d, k);
                    const Raw&        want = hands[side].joint[d][k];
                    double            got[3] = {0., 0., 0.};
                    if (!numbersAfter(text, name, "rotation", "real", 3, got))
                    {
                        std::printf("FAIL %s: no rotation of %s\n", path.c_str(), name.c_str());
                        ++failures;
                    }
                    else if (std::fabs(got[0] - want.x) > TOLERANCE || std::fabs(got[1] - want.y) > TOLERANCE || std::fabs(got[2] - want.z) > TOLERANCE)
                    {
                        std::printf("FAIL %s: %s is %.6g %.6g %.6g in the file, %.6g %.6g %.6g by makeNamedHand\n", path.c_str(), name.c_str(), got[0], got[1],
                                    got[2], want.x + 0.f, want.y + 0.f, want.z + 0.f);
                        ++failures;
                    }
                }
        double version = 0.;
        if (!numbersAfter(text, "version", "value", "integer", 1, &version) || version != namedHandFileVersion(which))
        {
            std::printf("FAIL %s: the version is not %d (namedHandFileVersion)\n", path.c_str(), namedHandFileVersion(which));
            ++failures;
        }
    }
    if (failures)
        std::printf("FAILED (%d): write the files again (this tool with the folder, or tools/hand_presets.py --write)\n", failures);
    else
        std::printf("ok (%d files of %s are what makeNamedHand gives)\n", NAMED_HAND_COUNT, folder.c_str());
    return failures;
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) != "--check")
        return writeAll(argv[1]);
    if (argc == 3 && std::string(argv[1]) == "--check")
        return checkAll(argv[2]) ? 1 : 0;
    const char* viewer = std::getenv("SELFTEST_VIEWER");
    if (argc == 1 && viewer && *viewer)
        return checkAll(std::string(viewer) + "/indra/newview/app_settings/plus/poser/hand_presets") ? 1 : 0;
    std::printf("usage: handcurl_genpresets_test <folder> | --check <folder> | (no argument, SELFTEST_VIEWER set: --check of its shipped files)\n");
    return 2;
}
