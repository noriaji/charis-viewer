// Stand-alone test of realskymath.h, realskytime.h, realstarcat.h and realstarart.h (not part of the viewer build):
//   cl /nologo /EHsc /std:c++17 /utf-8 /W4 /WX realsky_test.cpp && realsky_test.exe
// Reads ../../app_settings/plus/starcat-bsc5.bin next to this file (found from __FILE__).
// The REF table is made by tools/selftest/checks/realsky_check.py --emit-cpp (Meeus's one-star
// formulas, written apart from this header); that check also confirms the table is its own.
#include "realskymath.h"
#include "realskyplaces.h"
#include "realskytime.h"
#include "realstarart.h"
#include "realstarcat.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace plus::realstars;
using namespace plus::realstars::math;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)
static bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }
static double angDiff(double a, double b) { return wrapDeg180(a - b); }
static double hms(double h, double m, double s) { return (h + m / 60.0 + s / 3600.0) * 15.0; }
static double dms(double d, double m, double s) { return d + m / 60.0 + s / 3600.0; }

struct RefCase
{
    const char* name;
    double jd, lon, lat;
    double m[9];
};

static const RefCase REF[] = {
    { "tokyo-2026-12-21T14:19Z", 2461396.0965277776, 139.77, 35.68, { -0.995125389553805, 0.098583234009516, 0.002608646701259, -0.055370538681380, -0.580422964842488, 0.812430480305601, 0.081606142609002, 0.808325756026419, 0.583052235766915 } }, // REF
    { "sydney-2026-06-01T12:00Z", 2461193.0000000000, 151.21, -33.87, { 0.654184055194627, -0.756333513384399, -0.001684773196492, -0.419380115521348, -0.364591106240841, 0.831380565057532, -0.629415237052944, -0.543169349081565, -0.555700924584345 } }, // REF
    { "mauna-kea-2027-03-20T08:30Z", 2461484.8541666665, -155.47, 19.82, { -0.510045424765612, -0.860146420287955, 0.001341768043716, 0.294133270575832, -0.172947397596784, 0.939986604587993, -0.808294057763653, 0.479830525634120, 0.341208708642749 } }, // REF
};

static double unixOf(int y, int mo, int d, int h, int mi)
{
    // days from the civil date (Howard Hinnant's days_from_civil)
    y -= mo <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const double days = (double)era * 146097.0 + (double)doe - 719468.0;
    return days * 86400.0 + h * 3600.0 + mi * 60.0;
}

static double jdOf(int y, int mo, int d, int h = 0, int mi = 0)
{
    return julianDayFromUnix(unixOf(y, mo, d, h, mi));
}

static AltAz starAltAz(const CatalogStar& s, double jd, double lon, double lat)
{
    return altAzFromEnu(mul(skyMatrix(jd, lon, lat), Vec3d{ s.dir[0], s.dir[1], s.dir[2] }));
}

static bool orthonormal(const Mat3d& m)
{
    const Mat3d p = mul(m, transpose(m));
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (!near(p.m[i][j], i == j ? 1.0 : 0.0, 1e-12))
                return false;
    return true;
}

static std::string catalogPath()
{
    std::string here = __FILE__;
    const size_t cut = here.find_last_of("/\\");
    here = cut == std::string::npos ? std::string(".") : here.substr(0, cut);
    return here + "/../../app_settings/plus/starcat-bsc5.bin";
}

static const CatalogStar* nearestStar(const Catalog& cat, double raDeg, double decDeg, double& sepDeg)
{
    const Vec3d u = unitFromRaDec(raDeg, decDeg);
    const CatalogStar* best = nullptr;
    double bestDot = -2.0;
    for (const CatalogStar& s : cat.stars)
    {
        const double d = s.dir[0] * u.x + s.dir[1] * u.y + s.dir[2] * u.z;
        if (d > bestDot)
        {
            bestDot = d;
            best = &s;
        }
    }
    sepDeg = std::acos(bestDot > 1.0 ? 1.0 : bestDot) / DEG;
    return best;
}

