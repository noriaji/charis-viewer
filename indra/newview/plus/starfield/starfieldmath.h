/**
 * @file starfieldmath.h
 * @brief The catalogue of the night sky's stars and of the Milky Way, in plain C++ (research/176-star-field.md).
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

#ifndef PLUS_STARFIELDMATH_H
#define PLUS_STARFIELDMATH_H

// Standard C++ only, so that starfield_test.cpp can check it without the viewer.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace plus::starfield::math
{
    // The brightness is kept in a colour's alpha, on a log scale:
    // light = exp2((alpha - 1) * LEVEL_STOPS), 1 for the brightest.
    constexpr float LEVEL_STOPS = 12.f;

    constexpr int MODE_OFFICIAL = 0;
    constexpr int MODE_FIELD = 1;
    constexpr int MODE_MILKY_WAY = 2;

    // The stars. A real dark sky shows about 2500 to the naked eye (magnitude 6.5)
    // over the whole sphere; the fainter ones are what a camera records and make the
    // sky look deep. They cost one quad each.
    constexpr unsigned STAR_COUNT = 9000;
    constexpr unsigned BAND_STAR_COUNT = 5000;      // more, along the Milky Way (that mode)
    constexpr float BAND_MAG_BRIGHTEST = 5.f;       // they are all faint
    // The brightest and the faintest apparent magnitude. The number of stars grows
    // by 10^STAR_SLOPE per magnitude (the real sky: about 0.5).
    constexpr float MAG_BRIGHTEST = -1.f;
    constexpr float MAG_FAINTEST = 7.f;
    constexpr float STAR_SLOPE = 0.5f;
    // The real flux spans 100 000 times between the ends: the light shown follows a
    // magnitude as this share of a real one.
    constexpr float MAG_COMPRESSION = 0.62f;
    // How far a star's tint goes towards white (the eye sees little colour).
    constexpr float TINT_TO_WHITE = 0.42f;

    // The Milky Way: the plane's normal (z up) and the centre's longitude.
    constexpr float PLANE_X = 0.62f;
    constexpr float PLANE_Y = 0.38f;
    constexpr float PLANE_Z = 0.68f;
    constexpr float CENTRE_LONGITUDE = 1.57079633f;     // along the plane's highest point
    constexpr unsigned GLOW_LARGE = 1500;       // wide soft clouds: the band itself
    constexpr unsigned GLOW_MEDIUM = 3800;      // the patches
    constexpr unsigned GLOW_SMALL = 7000;       // the grain
    constexpr unsigned GLOW_TRIES = 40;         // tries of the rejection per patch

    constexpr float PI_F = 3.14159265f;
    constexpr float TWO_PI_F = 6.28318531f;

    struct Vec3
    {
        float x = 0.f, y = 0.f, z = 0.f;
    };

    struct Color
    {
        float r = 0.f, g = 0.f, b = 0.f, a = 0.f;
    };

    inline Vec3 operator+(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    inline Vec3 operator*(const Vec3& a, float k) { return { a.x * k, a.y * k, a.z * k }; }
    inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline Vec3 cross(const Vec3& a, const Vec3& b)
    {
        return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }
    inline Vec3 normalized(const Vec3& a)
    {
        const float l = std::sqrt(dot(a, a));
        return l > 0.f ? a * (1.f / l) : a;
    }

    // The catalogue: stars (unit directions above the horizon; colour rgb the tint, its
    // brightest channel 1, alpha the level) and the Milky Way's patches (unit directions;
    // colour alpha the level of its light; angular radius in radians).
    struct Catalogue
    {
        std::vector<Vec3> star_dir;
        std::vector<Color> star_color;
        std::vector<Vec3> glow_dir;
        std::vector<Color> glow_color;
        std::vector<float> glow_radius;

        void clear()
        {
            star_dir.clear();
            star_color.clear();
            glow_dir.clear();
            glow_color.clear();
            glow_radius.clear();
        }
    };

    inline float clampf(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }

    // The level (alpha, 0 .. 1) for a light (0 < light <= 1) and back.
    inline float levelOfLight(float light)
    {
        return clampf(1.f + std::log2(std::max(light, 1e-6f)) / LEVEL_STOPS, 0.f, 1.f);
    }

    inline float lightOfLevel(float level)
    {
        return std::exp2((clampf(level, 0.f, 1.f) - 1.f) * LEVEL_STOPS);
    }

    // The tint of a star of the surface temperature kelvin (1000 .. 40000): Tanner Helland's
    // fit of a black body's colour (sRGB), to linear light, a share of the way to white, the
    // brightest channel 1.
    inline Color tintOfTemperature(float kelvin)
    {
        const float t = clampf(kelvin, 1000.f, 40000.f) / 100.f;
        float r = 255.f;
        float g;
        float b = 255.f;
        if (t <= 66.f)
        {
            g = 99.4708025861f * std::log(t) - 161.1195681661f;
            b = (t <= 19.f) ? 0.f : 138.5177312231f * std::log(t - 10.f) - 305.0447927307f;
        }
        else
        {
            r = 329.698727446f * std::pow(t - 60.f, -0.1332047592f);
            g = 288.1221695283f * std::pow(t - 60.f, -0.0755148492f);
        }
        float rgb[3] = { r, g, b };
        float peak = 0.f;
        for (float& c : rgb)
        {
            c = std::pow(clampf(c, 0.f, 255.f) / 255.f, 2.2f);
            c = c + (1.f - c) * TINT_TO_WHITE;
            peak = std::max(peak, c);
        }
        return { rgb[0] / peak, rgb[1] / peak, rgb[2] / peak, 1.f };
    }

    namespace detail
    {
        // xorshift32: the same numbers on every machine and every run.
        struct Rng
        {
            uint32_t s;
            explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
            uint32_t next()
            {
                s ^= s << 13;
                s ^= s >> 17;
                s ^= s << 5;
                return s;
            }
            float uniform() { return (next() >> 8) * (1.f / 16777216.f); }     // [0, 1)
            float range(float a, float b) { return a + (b - a) * uniform(); }
            // Normal distribution with sigma 1 (Box-Muller).
            float normal()
            {
                const float u = std::max(uniform(), 1e-7f);
                return std::sqrt(-2.f * std::log(u)) * std::cos(TWO_PI_F * uniform());
            }
        };

        inline float smooth(float t) { return t * t * (3.f - 2.f * t); }

        // Hash of a lattice point to [0, 1).
        inline float lattice(int32_t x, int32_t y, uint32_t seed)
        {
            uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
            h = (h ^ (h >> 13)) * 1274126177u;
            h ^= h >> 16;
            return (h >> 8) * (1.f / 16777216.f);
        }

        // Fractal value noise in [0, 1], four octaves; x is the longitude and wraps with
        // the period (a whole number of lattice cells).
        inline float fbm(float x, float y, int32_t period, uint32_t seed)
        {
            float sum = 0.f;
            float amp = 0.5f;
            float norm = 0.f;
            int32_t per = std::max(period, 1);
            for (uint32_t o = 0; o < 4; ++o)
            {
                const float fx = std::floor(x);
                const float fy = std::floor(y);
                const float tx = smooth(x - fx);
                const float ty = smooth(y - fy);
                const int32_t ix = (int32_t)fx;
                const int32_t iy = (int32_t)fy;
                auto wrap = [per](int32_t v) { return ((v % per) + per) % per; };
                const float a = lattice(wrap(ix), iy, seed + o);
                const float b = lattice(wrap(ix + 1), iy, seed + o);
                const float c = lattice(wrap(ix), iy + 1, seed + o);
                const float d = lattice(wrap(ix + 1), iy + 1, seed + o);
                const float top = a + (b - a) * tx;
                const float bottom = c + (d - c) * tx;
                sum += amp * (top + (bottom - top) * ty);
                norm += amp;
                amp *= 0.5f;
                x *= 2.f;
                y *= 2.f;
                per *= 2;
            }
            return sum / norm;
        }

        // The plane of the Milky Way: e1, e2 in the plane, n its normal; longitude l,
        // latitude b -> direction.
        struct Plane
        {
            Vec3 e1, e2, n;
            bool whole_sky = false;     // the real sky (plus/realstars): no patch is left out below z 0

            Plane()
            {
                n = normalized({ PLANE_X, PLANE_Y, PLANE_Z });
                e1 = normalized(cross(n, { 0.f, 0.f, 1.f }));
                e2 = cross(e1, n);      // its z is positive: longitude 90 degrees is the highest point
            }

            // The plane with the normal pole and the centre (at longitude CENTRE_LONGITUDE) in a
            // frame of the whole sky (the real sky: the galactic pole and centre in J2000). The
            // longitude here runs against the galactic one, so the dark rift (riftDarkness: the
            // near side of the centre) falls towards Aquila and Cygnus as in the real sky.
            Plane(const Vec3& pole, const Vec3& centre)
            {
                n = normalized(pole);
                e2 = normalized(centre + n * -dot(centre, n));
                e1 = cross(n, e2);
                whole_sky = true;
            }

            Vec3 direction(float l, float b) const
            {
                const float cb = std::cos(b);
                return (e1 * std::cos(l) + e2 * std::sin(l)) * cb + n * std::sin(b);
            }
        };

        // The angle a, brought into -pi .. pi.
        inline float wrapAngle(float a)
        {
            while (a > PI_F)
            {
                a -= TWO_PI_F;
            }
            while (a < -PI_F)
            {
                a += TWO_PI_F;
            }
            return a;
        }

        // How much of the Milky Way's light is at longitude l: 1 at the centre, a little
        // at the far side.
        inline float centreWeight(float l)
        {
            const float d = wrapAngle(l - CENTRE_LONGITUDE);
            return std::exp(-d * d / (2.f * 0.85f * 0.85f));
        }

        // The dark rift: dust lying along the plane on the near side of the centre.
        inline float riftDarkness(float l, float b)
        {
            const float d = wrapAngle(l - CENTRE_LONGITUDE + 0.35f);
            const float along = std::exp(-d * d / (2.f * 0.55f * 0.55f));
            const float bb = b - 0.012f * std::sin(l * 3.f);
            const float across = std::exp(-bb * bb / (2.f * 0.022f * 0.022f));
            return along * across;
        }

        // The temperature of a star: the shares of the types O..M by number (the cool
        // dwarfs are the most, the hot blue ones the rarest), bright stars leaning to
        // the hot ones. brightness: 0 (faint) .. 1 (brightest).
        inline float randomTemperature(Rng& rng, float brightness)
        {
            const float u = rng.uniform();
            const float hot = 0.12f + 0.25f * brightness;
            if (u < hot * 0.5f)
            {
                return rng.range(11000.f, 28000.f);     // O, B: blue-white
            }
            if (u < hot)
            {
                return rng.range(7600.f, 10500.f);      // A: white
            }
            const float v = (u - hot) / (1.f - hot);
            if (v < 0.22f)
            {
                return rng.range(6000.f, 7500.f);       // F: yellow-white
            }
            if (v < 0.45f)
            {
                return rng.range(5200.f, 6000.f);       // G: yellow
            }
            if (v < 0.80f)
            {
                return rng.range(3900.f, 5200.f);       // K: orange
            }
            return rng.range(2800.f, 3900.f);           // M: red-orange
        }

        inline void addStar(Catalogue& field, Rng& rng, const Vec3& dir, float magnitude)
        {
            const float brightness = clampf((MAG_FAINTEST - magnitude) / (MAG_FAINTEST - MAG_BRIGHTEST), 0.f, 1.f);
            const float light = std::pow(10.f, -0.4f * MAG_COMPRESSION * (magnitude - MAG_BRIGHTEST));
            Color color = tintOfTemperature(randomTemperature(rng, brightness));
            color.a = levelOfLight(light);
            field.star_dir.push_back(dir);
            field.star_color.push_back(color);
        }

        // A magnitude with the number of stars growing by 10^slope per magnitude between
        // the brightest and the faintest (inverse of the cumulative count).
        inline float randomMagnitude(Rng& rng)
        {
            const float span = MAG_FAINTEST - MAG_BRIGHTEST;
            const float total = std::pow(10.f, STAR_SLOPE * span) - 1.f;
            return MAG_BRIGHTEST + std::log10(1.f + rng.uniform() * total) / STAR_SLOPE;
        }

        inline void generateStars(Catalogue& field, Rng& rng)
        {
            for (unsigned i = 0; i < STAR_COUNT; ++i)
            {
                // Uniform over the upper hemisphere: z is uniform.
                const float z = rng.uniform();
                const float r = std::sqrt(std::max(1.f - z * z, 0.f));
                const float phi = rng.range(0.f, TWO_PI_F);
                addStar(field, rng, { r * std::cos(phi), r * std::sin(phi), z }, randomMagnitude(rng));
            }
        }

        // More faint stars in the Milky Way's band, thickest to the centre and thinned by
        // the dust.
        inline void generateBandStars(Catalogue& field, Rng& rng, const Plane& plane)
        {
            unsigned made = 0;
            for (unsigned tries = 0; made < BAND_STAR_COUNT && tries < BAND_STAR_COUNT * GLOW_TRIES; ++tries)
            {
                const float l = rng.range(0.f, TWO_PI_F);
                const float b = rng.normal() * (0.10f + 0.08f * centreWeight(l));
                const float keep = (0.2f + 0.8f * centreWeight(l)) * (1.f - 0.85f * riftDarkness(l, b));
                if (rng.uniform() > keep)
                {
                    continue;
                }
                const Vec3 dir = plane.direction(l, b);
                if (!plane.whole_sky && dir.z < 0.f)
                {
                    continue;
                }
                addStar(field, rng, dir, rng.range(BAND_MAG_BRIGHTEST, MAG_FAINTEST));
                ++made;
            }
        }

        // One layer of patches: the count, the angular radius range, the share of the light
        // each gives, how tightly it follows the plane (away from and at the centre) and the
        // clumps' scale.
        inline void generateGlowLayer(Catalogue& field, Rng& rng, const Plane& plane, unsigned count,
                                      float radius_min, float radius_max, float light,
                                      float width_base, float width_centre, float noise_scale, uint32_t seed)
        {
            constexpr int32_t PERIOD = 8;   // whole noise cells around the circle at noise_scale 1
            const int32_t period = (int32_t)(PERIOD * noise_scale);
            unsigned made = 0;
            for (unsigned tries = 0; made < count && tries < count * GLOW_TRIES; ++tries)
            {
                const float l = rng.range(0.f, TWO_PI_F);
                const float centre = centreWeight(l);
                const float b = rng.normal() * (width_base + width_centre * centre);
                // Clumps: the light is patchy, with the dust cutting into it.
                const float u = l / TWO_PI_F * (float)period;
                const float clump = 0.25f + 0.75f * fbm(u, b * 14.f * noise_scale + 3.7f, period, seed);
                const float keep = (0.18f + 0.82f * centre) * clump * (1.f - 0.9f * riftDarkness(l, b));
                if (rng.uniform() > keep)
                {
                    continue;
                }
                const Vec3 dir = plane.direction(l, b);
                if (!plane.whole_sky && dir.z < -0.04f)
                {
                    continue;
                }
                // Warm yellow-white towards the centre, cooler blue-white away from it.
                const float warm = clampf(0.15f + 0.85f * centre + (clump - 0.5f) * 0.3f, 0.f, 1.f);
                const float amount = light * (0.35f + 0.65f * centre) * (0.55f + 0.45f * clump) * rng.range(0.6f, 1.f);
                Color color;
                color.r = 0.72f + (1.f - 0.72f) * warm;
                color.g = 0.84f + (0.86f - 0.84f) * warm;
                color.b = 1.f + (0.66f - 1.f) * warm;
                color.a = levelOfLight(clampf(amount, 1e-4f, 1.f));
                field.glow_dir.push_back(dir);
                field.glow_color.push_back(color);
                field.glow_radius.push_back(rng.range(radius_min, radius_max));
                ++made;
            }
        }

        inline void generateMilkyWay(Catalogue& field, Rng& rng, const Plane& plane = Plane())
        {
            generateBandStars(field, rng, plane);
            //                                    count         radius (rad)       light  width   +centre noise seed
            generateGlowLayer(field, rng, plane, GLOW_LARGE,  0.075f, 0.120f, 0.50f, 0.115f, 0.060f, 0.5f, 11);
            generateGlowLayer(field, rng, plane, GLOW_MEDIUM, 0.032f, 0.058f, 0.75f, 0.090f, 0.050f, 1.0f, 23);
            generateGlowLayer(field, rng, plane, GLOW_SMALL,  0.010f, 0.022f, 1.00f, 0.075f, 0.040f, 2.0f, 37);
        }
    }

    // The Milky Way alone (its band's faint stars and its patches), on the plane of the pole
    // and the centre given in a frame of the whole sky (plus/realstars: J2000, the real galactic
    // pole and centre). The same every time: a fixed seed.
    inline void generateMilkyWayOn(const Vec3& pole, const Vec3& centre, Catalogue& field)
    {
        detail::Rng rng(0x6a1ac71cu);
        field.glow_dir.reserve(field.glow_dir.size() + GLOW_LARGE + GLOW_MEDIUM + GLOW_SMALL);
        field.glow_color.reserve(field.glow_color.size() + GLOW_LARGE + GLOW_MEDIUM + GLOW_SMALL);
        field.glow_radius.reserve(field.glow_radius.size() + GLOW_LARGE + GLOW_MEDIUM + GLOW_SMALL);
        detail::generateMilkyWay(field, rng, detail::Plane(pole, centre));
    }

    // Fills the catalogue for the mode (MODE_FIELD or MODE_MILKY_WAY; the official mode has
    // nothing to fill). The same sky every time: a fixed seed.
    inline void generate(int mode, Catalogue& field)
    {
        field.clear();
        if (mode <= MODE_OFFICIAL)
        {
            return;
        }
        detail::Rng rng(0x5eed57a7u);
        field.star_dir.reserve(STAR_COUNT + BAND_STAR_COUNT);
        field.star_color.reserve(STAR_COUNT + BAND_STAR_COUNT);
        detail::generateStars(field, rng);
        if (mode >= MODE_MILKY_WAY)
        {
            field.glow_dir.reserve(GLOW_LARGE + GLOW_MEDIUM + GLOW_SMALL);
            field.glow_color.reserve(GLOW_LARGE + GLOW_MEDIUM + GLOW_SMALL);
            field.glow_radius.reserve(GLOW_LARGE + GLOW_MEDIUM + GLOW_SMALL);
            detail::generateMilkyWay(field, rng);
        }
    }

    // ---- research/224: the denser sky and the baked Milky Way (both options) -------------------

    // The dense sky (PlusSkyStarsDense): more faint stars, as fine grain between the bright ones.
    // They are made-up whatever the sky (the real catalogue ends near magnitude 6.5), at the
    // magnitudes 7 .. 9, shown as the faintest the star curve still draws (the made-up magnitudes
    // are compressed to DENSE_COMPRESSION, so 9 is not 100 times fainter than 7 on the screen),
    // some of them on the Milky Way's plane.
    constexpr unsigned DENSE_COUNT = 40000;
    constexpr float DENSE_MAG_BRIGHTEST = 7.f;
    constexpr float DENSE_MAG_FAINTEST = 9.f;
    constexpr float DENSE_LIGHT_BRIGHTEST = 0.0155f;    // the light of magnitude 7 here (0.0104 in the star field: this is seen)
    constexpr float DENSE_COMPRESSION = 0.21f;
    constexpr float DENSE_BAND_SHARE = 0.45f;           // of them on the band

    // The made-up sky's Milky Way plane (v1's Plane): the pole and the centre's direction, in the
    // frame of the star field (z up). For the bake and generateDenseOn of the made-up sky.
    inline void madeUpPlane(Vec3& pole, Vec3& centre)
    {
        const detail::Plane plane;
        pole = plane.n;
        centre = plane.e2;
    }

    // Appends DENSE_COUNT faint stars to field.star_dir / star_color (the glow_* are not touched),
    // on the sky of the pole and the centre given (the galactic ones in J2000 for the real sky,
    // madeUpPlane() for the made-up one). whole_sky: all over the sphere (the real sky turns);
    // false: above the horizon only (z up), as the made-up sky's stars are. The same every time.
    inline void generateDenseOn(const Vec3& pole, const Vec3& centre, Catalogue& field, bool whole_sky = true)
    {
        detail::Rng rng(0x0de45e57u);
        const detail::Plane plane(pole, centre);
        field.star_dir.reserve(field.star_dir.size() + DENSE_COUNT);
        field.star_color.reserve(field.star_color.size() + DENSE_COUNT);
        const float total = std::pow(10.f, STAR_SLOPE * (DENSE_MAG_FAINTEST - DENSE_MAG_BRIGHTEST)) - 1.f;
        unsigned made = 0;
        for (unsigned tries = 0; made < DENSE_COUNT && tries < DENSE_COUNT * GLOW_TRIES; ++tries)
        {
            // the band's share is decided first, then its star is thrown until the dust and the
            // centre's weight let it stay (else the share of the band would shrink with them)
            Vec3 dir;
            if (rng.uniform() < DENSE_BAND_SHARE)
            {
                bool kept = false;
                for (unsigned attempt = 0; attempt < GLOW_TRIES && !kept; ++attempt)
                {
                    const float l = rng.range(0.f, TWO_PI_F);
                    const float b = rng.normal() * (0.12f + 0.10f * detail::centreWeight(l));
                    const float keep = (0.25f + 0.75f * detail::centreWeight(l)) * (1.f - 0.85f * detail::riftDarkness(l, b));
                    if (rng.uniform() <= keep)
                    {
                        dir = plane.direction(l, b);
                        kept = true;
                    }
                }
                if (!kept)
                {
                    continue;
                }
            }
            else
            {
                const float z = whole_sky ? rng.range(-1.f, 1.f) : rng.uniform();
                const float r = std::sqrt(std::max(1.f - z * z, 0.f));
                const float phi = rng.range(0.f, TWO_PI_F);
                dir = { r * std::cos(phi), r * std::sin(phi), z };
            }
            if (!whole_sky && dir.z < 0.f)
            {
                continue;
            }
            const float magnitude = DENSE_MAG_BRIGHTEST + std::log10(1.f + rng.uniform() * total) / STAR_SLOPE;
            const float light = DENSE_LIGHT_BRIGHTEST * std::pow(10.f, -0.4f * DENSE_COMPRESSION * (magnitude - DENSE_MAG_BRIGHTEST));
            Color color = tintOfTemperature(detail::randomTemperature(rng, 0.f));
            color.a = levelOfLight(light);
            field.star_dir.push_back(dir);
            field.star_color.push_back(color);
            ++made;
        }
    }

    // The baked Milky Way (PlusSkyStarsMilkyWayHQ): the sky's light in an equirectangular picture,
    // in the frame of the pole and the centre given (the star field's: J2000 for the real sky).
    //   u = 0.5 + atan2(d.y, d.x) / (2 pi),  v = 0.5 + asin(d.z) / pi,  row 0 is d.z = -1,
    // linear light, 4 floats a texel (r, g, b, 1); the brightest of the band is about 1.
    // The diffuse light of the disc and the bulge, broken up by clouds and cut by the dust, and
    // the faint stars that are not told apart (their grain). One bake is about 2 million texels
    // of noise: a second at most on one thread; threads > 1 splits the rows (the same picture).
    struct BakeParams
    {
        Vec3 pole = { 0.f, 0.f, 1.f };
        Vec3 centre = { 1.f, 0.f, 0.f };
        int width = 2048;
        int height = 1024;
        int threads = 1;
        unsigned faint_stars = 400000;
    };

    struct BakeResult
    {
        int width = 0;
        int height = 0;
        std::vector<float> rgba;
    };

    // The share of the light in the faint stars' grain (the rest is the diffuse light).
    constexpr float BAKE_STAR_SHARE = 0.25f;
    constexpr float BAKE_STAR_SIGMA = 0.8f;     // texels

    namespace detail
    {
        // The direction of the texel position (u, v) in 0 .. 1.
        inline Vec3 equirectDirection(float u, float v)
        {
            const float lon = (u - 0.5f) * TWO_PI_F;
            const float lat = (v - 0.5f) * PI_F;
            const float c = std::cos(lat);
            return { c * std::cos(lon), c * std::sin(lon), std::sin(lat) };
        }

        // The longitude l (0 .. 2 pi) and latitude b of d on the plane (the inverse of Plane::direction).
        inline void galacticOf(const Plane& plane, const Vec3& d, float& l, float& b)
        {
            b = std::asin(clampf(dot(d, plane.n), -1.f, 1.f));
            l = std::atan2(dot(d, plane.e2), dot(d, plane.e1));
            if (l < 0.f)
            {
                l += TWO_PI_F;
            }
        }

        // The sky's diffuse light at the galactic (l, b), linear rgb, before it is normalised.
        inline void diffuseLight(float l, float b, float rgb[3])
        {
            const float lc = wrapAngle(l - CENTRE_LONGITUDE);   // from the centre; negative: the near side, with the rift
            const float cw = centreWeight(l);

            // The disc: a cusp on the plane, thicker towards the centre, brighter there.
            const float h = 0.095f + 0.055f * cw;
            const float disc = (0.28f + 0.72f * cw) * std::exp(-std::sqrt(b * b + (0.15f * h) * (0.15f * h)) / h);
            // The bulge: an ellipse round the centre.
            const float bulge = 1.1f * std::exp(-((lc / 0.32f) * (lc / 0.32f) + (b / 0.20f) * (b / 0.20f)));

            // The clouds: noise stretched along the plane (about 20 degrees by 6), warped by itself.
            constexpr int32_t CELLS = 18;
            const float u = l / TWO_PI_F * (float)CELLS;
            const float v = b * 10.f;
            const float w1 = fbm(u + 3.1f, v + 1.7f, CELLS, 101);
            const float w2 = fbm(u + 8.3f, v + 5.2f, CELLS, 103);
            const float cloud = fbm(u + 2.f * (w1 - 0.5f), v + 2.f * (w2 - 0.5f), CELLS, 107);
            const float grain = fbm(u * 4.f, v * 4.f, CELLS * 4, 109);
            float structure = 0.35f + 1.30f * (cloud - 0.2f);
            structure = clampf(structure, 0.15f, 1.6f) * (1.f + 0.4f * (grain - 0.5f));

            // The dust: thin dark lanes (a ridge of noise) along the plane, the most on the near
            // side of the centre (the Great Rift), a thin veil all along it.
            const float ridge_noise = fbm(u * 1.5f + 5.5f, v * 1.6f + 9.1f, CELLS * 3 / 2, 113);
            const float ridge = std::pow(1.f - std::fabs(2.f * ridge_noise - 1.f), 2.5f);
            const float width = 0.05f + 0.03f * cw;
            const float bd = b - 0.012f * std::sin(l * 3.f);
            const float profile = std::exp(-bd * bd / (2.f * width * width));
            const float strength = 0.18f + 1.1f * std::exp(-(lc + 0.35f) * (lc + 0.35f) / (2.f * 0.6f * 0.6f));
            const float veil = 0.3f * cw * std::exp(-b * b / (2.f * 0.15f * 0.15f));
            const float tau = 3.0f * strength * ridge * profile + veil;

            // The disc is white-blue, the bulge warm; the dust lets the red through the most.
            const float light_disc = disc * structure;
            const float total = light_disc + bulge;
            const float warm = total > 0.f ? bulge / total : 0.f;
            const float disc_rgb[3] = { 0.85f, 0.90f, 1.00f };
            const float bulge_rgb[3] = { 1.00f, 0.82f, 0.58f };
            const float absorb[3] = { 0.8f, 1.0f, 1.3f };
            for (int c = 0; c < 3; ++c)
            {
                const float tint = disc_rgb[c] + (bulge_rgb[c] - disc_rgb[c]) * warm;
                rgb[c] = total * tint * std::exp(-tau * absorb[c]);
            }
        }

        template <class Fn>
        inline void forRows(int rows, int threads, Fn fn)
        {
            threads = std::max(1, std::min(threads, 64));
            if (threads == 1 || rows < threads * 4)
            {
                for (int y = 0; y < rows; ++y)
                {
                    fn(y);
                }
                return;
            }
            std::atomic<int> next(0);
            std::vector<std::thread> pool;
            pool.reserve((size_t)threads);
            for (int t = 0; t < threads; ++t)
            {
                pool.emplace_back([&]() {
                    for (int y = next++; y < rows; y = next++)
                    {
                        fn(y);
                    }
                });
            }
            for (std::thread& thread : pool)
            {
                thread.join();
            }
        }
    }

    inline BakeResult bake(const BakeParams& params)
    {
        BakeResult out;
        const int W = std::max(16, std::min(params.width, 8192));
        const int H = std::max(8, std::min(params.height, 4096));
        out.width = W;
        out.height = H;
        out.rgba.assign((size_t)W * (size_t)H * 4, 0.f);
        const detail::Plane plane(params.pole, params.centre);

        // The diffuse light of every texel (rows alone, so the threads do not meet).
        detail::forRows(H, params.threads, [&](int y) {
            const float v = ((float)y + 0.5f) / (float)H;
            for (int x = 0; x < W; ++x)
            {
                const float u = ((float)x + 0.5f) / (float)W;
                float l, b;
                detail::galacticOf(plane, detail::equirectDirection(u, v), l, b);
                float* texel = &out.rgba[((size_t)y * (size_t)W + (size_t)x) * 4];
                detail::diffuseLight(l, b, texel);
            }
        });

        // Normalised so that the brightest of the diffuse light is 1 - BAKE_STAR_SHARE (the grain
        // of the stars is the rest, so the whole stays about 1). Y is the luminance.
        auto luminance = [](const float* t) { return 0.2126f * t[0] + 0.7152f * t[1] + 0.0722f * t[2]; };
        float max_y = 0.f;
        for (size_t i = 0; i < (size_t)W * (size_t)H; ++i)
        {
            max_y = std::max(max_y, luminance(&out.rgba[i * 4]));
        }
        const float scale = max_y > 0.f ? (1.f - BAKE_STAR_SHARE) / max_y : 0.f;
        // Where stars may be: the density follows the diffuse light (a floor so that the rest of
        // the sky has grain too), and the rows are thinner towards the poles (cos latitude).
        std::vector<float> density((size_t)W * (size_t)H);
        double diffuse_sum = 0.0;
        for (size_t i = 0; i < density.size(); ++i)
        {
            float* t = &out.rgba[i * 4];
            const float y = luminance(t) * scale / (1.f - BAKE_STAR_SHARE);   // 0 .. 1
            density[i] = 0.02f + 0.98f * std::sqrt(clampf(y, 0.f, 1.f));
            t[0] *= scale;
            t[1] *= scale;
            t[2] *= scale;
            t[3] = 1.f;
            diffuse_sum += luminance(t);
        }

        // The grain: faint stars thrown by that density, each a small Gaussian whose widths keep
        // its size on the sphere (wider in x towards the poles); the Pareto-like fluxes, all together
        // BAKE_STAR_SHARE of the whole light.
        if (params.faint_stars > 0 && diffuse_sum > 0.0)
        {
            detail::Rng rng(0x6a1ac5eeu);
            std::vector<float> row_cos((size_t)H);
            for (int y = 0; y < H; ++y)
            {
                row_cos[(size_t)y] = std::cos(((((float)y + 0.5f) / (float)H) - 0.5f) * PI_F);
            }
            const double star_energy = diffuse_sum * (double)BAKE_STAR_SHARE / (double)(1.f - BAKE_STAR_SHARE);
            struct Seed { float x, y, flux; Color tint; };
            std::vector<float> weights;
            std::vector<Seed> seeds;
            seeds.reserve(params.faint_stars);
            double flux_sum = 0.0;
            const unsigned max_tries = params.faint_stars * 400u;
            for (unsigned tries = 0; seeds.size() < params.faint_stars && tries < max_tries; ++tries)
            {
                const int x = std::min((int)(rng.uniform() * (float)W), W - 1);
                const int y = std::min((int)(rng.uniform() * (float)H), H - 1);
                if (rng.uniform() > row_cos[(size_t)y] * density[(size_t)y * (size_t)W + (size_t)x])
                {
                    continue;
                }
                Seed seed;
                seed.x = (float)x + rng.uniform();
                seed.y = (float)y + rng.uniform();
                seed.flux = std::min(std::pow(1.f - rng.uniform(), -1.f / 1.3f), 30.f);
                seed.tint = tintOfTemperature(detail::randomTemperature(rng, 0.f));
                flux_sum += (double)seed.flux;
                seeds.push_back(seed);
            }
            for (const Seed& seed : seeds)
            {
                const float energy = (float)(star_energy * (double)seed.flux / flux_sum);
                const float lat_cos = std::max(row_cos[(size_t)std::min((int)seed.y, H - 1)], 0.1f);
                const float sigma_y = BAKE_STAR_SIGMA;
                const float sigma_x = BAKE_STAR_SIGMA / lat_cos;
                const int reach_y = (int)std::ceil(3.f * sigma_y);
                const int reach_x = (int)std::ceil(3.f * sigma_x);
                const int cx = (int)std::floor(seed.x);
                const int cy = (int)std::floor(seed.y);
                // the weights, normalised over the footprint that lies in the picture
                weights.clear();
                float sum = 0.f;
                for (int dy = -reach_y; dy <= reach_y; ++dy)
                {
                    for (int dx = -reach_x; dx <= reach_x; ++dx)
                    {
                        float w = 0.f;
                        if (cy + dy >= 0 && cy + dy < H)
                        {
                            const float ex = ((float)(cx + dx) + 0.5f - seed.x) / sigma_x;
                            const float ey = ((float)(cy + dy) + 0.5f - seed.y) / sigma_y;
                            w = std::exp(-0.5f * (ex * ex + ey * ey));
                        }
                        weights.push_back(w);
                        sum += w;
                    }
                }
                if (sum <= 0.f)
                {
                    continue;
                }
                size_t k = 0;
                for (int dy = -reach_y; dy <= reach_y; ++dy)
                {
                    for (int dx = -reach_x; dx <= reach_x; ++dx, ++k)
                    {
                        if (weights[k] <= 0.f)
                        {
                            continue;
                        }
                        const int px = ((cx + dx) % W + W) % W;     // round the longitude
                        float* t = &out.rgba[((size_t)(cy + dy) * (size_t)W + (size_t)px) * 4];
                        const float e = energy * weights[k] / sum;
                        t[0] += e * seed.tint.r;
                        t[1] += e * seed.tint.g;
                        t[2] += e * seed.tint.b;
                    }
                }
            }
        }
        return out;
    }
}

#endif // PLUS_STARFIELDMATH_H
