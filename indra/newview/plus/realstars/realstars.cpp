/**
 * @file realstars.cpp
 * @brief The real night sky (research/177-real-sky-stars.md): the catalogue into the star field,
 *        and the turn of the sky for the place and the time.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "realstars.h"
#include "realskymath.h"
#include "realskytime.h"
#include "realstarart.h"
#include "realstarcat.h"

#include "plus/starfield/starfield.h"
#include "plus/starfield/starfieldmath.h"

#include "lldir.h"
#include "llfile.h"
#include "llrhi.h"
#include "llstring.h"
#include "llviewercontrol.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <vector>

namespace plus::realstars
{
    namespace
    {
        constexpr char SETTING_REAL[] = "PlusSkyStarsReal";
        constexpr char SETTING_LAT[] = "PlusSkyStarsLat";
        constexpr char SETTING_LON[] = "PlusSkyStarsLon";
        constexpr char SETTING_TIME_MODE[] = "PlusSkyStarsTimeMode";
        constexpr char SETTING_TIME[] = "PlusSkyStarsTime";
        constexpr char SETTING_LINES[] = "PlusSkyStarsLines";
        constexpr char SETTING_LINE_ALPHA[] = "PlusSkyStarsLineAlpha";
        constexpr char SETTING_LINE_WIDTH[] = "PlusSkyStarsLineWidth";
        constexpr char SETTING_ART[] = "PlusSkyStarsArt";                  // research/288
        constexpr char SETTING_ART_ALPHA[] = "PlusSkyStarsArtAlpha";
        constexpr F32 DEFAULT_ART_ALPHA = 0.5f;

        constexpr char SWITCH_ENV[] = "LLRHI_REAL_STARS";          // "0": the made-up sky always
        constexpr char CUE_REAL[] = "realstars.toggle";            // LLRHI_CUE_DIR: PlusSkyStarsReal flipped, not saved
        constexpr char CUE_LINES[] = "realstarlines.toggle";       // PlusSkyStarsLines flipped, not saved
        constexpr char CUE_ART[] = "realstarart.toggle";           // PlusSkyStarsArt flipped, not saved
        constexpr F64 CUE_POLL_SECONDS = 1.0;

        constexpr char CATALOG_FILE[] = "starcat-bsc5.bin";
        constexpr char LINES_FILE[] = "starcat-lines.bin";
        constexpr char ART_FILE[] = "starart.bin";                 // research/288: the plates
        constexpr char ART_PICTURES_FILE[] = "starart-tex.bin";    // and the pictures

        // PlusSkyStarsTimeMode
        constexpr S32 TIME_FOLLOW_SL = 0;   // turned with SL's sun (research/177 §2.1 (b))
        constexpr S32 TIME_NOW = 1;         // the real time now
        constexpr S32 TIME_GIVEN = 2;       // PlusSkyStarsTime

        // The defaults of settings_plus.xml: Tokyo.
        constexpr F32 DEFAULT_LAT = 35.68f;
        constexpr F32 DEFAULT_LON = 139.77f;

        // The galactic north pole and centre, J2000 (research/177 §3.1).
        constexpr double GALACTIC_POLE_RA = 192.85948, GALACTIC_POLE_DEC = 27.12825;
        constexpr double GALACTIC_CENTRE_RA = 266.40500, GALACTIC_CENTRE_DEC = -28.93617;

        // When the turn is worked out again: the real time by a second (the sky turns 15" a
        // second, a pixel in about ten), the date the sun's place is taken for by an hour.
        constexpr double NEW_TURN_SECONDS = 1.0;
        constexpr double NEW_DATE_DAYS = 1.0 / 24.0;

        bool switchAllowed()
        {
            static const bool allowed = LLRHI::envDefaultOn(SWITCH_ENV);
            return allowed;
        }

        // The whole file, at most maxBytes (a larger one is not read: false). Read in one go (the
        // pictures are about 15 MB).
        bool readFile(const std::string& path, std::vector<U8>& bytes, size_t maxBytes = 1u << 20)
        {
            bytes.clear();
            llifstream stream(path.c_str(), std::ios::binary);   // UTF-8 paths (a Japanese user name)
            if (!stream.is_open())
            {
                return false;
            }
            stream.seekg(0, std::ios::end);
            const std::streamoff size = stream.tellg();
            if (size <= 0 || (U64)size > (U64)maxBytes)
            {
                return false;
            }
            stream.seekg(0, std::ios::beg);
            bytes.resize((size_t)size);
            if (!stream.read((char*)bytes.data(), size))
            {
                bytes.clear();
                return false;
            }
            return true;
        }

        LLVector3 toLL(const starfield::math::Vec3& v) { return LLVector3(v.x, v.y, v.z); }
        LLColor4 toLL(const starfield::math::Color& c) { return LLColor4(c.r, c.g, c.b, c.a); }

        starfield::math::Vec3 unitOf(double raDeg, double decDeg)
        {
            const math::Vec3d u = math::unitFromRaDec(raDeg, decDeg);
            return { (float)u.x, (float)u.y, (float)u.z };
        }

        // A star of the catalogue as the star field draws it (research/177 §3.3): the light of
        // the made-up sky's scale (magnitude -1 the brightest: Sirius and Canopus alike), the
        // tint of its temperature.
        LLColor4 colorOf(const CatalogStar& star)
        {
            namespace sm = starfield::math;
            const float mag = llmax(star.vmag, sm::MAG_BRIGHTEST);
            const float light = std::pow(10.f, -0.4f * sm::MAG_COMPRESSION * (mag - sm::MAG_BRIGHTEST));
            sm::Color tint = sm::tintOfTemperature((float)math::temperatureOfBV(star.bv));
            tint.a = sm::levelOfLight(light);
            return toLL(tint);
        }

        void cue(const char* name, const char* setting)
        {
            if (!LLRHI::takeCue(name))
            {
                return;
            }
            LLControlVariable* control = gSavedSettings.getControl(setting);
            if (!control)
            {
                return;
            }
            const bool on = control->isType(TYPE_BOOLEAN) ? !control->getValue().asBoolean() : control->getValue().asInteger() == 0;
            control->setValue(control->isType(TYPE_BOOLEAN) ? LLSD(on) : LLSD((LLSD::Integer)(on ? 1 : 0)), false);
            LL_INFOS("RHIProfile") << "real stars (research/177): " << setting << " " << (on ? "on" : "off")
                                   << (switchAllowed() ? "" : " (held off by LLRHI_REAL_STARS=0)") << LL_ENDL;
        }

        double unixNow()
        {
            using namespace std::chrono;
            return duration<double>(system_clock::now().time_since_epoch()).count();
        }

        // The PC's offset from UTC now (seconds), for dates the C library cannot convert.
        double localOffsetNow()
        {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
            std::tm utc{};
#if LL_WINDOWS
            if (localtime_s(&local, &now) != 0 || gmtime_s(&utc, &now) != 0)
#else
            if (!localtime_r(&now, &local) || !gmtime_r(&now, &utc))
#endif
            {
                return 0.0;
            }
            const timeutil::Civil l{ local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec };
            const timeutil::Civil u{ utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec };
            return timeutil::unixOfUtc(l) - timeutil::unixOfUtc(u);
        }

        // The PC's local time of a Unix time (its daylight saving included where the C library
        // knows the date; before 1970 and far ahead, today's offset).
        timeutil::Civil localOfUnix(double unixSeconds)
        {
            const std::time_t t = (std::time_t)std::floor(unixSeconds);
            std::tm tm{};
#if LL_WINDOWS
            const bool known = unixSeconds >= 0.0 && localtime_s(&tm, &t) == 0;
#else
            const bool known = unixSeconds >= 0.0 && localtime_r(&t, &tm) != nullptr;
#endif
            if (!known)
            {
                return timeutil::utcOfUnix(unixSeconds + localOffsetNow());
            }
            return { tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec };
        }

        double unixOfLocal(const timeutil::Civil& c)
        {
            std::tm tm{};
            tm.tm_year = c.year - 1900;
            tm.tm_mon = c.month - 1;
            tm.tm_mday = c.day;
            tm.tm_hour = c.hour;
            tm.tm_min = c.minute;
            tm.tm_sec = c.second;
            tm.tm_isdst = -1;   // the C library knows whether daylight saving is on that day
            const std::time_t t = c.year >= 1970 ? std::mktime(&tm) : (std::time_t)-1;
            if (t == (std::time_t)-1)
            {
                return timeutil::unixOfUtc(c) - localOffsetNow();
            }
            return (double)t;
        }

        // What the turn was worked out for, and the turn.
        struct State
        {
            bool valid = false;
            S32 mode = -1;
            F32 lat = 0.f;
            F32 lon = 0.f;
            LLVector3 sun;
            double jd = 0.0;            // the time (or for TIME_FOLLOW_SL the date) it is for
            std::string time_text;      // PlusSkyStarsTime as last read
            bool time_given = false;    // it was a time
            double time_unix = 0.0;
            Frame frame{};
        };
        State sState;

        void setFrame(const math::Mat3d& m, Frame& frame)
        {
            for (int row = 0; row < 3; ++row)
            {
                for (int col = 0; col < 3; ++col)
                {
                    frame.gl[col * 4 + row] = (F32)m.m[row][col];   // column-major
                }
                frame.gl[3 * 4 + row] = 0.f;
                frame.gl[row * 4 + 3] = 0.f;
            }
            frame.gl[15] = 1.f;
            frame.zenith.set((F32)m.m[2][0], (F32)m.m[2][1], (F32)m.m[2][2]);
            frame.zenith.normVec();
        }
    }

    bool wanted()
    {
        static LLCachedControl<S32> real(gSavedSettings, SETTING_REAL, 0);
        return real() == 1 && switchAllowed();
    }

    void update()
    {
        static LLRHI::CuePoll cue_poll(CUE_POLL_SECONDS);
        if (!cue_poll.due())
        {
            return;
        }
        cue(CUE_REAL, SETTING_REAL);
        cue(CUE_LINES, SETTING_LINES);
        cue(CUE_ART, SETTING_ART);
        starfield::pollCues();
    }

    bool fill(S32 starMode, starfield::Field& field)
    {
        field.clear();
        std::vector<U8> bytes;
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "plus", CATALOG_FILE);
        Catalog catalog;
        if (!readFile(path, bytes))
        {
            LL_WARNS("Plus") << "real stars: cannot read " << CATALOG_FILE << "; the made-up sky is drawn" << LL_ENDL;
            return false;
        }
        const CatalogResult result = parseCatalog(bytes.data(), bytes.size(), catalog);
        if (result != CatalogResult::Ok || catalog.stars.empty())
        {
            LL_WARNS("Plus") << "real stars: " << CATALOG_FILE << " is not a star catalogue (" << (S32)result
                             << "); the made-up sky is drawn" << LL_ENDL;
            return false;
        }

        field.star_dir.reserve(catalog.stars.size() + starfield::math::BAND_STAR_COUNT);
        field.star_color.reserve(catalog.stars.size() + starfield::math::BAND_STAR_COUNT);
        for (const CatalogStar& star : catalog.stars)
        {
            field.star_dir.emplace_back(star.dir[0], star.dir[1], star.dir[2]);
            field.star_color.push_back(colorOf(star));
        }

        if (starMode >= starfield::MODE_MILKY_WAY)
        {
            // research/177 §3.1: the made-up Milky Way (patches and its band's faint stars), on the real plane
            starfield::math::Catalogue milky_way;
            starfield::math::generateMilkyWayOn(unitOf(GALACTIC_POLE_RA, GALACTIC_POLE_DEC),
                                                unitOf(GALACTIC_CENTRE_RA, GALACTIC_CENTRE_DEC), milky_way);
            for (size_t i = 0; i < milky_way.star_dir.size(); ++i)
            {
                field.star_dir.push_back(toLL(milky_way.star_dir[i]));
                field.star_color.push_back(toLL(milky_way.star_color[i]));
            }
            // <plus> research/224: with PlusSkyStarsMilkyWayHQ the band is the baked picture
            // (starfield::MilkyBaker), not patches; its faint stars stay
            if (!starfield::bakedMilkyWay(starMode))
            // </plus>
            {
                field.glow_dir.reserve(milky_way.glow_dir.size());
                field.glow_color.reserve(milky_way.glow_color.size());
                for (size_t i = 0; i < milky_way.glow_dir.size(); ++i)
                {
                    field.glow_dir.push_back(toLL(milky_way.glow_dir[i]));
                    field.glow_color.push_back(toLL(milky_way.glow_color[i]));
                }
                field.glow_radius = milky_way.glow_radius;
            }
        }

        // <plus> research/224: PlusSkyStarsDense, about 40,000 faint stars more, gathered along the
        // real plane, over the whole sphere (the real sky turns)
        if (starfield::dense())
        {
            starfield::math::Catalogue faint;
            starfield::math::generateDenseOn(unitOf(GALACTIC_POLE_RA, GALACTIC_POLE_DEC),
                                             unitOf(GALACTIC_CENTRE_RA, GALACTIC_CENTRE_DEC), faint, true);
            field.star_dir.reserve(field.star_dir.size() + faint.star_dir.size());
            field.star_color.reserve(field.star_color.size() + faint.star_color.size());
            for (size_t i = 0; i < faint.star_dir.size(); ++i)
            {
                field.star_dir.push_back(toLL(faint.star_dir[i]));
                field.star_color.push_back(toLL(faint.star_color[i]));
            }
        }
        // </plus>

        // The constellation lines: a file of their own, and none is no error (the sky without them).
        const std::string lines_path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "plus", LINES_FILE);
        Lines lines;
        if (readFile(lines_path, bytes))
        {
            const LinesResult lines_result = parseLines(bytes.data(), bytes.size(), catalog, lines);
            if (lines_result != LinesResult::Ok)
            {
                LL_WARNS("Plus") << "real stars: " << LINES_FILE << " not used (" << (S32)lines_result << ")" << LL_ENDL;
            }
        }
        const size_t segments = lines.ends.size() / 2;
        // research/177 §2.7: every star the lines join (the stars come first in the field, by
        // the catalogue's order), drawn a little brighter whether or not the lines are
        field.star_mark.assign(field.star_dir.size(), 0);
        for (const std::uint16_t end : lines.ends)
        {
            field.star_mark[end] = 1;
        }
        field.line_a.reserve(segments);
        field.line_b.reserve(segments);
        for (size_t i = 0; i < segments; ++i)
        {
            const CatalogStar& a = catalog.stars[lines.ends[2 * i]];
            const CatalogStar& b = catalog.stars[lines.ends[2 * i + 1]];
            field.line_a.emplace_back(a.dir[0], a.dir[1], a.dir[2]);
            field.line_b.emplace_back(b.dir[0], b.dir[1], b.dir[2]);
        }

        LL_INFOS("Plus") << "real stars: " << catalog.stars.size() << " stars, " << field.glow_dir.size()
                         << " Milky Way patches, " << segments << " constellation line segments" << LL_ENDL;
        return true;
    }

    void galacticPlane(LLVector3& pole, LLVector3& centre)
    {
        pole = toLL(unitOf(GALACTIC_POLE_RA, GALACTIC_POLE_DEC));
        centre = toLL(unitOf(GALACTIC_CENTRE_RA, GALACTIC_CENTRE_DEC));
    }

    const Frame& frame(const LLVector3& sunDir)
    {
        static LLCachedControl<S32> time_mode(gSavedSettings, SETTING_TIME_MODE, TIME_FOLLOW_SL);
        static LLCachedControl<F32> lat_setting(gSavedSettings, SETTING_LAT, DEFAULT_LAT);
        static LLCachedControl<F32> lon_setting(gSavedSettings, SETTING_LON, DEFAULT_LON);
        static LLCachedControl<std::string> time_setting(gSavedSettings, SETTING_TIME, "");

        State& s = sState;
        const S32 mode = llclamp((S32)time_mode(), TIME_FOLLOW_SL, TIME_GIVEN);
        const F32 lat = llfinite((F32)lat_setting()) ? llclamp((F32)lat_setting(), -90.f, 90.f) : DEFAULT_LAT;
        const F32 lon = llfinite((F32)lon_setting()) ? (F32)math::wrapDeg180(llclamp((F32)lon_setting(), -360.f, 360.f)) : DEFAULT_LON;
        const std::string& time_text = time_setting();
        if (time_text != s.time_text)
        {
            s.time_text = time_text;    // copied only when it changed
            s.time_given = timeutil::parseUtc(time_text, s.time_unix);
        }

        // the time: given, or now; for following SL's sun only the date counts (the sun's place, the precession)
        const double jd = math::julianDayFromUnix((mode != TIME_NOW && s.time_given) ? s.time_unix : unixNow());
        bool again = !s.valid || mode != s.mode || lat != s.lat || lon != s.lon;
        if (mode == TIME_FOLLOW_SL)
        {
            again = again || sunDir != s.sun || std::fabs(jd - s.jd) >= NEW_DATE_DAYS;
        }
        else
        {
            again = again || std::fabs(jd - s.jd) * 86400.0 >= NEW_TURN_SECONDS;
        }
        if (!again)
        {
            return s.frame;
        }

        math::Mat3d m;
        if (mode == TIME_FOLLOW_SL)
        {
            m = math::skyMatrixFollowingSun(jd, { sunDir.mV[VX], sunDir.mV[VY], sunDir.mV[VZ] }, lat);
        }
        else
        {
            m = math::skyMatrix(jd, lon, lat);
        }
        setFrame(m, s.frame);
        s.valid = true;
        s.mode = mode;
        s.lat = lat;
        s.lon = lon;
        s.sun = sunDir;
        s.jd = jd;
        return s.frame;
    }

    bool linesWanted()
    {
        static LLCachedControl<bool> lines(gSavedSettings, SETTING_LINES, false);
        return lines();
    }

    F32 lineAlpha()
    {
        static LLCachedControl<F32> setting(gSavedSettings, SETTING_LINE_ALPHA, 0.35f);
        const F32 value = setting;
        return llfinite(value) ? llclamp(value, 0.f, 1.f) : 0.35f;
    }

    F32 lineWidth()
    {
        static LLCachedControl<F32> setting(gSavedSettings, SETTING_LINE_WIDTH, 1.f);
        const F32 value = setting;
        return llfinite(value) ? llclamp(value, 0.5f, 3.f) : 1.f;
    }

    bool artWanted()
    {
        static LLCachedControl<bool> art(gSavedSettings, SETTING_ART, false);
        return art();
    }

    F32 artAlpha()
    {
        static LLCachedControl<F32> setting(gSavedSettings, SETTING_ART_ALPHA, DEFAULT_ART_ALPHA);
        const F32 value = setting;
        return llfinite(value) ? llclamp(value, 0.f, 1.f) : DEFAULT_ART_ALPHA;
    }

    void fillArt(Art& art)
    {
        art.plates.clear();
        std::vector<U8> bytes;
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "plus", ART_FILE);
        if (!readFile(path, bytes))
        {
            return;     // none: the sky without pictures
        }
        const ArtResult result = parseArt(bytes.data(), bytes.size(), art);
        if (result != ArtResult::Ok)
        {
            LL_WARNS("Plus") << "real stars: " << ART_FILE << " not used (" << (S32)result << ")" << LL_ENDL;
            return;
        }
        LL_INFOS("Plus") << "real stars: " << art.plates.size() << " constellation pictures" << LL_ENDL;
    }

    ArtPicturesReader::ArtPicturesReader(U32 layersNeeded)
        : mNeeded(layersNeeded)
    {
        // the path here (gDirUtilp is the main thread's); only the file is read on the thread
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "plus", ART_PICTURES_FILE);
        try
        {
            mThread = std::thread([this, path]()
            {
                S32 result = -1;
                try
                {
                    const size_t most = ART_TEX_HEADER_BYTES + (size_t)ART_MAX_LAYERS * artTexLayerBytes();
                    if (readFile(path, mBytes, most))
                    {
                        std::uint32_t count = 0;
                        const ArtTexResult checked = checkArtTexture(mBytes.data(), mBytes.size(), count);
                        result = checked != ArtTexResult::Ok ? (S32)checked : count < mNeeded ? -2 : 0;
                        mLayers = result == 0 ? count : 0;
                    }
                }
                catch (const std::bad_alloc&)
                {
                    result = -1;    // no memory for the file: no pictures
                }
                if (result != 0)
                {
                    std::vector<U8>().swap(mBytes);
                }
                mResult = result;
                mDone.store(true, std::memory_order_release);
            });
        }
        catch (...)
        {
            mResult = -1;   // no thread: no pictures
            mDone.store(true, std::memory_order_release);
        }
    }

    ArtPicturesReader::~ArtPicturesReader()
    {
        if (mThread.joinable())
        {
            mThread.join();     // a read of about 15 MB at most
        }
    }

    ArtPicturesReader::State ArtPicturesReader::poll()
    {
        if (!mDone.load(std::memory_order_acquire))
        {
            return State::Reading;
        }
        if (mThread.joinable())
        {
            mThread.join();     // it has set mDone: it is leaving
        }
        if (mResult == 0)
        {
            return State::Ready;
        }
        if (!mReported)
        {
            mReported = true;
            // the file's name only (the full path holds the user's folder)
            LL_WARNS("Plus") << "real stars: " << ART_PICTURES_FILE << " not used (" << mResult << ": -1 not read, -2 "
                             << "fewer layers than the " << mNeeded << " needed, else not right); no constellation pictures" << LL_ENDL;
        }
        return State::Failed;
    }

    const Place* places(size_t& count)
    {
        count = PLACE_COUNT;
        return PLACES;
    }

    std::string localTextOf(const std::string& utc)
    {
        double unix_seconds = 0.0;
        if (!timeutil::parseUtc(utc, unix_seconds))
        {
            return std::string();
        }
        return timeutil::formatCivil(localOfUnix(unix_seconds));
    }

    bool utcOfLocalText(const std::string& text, std::string& utc)
    {
        // the full-width digits and marks a Japanese input method types, as the half-width ones
        LLWString wide = utf8str_to_wstring(text);
        for (llwchar& c : wide)
        {
            if (c >= 0xFF01 && c <= 0xFF5E)
            {
                c -= 0xFEE0;    // the full-width forms of ASCII ("０" .. "９", "：", "－", "／")
            }
            else if (c == 0x3000)
            {
                c = ' ';        // the full-width space
            }
        }
        std::string trimmed = wstring_to_utf8str(wide);
        LLStringUtil::trim(trimmed);
        if (trimmed.empty())
        {
            utc.clear();
            return true;
        }
        timeutil::Civil local;
        if (!timeutil::parseCivil(trimmed, local))
        {
            return false;
        }
        utc = timeutil::formatUtc(timeutil::utcOfUnix(unixOfLocal(local)));
        return true;
    }
}