static void testMath()
{
    // time
    CHECK(near(julianDayFromUnix(0.0), 2440587.5, 1e-12));
    CHECK(near(jdOf(2000, 1, 1, 12, 0), JD_J2000, 1e-9));
    CHECK(near(wrapDeg360(-30.0), 330.0, 1e-12) && near(wrapDeg360(720.0), 0.0, 1e-12));
    CHECK(near(wrapDeg180(190.0), -170.0, 1e-12));
    // Meeus 12.a, 12.b
    CHECK(near(gmstDeg(2446895.5), hms(13, 10, 46.3668), 1e-5));
    CHECK(near(gmstDeg(jdOf(1987, 4, 10, 19, 21)), hms(8, 34, 57.0896), 1e-5));
    CHECK(near(lstDeg(2446895.5, 139.77), wrapDeg360(hms(13, 10, 46.3668) + 139.77), 1e-5));

    // Meeus 13.b: Venus from the US Naval Observatory, H = 64.352133 -> A = 68.0337 (from south), h = 15.1249
    const double latW = dms(38, 55, 17), lonW = -dms(77, 3, 56);
    const double decV = -dms(6, 43, 11.61), raV = hms(23, 9, 16.641);
    AltAz a = altAzFromHourAngle(64.352133, decV, latW);
    CHECK(near(a.altDeg, 15.1249, 2e-4) && near(a.azDeg, 248.0337, 2e-4));
    // the same through the matrix and the time (mean sidereal time; Meeus uses the apparent, 0.004 deg apart)
    const double jdV = jdOf(1987, 4, 10, 19, 21);
    a = altAzFromEnu(mul(horizonMatrix(lstDeg(jdV, lonW), latW), unitFromRaDec(raV, decV)));
    CHECK(near(a.altDeg, 15.1249, 0.01) && near(a.azDeg, 248.0337, 0.01));
    CHECK(near(hourAngleFromEnuDeg(mul(horizonMatrix(lstDeg(jdV, lonW), latW), unitFromRaDec(raV, decV)), latW),
               wrapDeg180(lstDeg(jdV, lonW) - raV), 1e-9));

    // Meeus 21.b: theta Persei, J2000 (with its proper motion applied) to 2028-11-13.19 TD
    double ra = 0.0, dec = 0.0;
    raDecFromVec(mul(precessionMatrix(2462088.69), unitFromRaDec(hms(2, 44, 12.975), dms(49, 13, 39.896))), ra, dec);
    CHECK(near(ra, 41.547214, 1e-5) && near(dec, 49.348483, 1e-5));
    CHECK(orthonormal(precessionMatrix(2462088.69)));
    // 26 years of precession move a star by about 0.36 deg
    raDecFromVec(mul(precessionMatrix(jdOf(2026, 1, 1)), unitFromRaDec(0.0, 0.0)), ra, dec);
    CHECK(ra > 0.3 && ra < 0.4);

    // Meeus 25.a: the Sun on 1992-10-13 0h TD
    sunRaDecDeg(2448908.5, ra, dec);
    CHECK(near(ra, 198.38083, 2e-4) && near(dec, -7.78507, 2e-4));

    // the Sun's ENU direction gives back the sidereal time (stage 2's "follow SL's sun")
    double worst = 0.0;
    for (int i = 0; i < 40; ++i)
    {
        const double jd = jdOf(2026, 6, 21) + i * 37.0 / 1440.0;
        sunRaDecDeg(jd, ra, dec);
        const double lst = lstDeg(jd, 139.77);
        const Vec3d enu = mul(horizonMatrix(lst, 35.68), unitFromRaDec(ra, dec));
        worst = std::fmax(worst, std::fabs(angDiff(hourAngleFromEnuDeg(enu, 35.68) + ra, lst)));
    }
    CHECK(worst < 1e-9);

    // the REF table (Python, one-star formulas) against skyMatrix
    for (const RefCase& r : REF)
    {
        const Mat3d m = skyMatrix(r.jd, r.lon, r.lat);
        CHECK(orthonormal(m));
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                if (!near(m.m[i][j], r.m[3 * i + j], 1e-12))
                {
                    std::printf("FAIL REF %s [%d][%d]: %.15f vs %.15f\n", r.name, i, j, m.m[i][j], r.m[3 * i + j]);
                    ++failures;
                }
    }
}

static void testCatalogParse()
{
    Catalog cat;
    const std::uint8_t junk[50] = { 'C', 'S', 'T', 'X' };
    CHECK(parseCatalog(nullptr, 0, cat) == CatalogResult::TooShort);
    CHECK(parseCatalog(junk, 10, cat) == CatalogResult::TooShort);
    CHECK(parseCatalog(junk, sizeof(junk), cat) == CatalogResult::BadMagic);
    std::uint8_t one[54] = { 'C', 'S', 'T', 'R', 1, 0, 0, 0, 1, 0, 0, 0 };
    // one star at (0, 0, 1), V 1.00, B-V 0.5
    one[44 + 4] = 0xff; one[44 + 5] = 0x7f;
    one[44 + 6] = 100;
    one[44 + 8] = 0xf4; one[44 + 9] = 0x01;
    CHECK(parseCatalog(one, sizeof(one), cat) == CatalogResult::Ok && cat.stars.size() == 1);
    CHECK(cat.stars.size() == 1 && near(cat.stars[0].dir[2], 1.0, 1e-7) && near(cat.stars[0].vmag, 1.0, 1e-6) && near(cat.stars[0].bv, 0.5, 1e-6));
    CHECK(parseCatalog(one, sizeof(one) - 1, cat) == CatalogResult::BadCount && cat.stars.empty());
    one[4] = 2;
    CHECK(parseCatalog(one, sizeof(one), cat) == CatalogResult::BadVersion);
    one[4] = 1;
    one[44 + 4] = 0; one[44 + 5] = 0; // zero vector
    CHECK(parseCatalog(one, sizeof(one), cat) == CatalogResult::BadStar && cat.stars.empty());
    std::uint8_t huge[44] = { 'C', 'S', 'T', 'R', 1, 0, 0, 0, 0xff, 0xff, 0xff, 0xff };
    CHECK(parseCatalog(huge, sizeof(huge), cat) == CatalogResult::BadCount);
}

