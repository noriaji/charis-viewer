// Stand-alone test of starfieldmath.h (not part of the viewer build):
//   cl /nologo /EHsc /std:c++17 /W4 /WX starfield_test.cpp && starfield_test.exe
#include "starfieldmath.h"

#include <cstdio>

using namespace plus::starfield::math;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)
static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

// The apparent magnitude a star of this light was made with (addStar's mapping, backwards).
static double magnitudeOfLight(double light)
{
    return MAG_BRIGHTEST - std::log10(light) / (0.4 * MAG_COMPRESSION);
}

static double length(const Vec3& v) { return std::sqrt((double)dot(v, v)); }

int main()
{
    // the level of a light and back
    CHECK(near(levelOfLight(1.f), 1.0));
    CHECK(near(lightOfLevel(1.f), 1.0));
    CHECK(near(lightOfLevel(levelOfLight(0.25f)), 0.25, 1e-5));
    CHECK(near(lightOfLevel(levelOfLight(0.01f)), 0.01, 1e-5));
    CHECK(levelOfLight(0.f) >= 0.f && levelOfLight(0.f) <= 1.f);
    CHECK(levelOfLight(5.f) == 1.f);
    CHECK(near(lightOfLevel(0.f), std::pow(2.0, -(double)LEVEL_STOPS), 1e-9));
    // an 8-bit alpha (the vertex colour) keeps the light to within 4% per step
    {
        const float level = levelOfLight(0.037f);
        const float q = std::round(level * 255.f) / 255.f;
        CHECK(std::fabs(lightOfLevel(q) / 0.037f - 1.f) < 0.04f);
    }

    // the temperature: cool is red, hot is blue, the brightest channel is 1, a hint of white
    {
        const Color cool = tintOfTemperature(3000.f);
        const Color sun = tintOfTemperature(5800.f);
        const Color hot = tintOfTemperature(20000.f);
        CHECK(cool.r > cool.b && hot.b > hot.r);
        CHECK(cool.r > sun.r * 0.99f && cool.b < sun.b);
        CHECK(near(std::max({ cool.r, cool.g, cool.b }), 1.0, 1e-5));
        CHECK(near(std::max({ hot.r, hot.g, hot.b }), 1.0, 1e-5));
        for (float t = 1000.f; t <= 40000.f; t += 500.f)
        {
            const Color c = tintOfTemperature(t);
            CHECK(c.r > 0.f && c.g > 0.f && c.b > 0.f);
            CHECK(c.r <= 1.0001f && c.g <= 1.0001f && c.b <= 1.0001f);
            // not saturated: a share of the way to white keeps the weakest channel above 0.3
            CHECK(std::min({ c.r, c.g, c.b }) > 0.3f);
        }
        // out of range is clamped, not NaN
        CHECK(std::isfinite(tintOfTemperature(-5.f).r) && std::isfinite(tintOfTemperature(1e9f).b));
    }

    // the modes
    Catalogue off;
    generate(MODE_OFFICIAL, off);
    CHECK(off.star_dir.empty() && off.glow_dir.empty());

    Catalogue field;
    generate(MODE_FIELD, field);
    CHECK(field.star_dir.size() == STAR_COUNT);
    CHECK(field.star_color.size() == field.star_dir.size());
    CHECK(field.glow_dir.empty() && field.glow_color.empty() && field.glow_radius.empty());

    Catalogue sky;
    generate(MODE_MILKY_WAY, sky);
    CHECK(sky.star_dir.size() > STAR_COUNT);
    CHECK(sky.star_dir.size() <= STAR_COUNT + BAND_STAR_COUNT);
    CHECK(sky.star_dir.size() >= STAR_COUNT + BAND_STAR_COUNT * 95 / 100);
    const size_t glow_wanted = GLOW_LARGE + GLOW_MEDIUM + GLOW_SMALL;
    CHECK(sky.glow_dir.size() <= glow_wanted);
    CHECK(sky.glow_dir.size() >= glow_wanted * 95 / 100);
    CHECK(sky.glow_color.size() == sky.glow_dir.size() && sky.glow_radius.size() == sky.glow_dir.size());

    // the same sky every time, and the field's stars are the star field's
    {
        Catalogue again;
        generate(MODE_MILKY_WAY, again);
        CHECK(again.star_dir.size() == sky.star_dir.size() && again.glow_dir.size() == sky.glow_dir.size());
        bool same = again.star_dir.size() == sky.star_dir.size();
        for (size_t i = 0; same && i < sky.star_dir.size(); ++i)
        {
            same = again.star_dir[i].x == sky.star_dir[i].x && again.star_color[i].a == sky.star_color[i].a;
        }
        CHECK(same);
        bool prefix = true;
        for (size_t i = 0; i < field.star_dir.size(); ++i)
        {
            prefix = prefix && field.star_dir[i].x == sky.star_dir[i].x && field.star_dir[i].z == sky.star_dir[i].z;
        }
        CHECK(prefix);
    }

    // every direction is a unit vector in the sky; every colour in range
    for (const Catalogue* c : { &field, &sky })
    {
        for (size_t i = 0; i < c->star_dir.size(); ++i)
        {
            CHECK(near(length(c->star_dir[i]), 1.0, 1e-3));
            CHECK(c->star_dir[i].z >= 0.f);
            const Color& k = c->star_color[i];
            CHECK(k.a >= 0.f && k.a <= 1.f && k.r > 0.f && k.g > 0.f && k.b > 0.f);
            CHECK(std::max({ k.r, k.g, k.b }) <= 1.0001f);
        }
    }
    for (size_t i = 0; i < sky.glow_dir.size(); ++i)
    {
        CHECK(near(length(sky.glow_dir[i]), 1.0, 1e-3));
        CHECK(sky.glow_dir[i].z >= -0.0401f);
        CHECK(sky.glow_radius[i] >= 0.009f && sky.glow_radius[i] <= 0.121f);
        const Color& k = sky.glow_color[i];
        CHECK(k.a >= 0.f && k.a <= 1.f && k.r > 0.f && k.g > 0.f && k.b > 0.f && k.r <= 1.f && k.g <= 1.f && k.b <= 1.f);
    }

    // the star field's directions are uniform over the hemisphere: z is spread evenly
    {
        unsigned low = 0, mid = 0, high = 0;
        for (const Vec3& d : field.star_dir)
        {
            (d.z < 0.333f ? low : d.z < 0.667f ? mid : high)++;
        }
        const double n = (double)field.star_dir.size();
        CHECK(std::fabs(low / n - 1.0 / 3.0) < 0.03 && std::fabs(mid / n - 1.0 / 3.0) < 0.03 && std::fabs(high / n - 1.0 / 3.0) < 0.03);
        // and around: each quarter of the horizon gets about a quarter
        unsigned quarter[4] = {};
        for (const Vec3& d : field.star_dir)
        {
            const int q = (d.x >= 0.f ? 0 : 1) + (d.y >= 0.f ? 0 : 2);
            ++quarter[q];
        }
        for (unsigned q : quarter)
        {
            CHECK(std::fabs(q / n - 0.25) < 0.03);
        }
    }

    // magnitudes: few bright ones, the number growing about 10^0.5 per magnitude
    {
        unsigned bin[8] = {};      // [-1,0) [0,1) ... [6,7)
        unsigned brighter_than_2 = 0, brighter_than_0 = 0;
        for (const Color& k : field.star_color)
        {
            const double m = magnitudeOfLight(lightOfLevel(k.a));
            CHECK(m > MAG_BRIGHTEST - 0.2 && m < MAG_FAINTEST + 0.2);
            const int b = (int)std::floor(m - MAG_BRIGHTEST);
            if (b >= 0 && b < 8)
            {
                ++bin[b];
            }
            brighter_than_2 += m < 2.0;
            brighter_than_0 += m < 0.0;
        }
        CHECK(brighter_than_2 >= 10 && brighter_than_2 <= 60);
        CHECK(brighter_than_0 >= 1 && brighter_than_0 <= 12);
        for (int b = 3; b < 7; ++b)
        {
            const double ratio = (double)bin[b + 1] / (double)std::max(bin[b], 1u);
            CHECK(ratio > 2.4 && ratio < 4.2);
        }
        // the median star is faint
        std::vector<float> levels;
        for (const Color& k : field.star_color)
        {
            levels.push_back(k.a);
        }
        std::sort(levels.begin(), levels.end());
        CHECK(magnitudeOfLight(lightOfLevel(levels[levels.size() / 2])) > 5.0);
    }

    // colours: the cool ones are the most, a good few are blue-white, the hot ones lean bright
    {
        unsigned red = 0, blue = 0;
        double hot_faint = 0, hot_bright = 0, n_faint = 0, n_bright = 0;
        for (const Color& k : field.star_color)
        {
            const bool is_blue = k.b > k.r;
            red += !is_blue;
            blue += is_blue;
            if (lightOfLevel(k.a) > 0.1f)
            {
                n_bright += 1;
                hot_bright += is_blue;
            }
            else
            {
                n_faint += 1;
                hot_faint += is_blue;
            }
        }
        CHECK(red > blue);
        CHECK(blue > field.star_color.size() / 10);
        CHECK(n_bright > 100 && n_faint > 100);
        CHECK(hot_bright / n_bright > hot_faint / n_faint);
    }

    // the Milky Way: a band along the plane, thick and warm to the centre, a dark rift
    {
        const detail::Plane plane;
        CHECK(near(length(plane.n), 1.0, 1e-5) && near(length(plane.e1), 1.0, 1e-5) && near(length(plane.e2), 1.0, 1e-5));
        CHECK(near(dot(plane.n, plane.e1), 0.0, 1e-5) && near(dot(plane.n, plane.e2), 0.0, 1e-5) && near(dot(plane.e1, plane.e2), 0.0, 1e-5));
        // the centre is the highest point of the band, well up in the sky
        const Vec3 top = plane.direction(CENTRE_LONGITUDE, 0.f);
        CHECK(top.z > 0.6f && top.z < 0.9f);

        // the patches lie near the plane: |dot(dir, n)| = |sin b|
        double sum_abs_b = 0;
        unsigned near_centre = 0, far_side = 0;
        for (const Vec3& d : sky.glow_dir)
        {
            sum_abs_b += std::fabs(dot(d, plane.n));
            const float l = std::atan2(dot(d, plane.e2), dot(d, plane.e1));
            const float dl = std::fabs(detail::wrapAngle(l - CENTRE_LONGITUDE));
            near_centre += dl < 0.52f;
            far_side += dl > PI_F - 0.52f;
        }
        CHECK(sum_abs_b / (double)sky.glow_dir.size() < 0.20);
        CHECK(near_centre > far_side * 3);

        // the warm colour is where the band is bright
        double warm_centre = 0, warm_far = 0;
        unsigned n_centre = 0, n_far = 0;
        for (size_t i = 0; i < sky.glow_dir.size(); ++i)
        {
            const Vec3& d = sky.glow_dir[i];
            const float l = std::atan2(dot(d, plane.e2), dot(d, plane.e1));
            const float dl = std::fabs(detail::wrapAngle(l - CENTRE_LONGITUDE));
            const Color& k = sky.glow_color[i];
            if (dl < 0.5f)
            {
                warm_centre += k.r - k.b;
                ++n_centre;
            }
            else if (dl > 1.6f)
            {
                warm_far += k.r - k.b;
                ++n_far;
            }
        }
        std::printf("info: patches near the centre %u, far %u\n", n_centre, n_far);
        CHECK(n_centre > 50 && n_far > 20);
        CHECK(warm_centre / n_centre > warm_far / n_far);

        // the rift: along the plane on the near side of the centre, the thin line of the plane
        // is emptier than the same line on the other side of the centre
        auto count_on_plane = [&](float l_lo, float l_hi)
        {
            unsigned n = 0;
            for (const Vec3& d : sky.glow_dir)
            {
                const float l = std::atan2(dot(d, plane.e2), dot(d, plane.e1));
                const float b = std::asin(clampf(dot(d, plane.n), -1.f, 1.f));
                const float dl = detail::wrapAngle(l - CENTRE_LONGITUDE);
                n += dl > l_lo && dl < l_hi && std::fabs(b) < 0.03f;
            }
            return n;
        };
        const unsigned rift = count_on_plane(-0.6f, -0.1f);
        const unsigned clear = count_on_plane(0.1f, 0.6f);
        CHECK(clear > 20);
        CHECK(rift * 10 < clear * 7);
    }

    // the dust also thins the faint stars of the band, never below the horizon
    for (size_t i = STAR_COUNT; i < sky.star_dir.size(); ++i)
    {
        CHECK(sky.star_dir[i].z >= 0.f);
        CHECK(magnitudeOfLight(lightOfLevel(sky.star_color[i].a)) > BAND_MAG_BRIGHTEST - 0.2);
    }

    // the noise wraps around the circle: the longitudes 0 and a whole period apart agree
    {
        for (float y = -0.5f; y <= 0.5f; y += 0.1f)
        {
            CHECK(near(detail::fbm(0.3f, y, 8, 11), detail::fbm(8.3f, y, 8, 11), 1e-5));
        }
        const float v = detail::fbm(2.7f, 0.4f, 8, 5);
        CHECK(v >= 0.f && v <= 1.f);
    }

    std::printf(failures ? "starfield_test: %d FAILED\n" : "starfield_test: ok\n", failures);
    return failures ? 1 : 0;
}
