// Stand-alone test of the dense sky and the baked Milky Way in starfieldmath.h (research/224),
// not part of the viewer build:
//   cl /nologo /EHsc /std:c++17 /W4 /WX starfieldbake_test.cpp && starfieldbake_test.exe
#include "starfieldmath.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace plus::starfield::math;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)
static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

static double length(const Vec3& v) { return std::sqrt((double)dot(v, v)); }

// The real galactic pole and centre in J2000 (plus/realstars): the frame of the real sky.
static Vec3 unitOf(double ra_deg, double dec_deg)
{
    const double ra = ra_deg * 3.14159265358979 / 180.0;
    const double dec = dec_deg * 3.14159265358979 / 180.0;
    return { (float)(std::cos(dec) * std::cos(ra)), (float)(std::cos(dec) * std::sin(ra)), (float)std::sin(dec) };
}

static float luminanceOf(const float* t) { return 0.2126f * t[0] + 0.7152f * t[1] + 0.0722f * t[2]; }

int main()
{
    // the made-up plane
    Vec3 pole, centre;
    madeUpPlane(pole, centre);
    CHECK(near(length(pole), 1.0, 1e-5) && near(length(centre), 1.0, 1e-5));
    CHECK(near(dot(pole, centre), 0.0, 1e-5));
    CHECK(centre.z > 0.6f && centre.z < 0.9f);      // the centre stands well up in the sky

    // the equirect mapping
    {
        const Vec3 front = detail::equirectDirection(0.5f, 0.5f);
        CHECK(near(front.x, 1.0, 1e-5) && near(front.y, 0.0, 1e-5) && near(front.z, 0.0, 1e-5));
        CHECK(detail::equirectDirection(0.5f, 1.f).z > 0.9999f);
        CHECK(detail::equirectDirection(0.5f, 0.f).z < -0.9999f);
        const Vec3 east = detail::equirectDirection(0.75f, 0.5f);       // u 0.75: longitude +90 degrees
        CHECK(near(east.y, 1.0, 1e-5));
        // galacticOf is the inverse of Plane::direction
        const detail::Plane plane(pole, centre);
        for (float l = 0.3f; l < 6.2f; l += 0.9f)
        {
            for (float b = -1.2f; b <= 1.2f; b += 0.4f)
            {
                float l2, b2;
                detail::galacticOf(plane, plane.direction(l, b), l2, b2);
                CHECK(near(b2, b, 1e-4) && near(std::cos(l2), std::cos(l), 1e-4) && near(std::sin(l2), std::sin(l), 1e-4));
            }
        }
    }

    // ---- the dense sky ------------------------------------------------------------------------
    const Vec3 gal_pole = unitOf(192.85948, 27.12825);
    const Vec3 gal_centre = unitOf(266.405, -28.93617);
    {
        Catalogue field;
        // it appends: what is there stays
        field.star_dir.push_back({ 0.f, 0.f, 1.f });
        field.star_color.push_back({ 1.f, 1.f, 1.f, 0.9f });
        field.glow_dir.push_back({ 0.f, 1.f, 0.f });
        generateDenseOn(gal_pole, gal_centre, field);
        CHECK(field.star_dir.size() == 1 + DENSE_COUNT && field.star_color.size() == field.star_dir.size());
        CHECK(field.star_dir[0].z == 1.f && field.star_color[0].a == 0.9f);
        CHECK(field.glow_dir.size() == 1 && field.glow_color.empty());      // not touched

        const float level_faint = levelOfLight(DENSE_LIGHT_BRIGHTEST *
            std::pow(10.f, -0.4f * DENSE_COMPRESSION * (DENSE_MAG_FAINTEST - DENSE_MAG_BRIGHTEST)));
        const float level_bright = levelOfLight(DENSE_LIGHT_BRIGHTEST);
        unsigned south = 0, on_band = 0;
        double mean_level = 0;
        for (size_t i = 1; i < field.star_dir.size(); ++i)
        {
            const Vec3& d = field.star_dir[i];
            const Color& c = field.star_color[i];
            CHECK(near(length(d), 1.0, 1e-3));
            CHECK(c.a >= level_faint - 1e-4f && c.a <= level_bright + 1e-4f);
            CHECK(std::max({ c.r, c.g, c.b }) <= 1.0001f && std::min({ c.r, c.g, c.b }) > 0.3f);
            south += d.z < 0.f;
            on_band += std::fabs(dot(d, gal_pole)) < 0.149f;      // within 8.6 degrees of the plane
            mean_level += c.a;
        }
        const double n = (double)DENSE_COUNT;
        CHECK(south > DENSE_COUNT / 4);                 // the whole sphere (the real sky turns)
        // uniform stars have a share 0.149 of them in the band; the band's own are all in it
        CHECK(on_band / n > 0.35 && on_band / n < 0.65);
        mean_level /= n;
        CHECK(mean_level > level_faint && mean_level < level_bright);

        // the same every time
        Catalogue again;
        generateDenseOn(gal_pole, gal_centre, again);
        bool same = again.star_dir.size() == DENSE_COUNT;
        for (size_t i = 0; same && i < again.star_dir.size(); ++i)
        {
            same = again.star_dir[i].x == field.star_dir[i + 1].x && again.star_color[i].a == field.star_color[i + 1].a;
        }
        CHECK(same);

        // above the horizon only: the made-up sky
        Catalogue up;
        generateDenseOn(pole, centre, up, false);
        CHECK(up.star_dir.size() == DENSE_COUNT);
        bool above = true;
        for (const Vec3& d : up.star_dir)
        {
            above = above && d.z >= 0.f;
        }
        CHECK(above);
    }

    // ---- the baked Milky Way ------------------------------------------------------------------
    {
        BakeParams params;
        params.pole = gal_pole;
        params.centre = gal_centre;
        params.width = 1024;
        params.height = 512;
        params.faint_stars = 0;
        const BakeResult diffuse = bake(params);
        CHECK(diffuse.width == 1024 && diffuse.height == 512);
        CHECK(diffuse.rgba.size() == (size_t)1024 * 512 * 4);

        const detail::Plane plane(gal_pole, gal_centre);
        double max_y = 0;
        bool sane = true;
        for (size_t i = 0; i < (size_t)1024 * 512; ++i)
        {
            const float* t = &diffuse.rgba[i * 4];
            sane = sane && std::isfinite(t[0]) && std::isfinite(t[1]) && std::isfinite(t[2]) && t[0] >= 0.f && t[1] >= 0.f && t[2] >= 0.f && t[3] == 1.f;
            max_y = std::max(max_y, (double)luminanceOf(t));
        }
        CHECK(sane);
        CHECK(near(max_y, 1.0 - BAKE_STAR_SHARE, 1e-3));    // the diffuse light's brightest: 1 minus the grain's share

        // mean luminance by galactic latitude and longitude
        double near_plane = 0, away = 0, centre_side = 0, far_side = 0, lane = 0, clear = 0;
        unsigned n_near = 0, n_away = 0, n_centre = 0, n_far = 0, n_lane = 0, n_clear = 0;
        for (int y = 0; y < 512; ++y)
        {
            for (int x = 0; x < 1024; ++x)
            {
                const Vec3 d = detail::equirectDirection(((float)x + 0.5f) / 1024.f, ((float)y + 0.5f) / 512.f);
                float l, b;
                detail::galacticOf(plane, d, l, b);
                const float lc = detail::wrapAngle(l - CENTRE_LONGITUDE);
                const double lum = luminanceOf(&diffuse.rgba[((size_t)y * 1024 + (size_t)x) * 4]);
                if (std::fabs(b) < 0.10f)
                {
                    near_plane += lum;
                    ++n_near;
                    if (std::fabs(lc) < 0.5f) { centre_side += lum; ++n_centre; }
                    if (std::fabs(lc) > 2.4f) { far_side += lum; ++n_far; }
                }
                if (std::fabs(b) > 0.6f)
                {
                    away += lum;
                    ++n_away;
                }
                // the Great Rift: on the plane, the near side of the centre, against the far side of it
                if (std::fabs(b) < 0.02f && lc > -0.6f && lc < -0.1f) { lane += lum; ++n_lane; }
                if (std::fabs(b) < 0.02f && lc > 0.1f && lc < 0.6f) { clear += lum; ++n_clear; }
            }
        }
        CHECK(n_near > 1000 && n_away > 1000 && n_centre > 100 && n_far > 100 && n_lane > 20 && n_clear > 20);
        near_plane /= n_near; away /= n_away; centre_side /= n_centre; far_side /= n_far; lane /= n_lane; clear /= n_clear;
        std::printf("info: mean luminance: band %.4f, high latitude %.5f, centre %.4f, far side %.4f, lane %.4f, clear %.4f\n",
                    near_plane, away, centre_side, far_side, lane, clear);
        CHECK(near_plane > 8.0 * away);
        CHECK(centre_side > 1.8 * far_side);
        CHECK(lane < 0.85 * clear);

        // the longitude wraps: the first and last columns are neighbours
        double seam = 0, step = 0;
        for (int y = 0; y < 512; ++y)
        {
            const float* first = &diffuse.rgba[((size_t)y * 1024) * 4];
            const float* last = &diffuse.rgba[((size_t)y * 1024 + 1023) * 4];
            const float* second = &diffuse.rgba[((size_t)y * 1024 + 1) * 4];
            seam += std::fabs(luminanceOf(first) - luminanceOf(last));
            step += std::fabs(luminanceOf(first) - luminanceOf(second));
        }
        CHECK(seam < 3.0 * step + 1e-3);

        // the same picture for any number of threads
        BakeParams threaded = params;
        threaded.threads = 4;
        const BakeResult parallel = bake(threaded);
        CHECK(parallel.rgba == diffuse.rgba);

        // with the grain of faint stars: the light adds up to the diffuse light and its share
        BakeParams grained = params;
        grained.faint_stars = 60000;
        const BakeResult with_stars = bake(grained);
        double sum_diffuse = 0, sum_stars = 0;
        for (size_t i = 0; i < (size_t)1024 * 512; ++i)
        {
            sum_diffuse += luminanceOf(&diffuse.rgba[i * 4]);
            sum_stars += luminanceOf(&with_stars.rgba[i * 4]);
        }
        // the tints are below 1 in luminance, so a little less than the whole share lands
        const double expected = sum_diffuse * (1.0 + BAKE_STAR_SHARE / (1.0 - BAKE_STAR_SHARE));
        std::printf("info: light with stars %.1f, diffuse %.1f, if all the share landed %.1f\n", sum_stars, sum_diffuse, expected);
        CHECK(sum_stars > sum_diffuse * 1.15 && sum_stars < expected * 1.02);
        bool grain_sane = true;
        bool row_ends_ok = true;
        for (size_t i = 0; i < (size_t)1024 * 512; ++i)
        {
            const float* t = &with_stars.rgba[i * 4];
            grain_sane = grain_sane && std::isfinite(t[0] + t[1] + t[2]) && t[0] >= 0.f && t[1] >= 0.f && t[2] >= 0.f;
        }
        for (int x = 0; x < 1024; ++x)
        {
            row_ends_ok = row_ends_ok && std::isfinite(with_stars.rgba[(size_t)x * 4]) &&
                          std::isfinite(with_stars.rgba[((size_t)511 * 1024 + (size_t)x) * 4]);
        }
        CHECK(grain_sane && row_ends_ok);

        // the grain breaks the band up: more variation along the plane with the stars than without
        auto roughness = [&](const BakeResult& r)
        {
            double acc = 0;
            unsigned n = 0;
            for (int y = 0; y < 512; ++y)
            {
                for (int x = 0; x + 1 < 1024; ++x)
                {
                    const Vec3 d = detail::equirectDirection(((float)x + 0.5f) / 1024.f, ((float)y + 0.5f) / 512.f);
                    float l, b;
                    detail::galacticOf(plane, d, l, b);
                    if (std::fabs(b) < 0.08f)
                    {
                        acc += std::fabs(luminanceOf(&r.rgba[((size_t)y * 1024 + (size_t)x) * 4]) - luminanceOf(&r.rgba[((size_t)y * 1024 + (size_t)x + 1) * 4]));
                        ++n;
                    }
                }
            }
            return acc / (double)n;
        };
        CHECK(roughness(with_stars) > roughness(diffuse));

        // the same every time
        const BakeResult again = bake(grained);
        CHECK(again.rgba == with_stars.rgba);
    }

    // sizes are held in range and the made-up sky bakes too
    {
        BakeParams tiny;
        tiny.pole = pole;
        tiny.centre = centre;
        tiny.width = 1;
        tiny.height = 1;
        tiny.faint_stars = 100;
        const BakeResult r = bake(tiny);
        CHECK(r.width >= 16 && r.height >= 8 && r.rgba.size() == (size_t)r.width * r.height * 4);
        bool finite = true;
        for (float v : r.rgba)
        {
            finite = finite && std::isfinite(v) && v >= 0.f;
        }
        CHECK(finite);
    }

    // the full-size bake: how long it takes on one thread (it runs on a worker, once), and, with
    // STARFIELD_BAKE_DUMP=<file.ppm>, the picture to look at (tone mapped x / (1 + x), sRGB)
    {
        BakeParams full;
        full.pole = gal_pole;
        full.centre = gal_centre;
        const auto start = std::chrono::steady_clock::now();
        const BakeResult picture = bake(full);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("info: full bake %dx%d, %u faint stars, one thread: %.2f s\n", picture.width, picture.height, full.faint_stars, seconds);
        CHECK(seconds < 20.0);
        if (const char* path = std::getenv("STARFIELD_BAKE_DUMP"))
        {
            if (std::FILE* file = std::fopen(path, "wb"))
            {
                std::fprintf(file, "P6\n%d %d\n255\n", picture.width, picture.height);
                for (int y = picture.height - 1; y >= 0; --y)     // the top of the picture is the zenith
                {
                    for (int x = 0; x < picture.width; ++x)
                    {
                        for (int c = 0; c < 3; ++c)
                        {
                            const float v = picture.rgba[((size_t)y * (size_t)picture.width + (size_t)x) * 4 + (size_t)c];
                            const float t = v / (1.f + v);
                            const float s = t <= 0.0031308f ? 12.92f * t : 1.055f * std::pow(t, 1.f / 2.4f) - 0.055f;
                            std::fputc((int)clampf(s * 255.f + 0.5f, 0.f, 255.f), file);
                        }
                    }
                }
                std::fclose(file);
            }
        }
    }

    std::printf(failures ? "starfieldbake_test: %d FAILED\n" : "starfieldbake_test: ok\n", failures);
    return failures ? 1 : 0;
}