static void testCatalogSky()
{
    std::ifstream f(catalogPath(), std::ios::binary);
    CHECK(f.good());
    if (!f.good())
        return;
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Catalog cat;
    CHECK(parseCatalog(bytes.data(), bytes.size(), cat) == CatalogResult::Ok);
    CHECK(cat.stars.size() == 9096);
    if (cat.stars.size() != 9096)
        return;

    // the brightest is Sirius
    const CatalogStar* brightest = &cat.stars[0];
    for (const CatalogStar& s : cat.stars)
        if (s.vmag < brightest->vmag)
            brightest = &s;
    double sep = 0.0;
    const CatalogStar* sirius = nearestStar(cat, hms(6, 45, 8.92), -dms(16, 42, 58.0), sep);
    CHECK(brightest == sirius && sep < 0.01 && near(sirius->vmag, -1.46, 0.005));

    const CatalogStar* polaris = nearestStar(cat, hms(2, 31, 49.09), dms(89, 15, 50.8), sep);
    CHECK(sep < 0.01);
    const CatalogStar* acrux = nearestStar(cat, hms(12, 26, 35.90), -dms(63, 5, 56.7), sep);
    CHECK(sep < 0.01);
    const CatalogStar* mintaka = nearestStar(cat, hms(5, 32, 0.40), -dms(0, 17, 56.7), sep);
    CHECK(sep < 0.01);
    const double cas[5][2] = { { hms(0, 9, 10.69), dms(59, 8, 59.2) }, { hms(0, 40, 30.44), dms(56, 32, 14.4) },
                               { hms(0, 56, 42.53), dms(60, 43, 0.3) }, { hms(1, 25, 48.95), dms(60, 14, 7.0) },
                               { hms(1, 54, 23.73), dms(63, 40, 12.4) } };

    const double tokyoLon = 139.77, tokyoLat = 35.68;
    // Polaris's altitude is the latitude (within its 0.63 deg from the pole)
    double worst = 0.0;
    const double lats[] = { -30.0, 0.0, 26.21, 35.68, 43.06, 64.0, 89.0 };
    for (double lat : lats)
        for (int i = 0; i <= 144; ++i)
            worst = std::fmax(worst, std::fabs(starAltAz(*polaris, jdOf(2026, 10, 24) + i / 144.0, tokyoLon, lat).altDeg - lat));
    CHECK(worst < 1.0);

    // Acrux never rises at Tokyo; Cassiopeia never sets at Sapporo
    double hi = -90.0, lo = 90.0;
    for (int i = 0; i <= 288; ++i)
    {
        const double jd = jdOf(2026, 1, 15) + i / 288.0;
        hi = std::fmax(hi, starAltAz(*acrux, jd, tokyoLon, tokyoLat).altDeg);
        for (const auto& c : cas)
            lo = std::fmin(lo, starAltAz(*nearestStar(cat, c[0], c[1], sep), jd, 141.35, 43.06).altDeg);
    }
    CHECK(hi < -8.0);
    CHECK(lo > 5.0);

    // Mintaka rises due east at Tokyo
    double riseAz = -1.0;
    AltAz prev = starAltAz(*mintaka, jdOf(2026, 12, 21), tokyoLon, tokyoLat);
    for (int i = 1; i <= 2880 && riseAz < 0.0; ++i)
    {
        const AltAz cur = starAltAz(*mintaka, jdOf(2026, 12, 21) + i / 2880.0, tokyoLon, tokyoLat);
        if (prev.altDeg < 0.0 && cur.altDeg >= 0.0)
            riseAz = prev.azDeg + (-prev.altDeg / (cur.altDeg - prev.altDeg)) * angDiff(cur.azDeg, prev.azDeg);
        prev = cur;
    }
    CHECK(near(riseAz, 90.0, 1.0));

    // Sirius culminates in the south at 90 - lat + dec (dec of the date)
    AltAz best = { -90.0, 0.0 };
    double bestJd = 0.0;
    for (int i = 0; i <= 5760; ++i)
    {
        const double jd = jdOf(2026, 1, 15) + i / 5760.0;
        const AltAz cur = starAltAz(*sirius, jd, tokyoLon, tokyoLat);
        if (cur.altDeg > best.altDeg)
        {
            best = cur;
            bestJd = jd;
        }
    }
    double raD = 0.0, decD = 0.0;
    raDecFromVec(mul(precessionMatrix(bestJd), Vec3d{ sirius->dir[0], sirius->dir[1], sirius->dir[2] }), raD, decD);
    CHECK(near(best.altDeg, 90.0 - tokyoLat + decD, 0.1) && near(best.azDeg, 180.0, 0.5));

    // B-V: Betelgeuse orange, Rigel blue-white
    const CatalogStar* bet = nearestStar(cat, hms(5, 55, 10.31), dms(7, 24, 25.4), sep);
    const CatalogStar* rig = nearestStar(cat, hms(5, 14, 32.27), -dms(8, 12, 5.9), sep);
    CHECK(bet->bv > 1.6 && rig->bv < 0.0);
}

// Stage 2 (research/177 §2.1 (b)): the sky turned with SL's sun, the colour of B-V.
static void testFollowSun()
{
    // SL's sun anywhere: the Sun of the date, through the matrix, comes out where SL's sun is
    // when SL's sun is on the Sun's own day circle (the hour angle is all the matrix takes from it)
    double worst = 0.0;
    const double lats[] = { -33.87, 0.0, 35.68, 64.0 };
    for (double lat : lats)
        for (int i = 0; i < 48; ++i)
        {
            const double jd = jdOf(2026, 3, 1) + i * 0.37;
            double ra = 0.0, dec = 0.0;
            sunRaDecDeg(jd, ra, dec);
            const double h = -180.0 + i * 7.5;
            const AltAz want = altAzFromHourAngle(h, dec, lat);
            const Vec3d sun = { std::cos(want.altDeg * DEG) * std::sin(want.azDeg * DEG),
                                std::cos(want.altDeg * DEG) * std::cos(want.azDeg * DEG), std::sin(want.altDeg * DEG) };
            const Mat3d m = skyMatrixFollowingSun(jd, sun, lat);
            CHECK(orthonormal(m));
            // the Sun of the date (equator of the date) back to J2000, then the matrix
            const Vec3d j2000 = mul(transpose(precessionMatrix(jd)), unitFromRaDec(ra, dec));
            const Vec3d got = mul(m, j2000);
            // the angle apart as the chord (acos near 1 has too few digits)
            const double dx = got.x - sun.x, dy = got.y - sun.y, dz = got.z - sun.z;
            worst = std::fmax(worst, std::sqrt(dx * dx + dy * dy + dz * dz) / DEG);
        }
    CHECK(worst < 1e-6);
    // SL's sun rising in the east (x) at the equator: the stars of the Sun's right ascension rise there too
    double ra = 0.0, dec = 0.0;
    sunRaDecDeg(jdOf(2026, 3, 20, 12, 0), ra, dec);
    const Mat3d m = skyMatrixFollowingSun(jdOf(2026, 3, 20, 12, 0), Vec3d{ 1.0, 0.0, 0.0 }, 0.0);
    const Vec3d east = mul(m, mul(transpose(precessionMatrix(jdOf(2026, 3, 20, 12, 0))), unitFromRaDec(ra, 0.0)));
    CHECK(near(east.x, 1.0, 1e-6));
    CHECK(near(altitudeDegOfEnu(Vec3d{ 0.0, 1.0, 1.0 }), 45.0, 1e-9));

    // B-V to temperature: the Sun (0.65) about 5800 K, Rigel (-0.03) above 10000 K, Betelgeuse (1.85) about 3300 K
    CHECK(near(temperatureOfBV(0.65), 5800.0, 150.0));
    CHECK(temperatureOfBV(-0.03) > 10000.0);
    CHECK(near(temperatureOfBV(1.85), 3300.0, 250.0));
    CHECK(temperatureOfBV(-5.0) == temperatureOfBV(-0.4) && temperatureOfBV(9.0) == temperatureOfBV(2.0));
}

// PlusSkyStarsTime and the photo tools' box.
static void testTime()
{
    using namespace plus::realstars::timeutil;
    double u = 0.0;
    CHECK(parseUtc("2026-12-14T13:00:00Z", u) && near(u, unixOf(2026, 12, 14, 13, 0), 1e-9));
    CHECK(parseUtc("2026-12-14T13:00Z", u) && near(u, unixOf(2026, 12, 14, 13, 0), 1e-9));
    CHECK(parseUtc("1970-01-01T00:00:00Z", u) && u == 0.0);
    CHECK(!parseUtc("", u) && !parseUtc("2026-12-14", u) && !parseUtc("2026-02-30T00:00Z", u) && !parseUtc("garbage", u));
    CHECK(!parseUtc("1799-12-31T23:59Z", u) && !parseUtc("2201-01-01T00:00Z", u) && !parseUtc("2026-12-14T24:00Z", u));
    Civil c;
    CHECK(parseCivil(" 2026/12/14 22:05 ", c) && c.year == 2026 && c.month == 12 && c.day == 14 && c.hour == 22 && c.minute == 5);
    CHECK(parseCivil("2026-12-14", c) && c.hour == 0 && c.minute == 0);
    CHECK(parseCivil("2024-02-29T23:59:30", c) && c.second == 30);
    CHECK(!parseCivil("2023-02-29 00:00", c) && !parseCivil("2026-12-14 22", c) && !parseCivil("2026-12-1422:00", c));
    CHECK(!parseCivil("2026-13-01 00:00", c) && !parseCivil("2026-12-14 22:60", c) && !parseCivil("2026-12-14 22:00 x", c));
    // round trips, before 1970 too
    const double times[] = { 0.0, -1.0, -86400.0 * 365.25 * 150.0, 1797253200.0, unixOf(2200, 12, 31, 23, 59) };
    for (double t : times)
    {
        double back = 0.0;
        CHECK(parseUtc(formatUtc(utcOfUnix(t)), back) && near(back, std::floor(t), 1e-6));
    }
    CHECK(formatUtc(utcOfUnix(unixOf(2026, 12, 14, 13, 0))) == "2026-12-14T13:00:00Z");
    CHECK(formatCivil(utcOfUnix(unixOf(2026, 1, 2, 3, 4))) == "2026-01-02 03:04");
    CHECK(daysInMonth(2000, 2) == 29 && daysInMonth(1900, 2) == 28 && daysInMonth(2026, 4) == 30);
}

// The constellation pictures' files (research/288): the plates and the pictures' header.
static void putU16(std::uint8_t* p, std::uint16_t v) { p[0] = (std::uint8_t)v; p[1] = (std::uint8_t)(v >> 8); }
static void putU32(std::uint8_t* p, std::uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (std::uint8_t)(v >> (8 * i)); }
static void putF32(std::uint8_t* p, float v) { std::uint32_t b; std::memcpy(&b, &v, 4); putU32(p, b); }

// A file of count plates, each a square on the plane z = 1, layer i.
static std::vector<std::uint8_t> artFile(std::uint32_t count)
{
    std::vector<std::uint8_t> f(ART_HEADER_BYTES + count * ART_PLATE_BYTES, 0);
    std::memcpy(f.data(), "SART", 4);
    putU16(f.data() + 4, ART_VERSION);
    putU32(f.data() + 8, count);
    f[12] = 0xab;   // the source's SHA-256 (its first byte)
    const float corners[4][3] = { { -0.2f, 0.2f, 1.f }, { 0.2f, 0.2f, 1.f }, { 0.2f, -0.2f, 1.f }, { -0.2f, -0.2f, 1.f } };
    for (std::uint32_t i = 0; i < count; ++i)
    {
        std::uint8_t* p = f.data() + ART_HEADER_BYTES + i * ART_PLATE_BYTES;
        putU16(p, (std::uint16_t)i);
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 3; ++k)
                putF32(p + 4 + (c * 3 + k) * 4, corners[c][k]);
    }
    return f;
}

static void testArtParse()
{
    Art art;
    // good: two plates, as written
    const std::vector<std::uint8_t> f = artFile(2);
    CHECK(parseArt(f.data(), f.size(), art) == ArtResult::Ok && art.plates.size() == 2);
    CHECK(art.plates.size() == 2 && art.plates[1].layer == 1 && near(art.plates[0].corner[2][0], 0.2, 1e-7)
          && near(art.plates[0].corner[2][1], -0.2, 1e-7) && near(art.plates[0].corner[3][2], 1.0, 1e-7));
    CHECK(art.sourceSha256[0] == 0xab);
    CHECK(artLayersNeeded(art) == 2);
    // none is fine too (a file without plates)
    const std::vector<std::uint8_t> none = artFile(0);
    CHECK(parseArt(none.data(), none.size(), art) == ArtResult::Ok && art.plates.empty() && artLayersNeeded(art) == 0);
    // short: nothing, the header cut, a plate cut
    CHECK(parseArt(nullptr, 0, art) == ArtResult::TooShort);
    CHECK(parseArt(f.data(), ART_HEADER_BYTES - 1, art) == ArtResult::TooShort && art.plates.empty());
    CHECK(parseArt(f.data(), f.size() - 1, art) == ArtResult::BadCount && art.plates.empty());
    // the magic, the version
    std::vector<std::uint8_t> bad = f;
    bad[3] = 'X';
    CHECK(parseArt(bad.data(), bad.size(), art) == ArtResult::BadMagic);
    bad = f;
    putU16(bad.data() + 4, 2);
    CHECK(parseArt(bad.data(), bad.size(), art) == ArtResult::BadVersion);
    // the count: one more than there are, far too many
    bad = f;
    putU32(bad.data() + 8, 3);
    CHECK(parseArt(bad.data(), bad.size(), art) == ArtResult::BadCount && art.plates.empty());
    bad = f;
    putU32(bad.data() + 8, 0xffffffffu);
    CHECK(parseArt(bad.data(), bad.size(), art) == ArtResult::BadCount);
    // a corner NaN, infinite, too near the middle, too far out; a plate too large or crushed; a layer too high
    std::uint8_t* plate = nullptr;
    bad = f;
    plate = bad.data() + ART_HEADER_BYTES + ART_PLATE_BYTES;
    putF32(plate + 4 + 4, std::nanf(""));
    CHECK(parseArt(bad.data(), bad.size(), art) == ArtResult::BadPlate && art.plates.empty());
    bad = f;
    plate = bad.data() + ART_HEADER_BYTES;
    putF32(plate + 4, HUGE_VALF);
    CHECK(parseArt(bad.data(), bad.size(), art) == ArtResult::BadPlate);
    bad = f;
    plate = bad.data() + ART_HEADER_BYTES;
    putF32(plate + 4, 0.f);
    putF32(plate + 8, 0.f);
    putF32(plate + 12, 0.3f);   // length 0.3: too near the centre
    CHECK(parseArt(bad.data(), bad.size(), art) == ArtResult::BadPlate);
    bad = f;
    plate = bad.data() + ART_HEADER_BYTES;
    putF32(plate + 4, 5.f);     // length about 5.1
    CHECK(parseArt(bad.data(), bad.size(), art) == ArtResult::BadPlate);
    bad = f;
    plate = bad.data() + ART_HEADER_BYTES;
    putF32(plate + 4, -3.5f);
    putF32(plate + 4 + 12, 3.5f);   // lengths about 3.6, but 7 apart
    CHECK(parseArt(bad.data(), bad.size(), art) == ArtResult::BadPlate);
    bad = f;
    plate = bad.data() + ART_HEADER_BYTES;
    for (int c = 1; c < 4; ++c)     // every corner the first: a point
        std::memcpy(plate + 4 + c * 12, plate + 4, 12);
    CHECK(parseArt(bad.data(), bad.size(), art) == ArtResult::BadPlate);
    bad = f;
    putU16(bad.data() + ART_HEADER_BYTES, (std::uint16_t)ART_MAX_LAYERS);
    CHECK(parseArt(bad.data(), bad.size(), art) == ArtResult::BadPlate);

    // the plate's two triangles use its four corners; the corners' places in the picture
    int used[4] = {};
    for (const int c : ART_TRIANGLE_CORNERS)
        ++used[c];
    CHECK(used[0] == 2 && used[1] == 1 && used[2] == 2 && used[3] == 1);
    CHECK(ART_CORNER_UV[0][0] == 0.f && ART_CORNER_UV[0][1] == 0.f && ART_CORNER_UV[1][0] == 1.f && ART_CORNER_UV[1][1] == 0.f);
    CHECK(ART_CORNER_UV[2][0] == 1.f && ART_CORNER_UV[2][1] == 1.f && ART_CORNER_UV[3][0] == 0.f && ART_CORNER_UV[3][1] == 1.f);
}

static void testArtTexture()
{
    // the sizes: 8 mips of 512, BC4 (8 B a block), the last mip one block of 4 x 4 pixels
    CHECK(artTexMipBytes(0) == 512u * 512u / 2u && artTexMipBytes(7) == 8u);
    CHECK(artTexLayerBytes() == 174760u);
    // version 2, a mip's layers together: mip 0 of every layer, then mip 1 of every layer, ...
    static_assert(ART_TEX_VERSION == 2, "the offsets below are version 2's");
    CHECK(artTexMipOffset(3, 0) == ART_TEX_HEADER_BYTES && artTexMipOffset(3, 1) == ART_TEX_HEADER_BYTES + 3u * 131072u);
    CHECK(artTexMipOffset(85, 2) == ART_TEX_HEADER_BYTES + 85u * (131072u + 32768u));
    CHECK(artTexMipOffset(2, 7) + 2u * artTexMipBytes(7) == ART_TEX_HEADER_BYTES + 2u * artTexLayerBytes());

    const std::uint32_t layers_in = 3;
    std::vector<std::uint8_t> f(ART_TEX_HEADER_BYTES + layers_in * artTexLayerBytes(), 0);
    std::memcpy(f.data(), "ATEX", 4);
    putU16(f.data() + 4, ART_TEX_VERSION);
    putU32(f.data() + 8, layers_in);
    putU16(f.data() + 12, (std::uint16_t)ART_TEX_SIZE);
    putU16(f.data() + 14, (std::uint16_t)ART_TEX_MIPS);
    std::uint32_t layers = 99;
    CHECK(checkArtTexture(f.data(), f.size(), layers) == ArtTexResult::Ok && layers == layers_in);
    CHECK(checkArtTexture(nullptr, 0, layers) == ArtTexResult::TooShort && layers == 0);
    CHECK(checkArtTexture(f.data(), 15, layers) == ArtTexResult::TooShort);
    CHECK(checkArtTexture(f.data(), f.size() - 1, layers) == ArtTexResult::BadCount && layers == 0);
    std::vector<std::uint8_t> bad = f;
    bad[0] = 'B';
    CHECK(checkArtTexture(bad.data(), bad.size(), layers) == ArtTexResult::BadMagic);
    bad = f;
    putU16(bad.data() + 4, 1);  // the layer-first file of before: not read (its offsets are not these)
    CHECK(checkArtTexture(bad.data(), bad.size(), layers) == ArtTexResult::BadVersion && layers == 0);
    bad = f;
    putU16(bad.data() + 4, 3);
    CHECK(checkArtTexture(bad.data(), bad.size(), layers) == ArtTexResult::BadVersion);
    // every byte after the header belongs to one mip of one layer: the runs of the mips, one after another
    std::size_t at = ART_TEX_HEADER_BYTES;
    for (std::uint32_t mip = 0; mip < ART_TEX_MIPS; ++mip)
    {
        CHECK(artTexMipOffset(layers_in, mip) == at);
        at += layers_in * artTexMipBytes(mip);
    }
    CHECK(at == f.size());
    bad = f;
    putU16(bad.data() + 12, 256);
    CHECK(checkArtTexture(bad.data(), bad.size(), layers) == ArtTexResult::BadShape);
    bad = f;
    putU16(bad.data() + 14, 10);
    CHECK(checkArtTexture(bad.data(), bad.size(), layers) == ArtTexResult::BadShape);
    bad = f;
    putU32(bad.data() + 8, 0);
    CHECK(checkArtTexture(bad.data(), bad.size(), layers) == ArtTexResult::BadCount);
    bad = f;
    putU32(bad.data() + 8, 4);
    CHECK(checkArtTexture(bad.data(), bad.size(), layers) == ArtTexResult::BadCount);
    bad = f;
    putU32(bad.data() + 8, 0x80000000u);
    CHECK(checkArtTexture(bad.data(), bad.size(), layers) == ArtTexResult::BadCount);
}

// The files star-art-data makes (app_settings/plus/starart.bin, starart-tex.bin), shipped next to
// the catalogue once both branches are in: there, of this reader's version, read as the viewer
// reads them, the plates' layers within the pictures, every plate on the near side of the sky.
static void testArtFiles()
{
    std::string dir = catalogPath();
    dir = dir.substr(0, dir.find_last_of("/\\"));
    std::ifstream plates_file(dir + "/starart.bin", std::ios::binary);
    std::ifstream pictures_file(dir + "/starart-tex.bin", std::ios::binary);
    CHECK(plates_file.good() && pictures_file.good());     // shipped: missing is a fault
    if (!plates_file.good() || !pictures_file.good())
    {
        std::printf("art files: starart.bin or starart-tex.bin missing\n");
        return;
    }
    const std::vector<std::uint8_t> plates_bytes((std::istreambuf_iterator<char>(plates_file)), std::istreambuf_iterator<char>());
    const std::vector<std::uint8_t> pictures_bytes((std::istreambuf_iterator<char>(pictures_file)), std::istreambuf_iterator<char>());
    Art art;
    CHECK(parseArt(plates_bytes.data(), plates_bytes.size(), art) == ArtResult::Ok);
    CHECK(art.plates.size() >= 80 && art.plates.size() <= 88);
    std::uint32_t layers = 0;
    // of this reader's version (an older file would leave the sky without pictures)
    CHECK(checkArtTexture(pictures_bytes.data(), pictures_bytes.size(), layers) == ArtTexResult::Ok);
    CHECK(layers >= artLayersNeeded(art) && artLayersNeeded(art) > 0);
    for (const ArtPlate& plate : art.plates)
    {
        // the plate faces the centre: its corners' sum points the way of each corner
        double mid[3] = {};
        for (const float* c : plate.corner)
            for (int k = 0; k < 3; ++k)
                mid[k] += c[k] / 4.0;
        for (const float* c : plate.corner)
            CHECK(mid[0] * c[0] + mid[1] * c[1] + mid[2] * c[2] > 0.0);
    }
    std::printf("art files: %zu plates, %u layers\n", art.plates.size(), (unsigned)layers);
}

// The constellation lines' file (stage 4).
static void testLinesParse()
{
    Catalog cat;
    cat.stars.resize(3);
    for (int i = 0; i < 32; ++i)
        cat.sourceSha256[i] = (std::uint8_t)(i + 1);
    std::vector<std::uint8_t> file(76 + 2 * 4, 0);
    file[0] = 'C'; file[1] = 'L'; file[2] = 'I'; file[3] = 'N'; file[4] = 1; file[8] = 2;
    for (int i = 0; i < 32; ++i)
        file[44 + i] = (std::uint8_t)(i + 1);
    // 0-1, 1-2
    file[76] = 0; file[78] = 1; file[80] = 1; file[82] = 2;
    Lines lines;
    CHECK(parseLines(file.data(), file.size(), cat, lines) == LinesResult::Ok && lines.ends.size() == 4);
    CHECK(lines.ends.size() == 4 && lines.ends[0] == 0 && lines.ends[1] == 1 && lines.ends[2] == 1 && lines.ends[3] == 2);
    CHECK(parseLines(file.data(), file.size() - 1, cat, lines) == LinesResult::BadCount && lines.ends.empty());
    CHECK(parseLines(file.data(), 10, cat, lines) == LinesResult::TooShort);
    file[82] = 3; // past the catalogue
    CHECK(parseLines(file.data(), file.size(), cat, lines) == LinesResult::BadStar && lines.ends.empty());
    file[82] = 1; // a star to itself
    CHECK(parseLines(file.data(), file.size(), cat, lines) == LinesResult::BadStar);
    file[82] = 2;
    file[44] = 0; // another catalogue's
    CHECK(parseLines(file.data(), file.size(), cat, lines) == LinesResult::OtherCatalogue);
    file[44] = 1;
    file[4] = 2;
    CHECK(parseLines(file.data(), file.size(), cat, lines) == LinesResult::BadVersion);
    file[4] = 1;
    file[3] = 'X';
    CHECK(parseLines(file.data(), file.size(), cat, lines) == LinesResult::BadMagic);
}

// Duplicate segments, either way round, in a made-up list and in the viewer's starcat-lines.bin.
static void testLinesDuplicates()
{
    CHECK(duplicateSegments({ 0, 1, 1, 2, 2, 3 }) == 0);
    CHECK(duplicateSegments({ 0, 1, 1, 2, 0, 1 }) == 1);
    CHECK(duplicateSegments({ 0, 1, 1, 2, 1, 0 }) == 1);    // the other way round
    CHECK(duplicateSegments({ 5, 4, 4, 5, 5, 4 }) == 2);

    std::string path = catalogPath();
    path.replace(path.rfind("starcat-bsc5.bin"), std::string("starcat-bsc5.bin").size(), "starcat-lines.bin");
    std::ifstream f(path, std::ios::binary);
    if (!f.good())
        return; // no lines shipped: the sky without them (realsky_lines_check.py checks that)
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::uint16_t> ends;
    for (std::size_t p = LINES_HEADER_BYTES; p + LINES_SEGMENT_BYTES <= bytes.size(); p += LINES_SEGMENT_BYTES)
    {
        ends.push_back((std::uint16_t)(bytes[p] | (bytes[p + 1] << 8)));
        ends.push_back((std::uint16_t)(bytes[p + 2] | (bytes[p + 3] << 8)));
    }
    CHECK(!ends.empty() && duplicateSegments(ends) == 0);
}

// A line's quad as starsV.glsl spreads it on the screen (its math in 2D, the ends a and b in
// pixels): the distance across that each corner gives the fragment shader must be the corner's
// own distance from the segment, so it is linear over both triangles and 0 all along the middle.
// fromEnd false: the code before 2026-10-25 (the side seen from the vertex's own end), which
// must fail, so the check can tell.
struct QuadVert
{
    double x, y, across;
};

static void lineQuad(double ax, double ay, double bx, double by, double halfWidth, bool fromEnd, QuadVert out[6])
{
    for (int k = 0; k < 6; ++k)
    {
        const LineCorner& c = LINE_CORNERS[k];
        const std::uint8_t alpha = lineCornerAlpha(c);
        // starsV.glsl: the code, the side seen from this end, the end
        const double code = std::floor(alpha / 255.0 * 3.0 + 0.5);
        const double side = std::fmod(code, 2.0) > 0.5 ? 1.0 : -1.0;
        const double fromA = code > 1.5 ? -1.0 : 1.0;
        const double ex = c.atB ? bx : ax, ey = c.atB ? by : ay;
        const double ox = c.atB ? ax : bx, oy = c.atB ? ay : by;
        const double len = std::hypot(ox - ex, oy - ey);
        const double alx = (ox - ex) / len, aly = (oy - ey) / len;
        const double acx = -aly, acy = alx;
        const double back = std::fmin(3.5, 0.3 * len);
        out[k].x = ex + alx * back + acx * side * halfWidth;
        out[k].y = ey + aly * back + acy * side * halfWidth;
        out[k].across = (fromEnd ? side * fromA : side) * halfWidth;
    }
}

// The largest |across| the fragment shader sees on the segment's middle (sampled inside the quad),
// and whether both triangles turn counter-clockwise and each corner's across is its distance.
static double quadMiddle(double ax, double ay, double bx, double by, double halfWidth, bool fromEnd, bool& shapeOk)
{
    QuadVert v[6];
    lineQuad(ax, ay, bx, by, halfWidth, fromEnd, v);
    const double len = std::hypot(bx - ax, by - ay);
    const double ux = (bx - ax) / len, uy = (by - ay) / len;
    shapeOk = true;
    double worst = 0.0;
    for (int t = 0; t < 2; ++t)
    {
        const QuadVert& p0 = v[3 * t];
        const QuadVert& p1 = v[3 * t + 1];
        const QuadVert& p2 = v[3 * t + 2];
        const double area = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
        shapeOk = shapeOk && area > 0.0;
        for (const QuadVert* p : { &p0, &p1, &p2 })
        {
            const double distance = (p->x - ax) * -uy + (p->y - ay) * ux;
            shapeOk = shapeOk && near(distance, p->across, 1e-9 * (1.0 + len));
        }
        // the middle, from end to end of the quad
        for (int s = 0; s <= 100; ++s)
        {
            const double f = 0.05 + 0.9 * s / 100.0;
            const double px = ax + (bx - ax) * f, py = ay + (by - ay) * f;
            const double w0 = ((p1.x - px) * (p2.y - py) - (p2.x - px) * (p1.y - py)) / area;
            const double w1 = ((p2.x - px) * (p0.y - py) - (p0.x - px) * (p2.y - py)) / area;
            const double w2 = 1.0 - w0 - w1;
            if (w0 < -1e-9 || w1 < -1e-9 || w2 < -1e-9)
                continue;   // in the other triangle
            worst = std::fmax(worst, std::fabs(w0 * p0.across + w1 * p1.across + w2 * p2.across));
        }
    }
    return worst;
}

static void testLineQuad()
{
    // the six corners: four places, each end on both sides
    int seen[2][2] = {};
    for (const LineCorner& c : LINE_CORNERS)
        ++seen[c.atB][c.plusSide];
    CHECK(seen[0][0] == 2 && seen[0][1] == 1 && seen[1][0] == 1 && seen[1][1] == 2);
    // the alpha keeps both bits apart
    CHECK(lineCornerAlpha({ false, false }) == 0 && lineCornerAlpha({ false, true }) == 85
          && lineCornerAlpha({ true, true }) == 170 && lineCornerAlpha({ true, false }) == 255);

    // lines on the screen every way round, long and short, at the widths of PlusSkyStarsLineWidth
    const double ends[][4] = {
        { 100, 100, 400, 130 }, { 400, 130, 100, 100 }, { 50, 300, 50, 20 }, { 10, 10, 18, 25 },
        { 500, 200, 320, 380 }, { 0, 0, 3, 0 },
    };
    for (const auto& e : ends)
        for (double width : { 0.5, 1.0, 2.0, 4.0 })
        {
            const double half = 0.5 * width + 1.0;  // starsV.glsl: a pixel more for the soft edge
            bool shapeOk = false;
            CHECK(quadMiddle(e[0], e[1], e[2], e[3], half, true, shapeOk) < 1e-6);
            CHECK(shapeOk);
            // the old code: 0 on two slanted lines instead, far from 0 on the middle
            bool oldShape = true;
            CHECK(quadMiddle(e[0], e[1], e[2], e[3], half, false, oldShape) > 0.5 * half);
            CHECK(!oldShape);
        }
}

// LLSlider::setValue(): the latitude and longitude rows (step 0.01) round what is put in them so.
static float sliderValue(float value, float lo, float hi, float step)
{
    value = value < lo ? lo : (value > hi ? hi : value);
    value -= lo;
    value += step / 2.0001f;
    value -= std::fmod(value, step);
    return value + lo;
}

// The "Seen From" list (realskyplaces.h): ranges, names, groups, and that each place's values,
// as the setting and as the rows show and take them, are found as that place again.
static void testPlaces()
{
    static_assert(PLACE_COUNT >= 40 && PLACE_COUNT <= 60, "the list is 40 to 60 places");

    // the eight of the first list keep their names and values (saved settings still show them)
    struct Old { const char* en; const char* ja; float lat, lon; };
    const Old OLD[] = {
        { "Tokyo", "東京", 35.68f, 139.77f }, { "Sapporo", "札幌", 43.06f, 141.35f },
        { "Naha", "那覇", 26.21f, 127.68f }, { "Mauna Kea", "マウナケア", 19.82f, -155.47f },
        { "Sydney", "シドニー", -33.87f, 151.21f }, { "London", "ロンドン", 51.51f, -0.13f },
        { "New York", "ニューヨーク", 40.71f, -74.01f }, { "North Pole", "北極", 90.f, 0.f },
    };
    for (const Old& old : OLD)
    {
        const size_t i = placeIndexOf(old.lat, old.lon);
        CHECK(i < PLACE_COUNT);
        if (i < PLACE_COUNT)
        {
            CHECK(std::strcmp(PLACES[i].en, old.en) == 0);
            CHECK(std::strcmp(PLACES[i].ja, old.ja) == 0);
        }
    }

    size_t named_en = 0, named_ja = 0;
    bool region_seen[REGION_COUNT] = {};
    int last_region = -1;
    for (size_t i = 0; i < PLACE_COUNT; ++i)
    {
        const Place& p = PLACES[i];
        CHECK(std::isfinite(p.lat) && p.lat >= -90.f && p.lat <= 90.f);
        CHECK(std::isfinite(p.lon) && p.lon >= -180.f && p.lon <= 180.f);
        named_en += p.en && p.en[0] ? 1 : 0;
        named_ja += p.ja && p.ja[0] ? 1 : 0;
        // grouped: a region's places together, in the order of REGIONS
        CHECK((int)p.region >= 0 && p.region < REGION_COUNT);
        CHECK((int)p.region >= last_region);
        last_region = (int)p.region;
        region_seen[p.region] = true;
        for (size_t j = 0; j < i; ++j)
        {
            CHECK(std::strcmp(PLACES[j].en, p.en) != 0);
            CHECK(std::strcmp(PLACES[j].ja, p.ja) != 0);
        }
        // the setting's value (F32), the rows' rounding and a value typed as the row shows it
        // ("%.2f") are all this place, not another one nor "Custom"
        CHECK(placeIndexOf(p.lat, p.lon) == i);
        const float shown_lat = sliderValue(p.lat, -90.f, 90.f, 0.01f);
        const float shown_lon = sliderValue(p.lon, -180.f, 180.f, 0.01f);
        CHECK(placeIndexOf(shown_lat, shown_lon) == i);
        char text[32];
        std::snprintf(text, sizeof(text), "%.2f", shown_lat);
        const double typed_lat = std::atof(text);
        std::snprintf(text, sizeof(text), "%.2f", shown_lon);
        const double typed_lon = std::atof(text);
        CHECK(placeIndexOf(sliderValue((float)typed_lat, -90.f, 90.f, 0.01f), sliderValue((float)typed_lon, -180.f, 180.f, 0.01f)) == i);
        // a value moved by a step is "Custom" (or, by chance, another place: never this one)
        CHECK(placeIndexOf(p.lat + 0.01, p.lon) != i || p.lat >= 90.f);
    }
    CHECK(named_en == PLACE_COUNT && named_ja == PLACE_COUNT);  // en and ja for every place
    for (int r = 0; r < REGION_COUNT; ++r)
    {
        CHECK(region_seen[r]);
        CHECK(REGIONS[r].en && REGIONS[r].en[0] && REGIONS[r].ja && REGIONS[r].ja[0]);
        for (int s = 0; s < r; ++s)
        {
            CHECK(std::strcmp(REGIONS[s].en, REGIONS[r].en) != 0);
            CHECK(std::strcmp(REGIONS[s].ja, REGIONS[r].ja) != 0);
        }
    }
    CHECK(placeIndexOf(12.34, 56.78) == PLACE_COUNT);  // no place: "Custom"
    std::printf("places: %zu in %d regions\n", PLACE_COUNT, (int)REGION_COUNT);
}

int main()
{
    testPlaces();
    testMath();
    testFollowSun();
    testTime();
    testLinesParse();
    testLinesDuplicates();
    testLineQuad();
    testArtParse();
    testArtTexture();
    testArtFiles();
    testCatalogParse();
    testCatalogSky();
    if (failures)
    {
        std::printf("realsky_test: %d FAILED\n", failures);
        return 1;
    }
    std::printf("realsky_test: all passed\n");
    return 0;
}
