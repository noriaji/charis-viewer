/**
 * @file lutfile.cpp
 * @brief Colour lookup tables (LUTs) from Adobe .cube text and from strip
 *        images, as one 3D table.
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

// Written from the formats' public descriptions (the Adobe Cube LUT
// specification 1.0; the strip layout of ReShade's LUT.fx as research/102
// describes it). No code taken from another viewer or tool.
// Standard C++ only: the viewer build force-includes its precompiled header,
// the stand-alone test (tools/looks/lut_test.cpp) does not need it.

#include "lutfile.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <string_view>
#include <system_error>

namespace
{
    using plus::look::LutTable;

    bool isBlank(char c)
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
    }

    std::string_view trim(std::string_view s)
    {
        while (!s.empty() && isBlank(s.front()))
        {
            s.remove_prefix(1);
        }
        while (!s.empty() && isBlank(s.back()))
        {
            s.remove_suffix(1);
        }
        return s;
    }

    // Exactly `count` numbers separated by blanks (the whole of s). Locale
    // independent; anything that is not a finite number fails.
    bool parseNumbers(std::string_view s, float* out, unsigned count)
    {
        const char* p = s.data();
        const char* const end = p + s.size();
        for (unsigned i = 0; i < count; ++i)
        {
            while (p < end && isBlank(*p))
            {
                ++p;
            }
            if (p < end && *p == '+')
            {
                ++p;    // from_chars takes no plus sign
            }
            const std::from_chars_result result = std::from_chars(p, end, out[i]);
            if (result.ec != std::errc() || result.ptr == p || !std::isfinite(out[i]))
            {
                return false;
            }
            p = result.ptr;
        }
        while (p < end && isBlank(*p))
        {
            ++p;
        }
        return p == end;
    }

    bool parseUnsigned(std::string_view s, unsigned& out)
    {
        s = trim(s);
        const std::from_chars_result result = std::from_chars(s.data(), s.data() + s.size(), out);
        return result.ec == std::errc() && result.ptr == s.data() + s.size() && !s.empty();
    }

    std::string lineError(unsigned line, const std::string& what)
    {
        return "line " + std::to_string(line) + ": " + what;
    }

    // A 1D table (n rows, one curve a channel) as a 3D one: each channel goes
    // through its own curve, linearly between the rows.
    void expand1D(const std::vector<float>& curves, unsigned n, LutTable& out)
    {
        const unsigned size = std::min(n, plus::look::MAX_LUT_SIZE);
        std::vector<float> at((size_t)size * 3);     // the curves at the 3D table's steps
        for (unsigned i = 0; i < size; ++i)
        {
            const float x = (float)i / (float)(size - 1) * (float)(n - 1);
            const unsigned i0 = std::min((unsigned)x, n - 1);
            const unsigned i1 = std::min(i0 + 1, n - 1);
            const float f = x - (float)i0;
            for (unsigned c = 0; c < 3; ++c)
            {
                at[(size_t)i * 3 + c] = curves[(size_t)i0 * 3 + c] * (1.f - f) + curves[(size_t)i1 * 3 + c] * f;
            }
        }
        out.size = size;
        out.rgb.resize((size_t)size * size * size * 3);
        size_t entry = 0;
        for (unsigned b = 0; b < size; ++b)
        {
            for (unsigned g = 0; g < size; ++g)
            {
                for (unsigned r = 0; r < size; ++r, ++entry)
                {
                    out.rgb[entry * 3 + 0] = at[(size_t)r * 3 + 0];
                    out.rgb[entry * 3 + 1] = at[(size_t)g * 3 + 1];
                    out.rgb[entry * 3 + 2] = at[(size_t)b * 3 + 2];
                }
            }
        }
    }
}

namespace plus::look
{
    bool parseCube(const std::string& text, LutTable& out, std::string& error)
    {
        out = LutTable();
        if (text.size() > MAX_CUBE_FILE_BYTES)
        {
            error = "the file is larger than " + std::to_string(MAX_CUBE_FILE_BYTES >> 20) + " MB";
            return false;
        }
        std::string_view all(text);
        constexpr std::string_view UTF8_BOM = "\xEF\xBB\xBF";
        if (all.substr(0, UTF8_BOM.size()) == UTF8_BOM)
        {
            all.remove_prefix(UTF8_BOM.size());
        }

        unsigned size_3d = 0;
        unsigned size_1d = 0;
        float domain_min[3] = { 0.f, 0.f, 0.f };
        float domain_max[3] = { 1.f, 1.f, 1.f };
        std::vector<float> values;
        size_t expected = 0;        // numbers in the table, once its first row is read
        unsigned line_number = 0;
        size_t start = 0;
        while (start < all.size())
        {
            size_t end = all.find('\n', start);
            if (end == std::string_view::npos)
            {
                end = all.size();
            }
            ++line_number;
            const std::string_view line = trim(all.substr(start, end - start));
            start = end + 1;
            if (line.empty() || line.front() == '#')
            {
                continue;
            }

            // Keywords are upper case (a lower-case "nan" or "inf" is a broken row).
            if (std::isupper((unsigned char)line.front()))
            {
                if (!values.empty())
                {
                    error = lineError(line_number, "a keyword after the table's rows");
                    return false;
                }
                const size_t keyword_end = (size_t)(std::find_if(line.begin(), line.end(), isBlank) - line.begin());
                const std::string_view keyword = line.substr(0, keyword_end);
                const std::string_view rest = trim(line.substr(keyword_end));
                bool ok = true;
                if (keyword == "TITLE")
                {
                    std::string_view title = rest;
                    if (title.size() >= 2 && title.front() == '"' && title.back() == '"')
                    {
                        title = title.substr(1, title.size() - 2);
                    }
                    out.title = std::string(title);
                }
                else if (keyword == "LUT_3D_SIZE")
                {
                    ok = parseUnsigned(rest, size_3d) && size_3d >= MIN_LUT_SIZE && size_3d <= MAX_LUT_SIZE;
                    if (!ok)
                    {
                        error = lineError(line_number, "LUT_3D_SIZE must be " + std::to_string(MIN_LUT_SIZE) + " to " +
                                                       std::to_string(MAX_LUT_SIZE));
                        return false;
                    }
                }
                else if (keyword == "LUT_1D_SIZE")
                {
                    ok = parseUnsigned(rest, size_1d) && size_1d >= MIN_LUT_SIZE && size_1d <= MAX_1D_LUT_SIZE;
                }
                else if (keyword == "DOMAIN_MIN")
                {
                    ok = parseNumbers(rest, domain_min, 3);
                }
                else if (keyword == "DOMAIN_MAX")
                {
                    ok = parseNumbers(rest, domain_max, 3);
                }
                else if (keyword == "LUT_3D_INPUT_RANGE" || keyword == "LUT_1D_INPUT_RANGE")
                {
                    float range[2];
                    ok = parseNumbers(rest, range, 2);
                    for (unsigned c = 0; ok && c < 3; ++c)
                    {
                        domain_min[c] = range[0];
                        domain_max[c] = range[1];
                    }
                }
                // Other keywords (some programs add their own) are skipped.
                if (!ok)
                {
                    error = lineError(line_number, "cannot read " + std::string(keyword));
                    return false;
                }
                continue;
            }

            // A row of the table.
            if (!expected)
            {
                if (size_3d && size_1d)
                {
                    error = "a 1D table before a 3D one (a shaper LUT) is not supported";
                    return false;
                }
                if (!size_3d && !size_1d)
                {
                    error = lineError(line_number, "numbers before LUT_3D_SIZE or LUT_1D_SIZE");
                    return false;
                }
                expected = size_3d ? (size_t)size_3d * size_3d * size_3d * 3 : (size_t)size_1d * 3;
                values.reserve(expected);
            }
            if (values.size() >= expected)
            {
                error = lineError(line_number, "more rows than the size gives");
                return false;
            }
            float row[3];
            if (!parseNumbers(line, row, 3))
            {
                error = lineError(line_number, "not three numbers");
                return false;
            }
            values.insert(values.end(), row, row + 3);
        }

        if (!expected)
        {
            error = "no table (LUT_3D_SIZE or LUT_1D_SIZE and its rows)";
            return false;
        }
        if (values.size() != expected)
        {
            error = std::to_string(values.size() / 3) + " rows where the size gives " + std::to_string(expected / 3);
            return false;
        }
        for (unsigned c = 0; c < 3; ++c)
        {
            if (!(domain_max[c] > domain_min[c]))
            {
                error = "DOMAIN_MAX must be above DOMAIN_MIN";
                return false;
            }
            out.domain_min[c] = domain_min[c];
            out.domain_max[c] = domain_max[c];
        }
        if (size_3d)
        {
            out.size = size_3d;
            out.rgb = std::move(values);
        }
        else
        {
            expand1D(values, size_1d, out);
        }
        return true;
    }

    bool stripLayout(unsigned width, unsigned height, StripLayout& out)
    {
        const unsigned size = (unsigned)std::lround(std::sqrt((double)width));
        if (size < MIN_LUT_SIZE || size > MAX_LUT_SIZE || size * size != width || !height || height % size ||
            height / size > MAX_STRIPS)
        {
            return false;
        }
        out.size = size;
        out.count = height / size;
        return true;
    }

    bool parseStrip(const std::uint8_t* pixels, unsigned width, unsigned height, unsigned components, bool bottom_up,
                    unsigned strip, LutTable& out, bool& flipped, std::string& error)
    {
        out = LutTable();
        flipped = false;
        StripLayout layout;
        if (!pixels || !stripLayout(width, height, layout))
        {
            error = std::to_string(width) + "x" + std::to_string(height) + " is not a LUT strip (N tiles of NxN side by side, N " +
                    std::to_string(MIN_LUT_SIZE) + " to " + std::to_string(MAX_LUT_SIZE) + ")";
            return false;
        }
        if (components != 3 && components != 4)
        {
            error = "the image must be RGB or RGBA";
            return false;
        }
        if (strip >= layout.count)
        {
            error = "strip " + std::to_string(strip) + " asked for, the image has " + std::to_string(layout.count);
            return false;
        }

        const unsigned n = layout.size;
        const unsigned top = strip * n;
        auto pixel = [&](unsigned x, unsigned y_from_top) -> const std::uint8_t*
        {
            const unsigned row = bottom_up ? height - 1 - y_from_top : y_from_top;
            return pixels + ((size_t)row * width + x) * components;
        };

        // Which way green grows. Down the strip the colours change as along
        // red in a tile (both brighter for most looks, both darker for an
        // inverting one); the other way round, the strip is upside down.
        // When red changes nothing, the colours must grow downwards.
        std::int64_t down = 0;      // the last row against the first
        std::int64_t across = 0;    // the last column of each tile against its first
        for (unsigned x = 0; x < width; ++x)
        {
            for (unsigned c = 0; c < 3; ++c)
            {
                down += (std::int64_t)pixel(x, top + n - 1)[c] - (std::int64_t)pixel(x, top)[c];
            }
        }
        for (unsigned y = top; y < top + n; ++y)
        {
            for (unsigned tile = 0; tile < n; ++tile)
            {
                for (unsigned c = 0; c < 3; ++c)
                {
                    across += (std::int64_t)pixel(tile * n + n - 1, y)[c] - (std::int64_t)pixel(tile * n, y)[c];
                }
            }
        }
        if (down == 0)
        {
            flipped = false;            // green changes nothing: either way reads the same
        }
        else if (across == 0)
        {
            flipped = down < 0;
        }
        else
        {
            flipped = (down > 0) != (across > 0);
        }

        out.size = n;
        out.rgb.resize((size_t)n * n * n * 3);
        size_t entry = 0;
        for (unsigned b = 0; b < n; ++b)
        {
            for (unsigned g = 0; g < n; ++g)
            {
                const unsigned y = top + (flipped ? n - 1 - g : g);
                for (unsigned r = 0; r < n; ++r, ++entry)
                {
                    const std::uint8_t* p = pixel(b * n + r, y);
                    for (unsigned c = 0; c < 3; ++c)
                    {
                        out.rgb[entry * 3 + c] = (float)p[c] / 255.f;
                    }
                }
            }
        }
        return true;
    }

    void makeIdentity(unsigned size, LutTable& out)
    {
        out = LutTable();
        size = std::clamp(size, MIN_LUT_SIZE, MAX_LUT_SIZE);
        out.size = size;
        out.rgb.resize((size_t)size * size * size * 3);
        size_t entry = 0;
        for (unsigned b = 0; b < size; ++b)
        {
            for (unsigned g = 0; g < size; ++g)
            {
                for (unsigned r = 0; r < size; ++r, ++entry)
                {
                    out.rgb[entry * 3 + 0] = (float)r / (float)(size - 1);
                    out.rgb[entry * 3 + 1] = (float)g / (float)(size - 1);
                    out.rgb[entry * 3 + 2] = (float)b / (float)(size - 1);
                }
            }
        }
    }

    unsigned gpuSize(unsigned size)
    {
        return size <= 16 ? 16 : size <= 32 ? 32 : 64;
    }

    void apply(const LutTable& table, const float colour[3], float out[3])
    {
        const unsigned n = table.size;
        unsigned lower[3];
        float blend[3];
        for (unsigned c = 0; c < 3; ++c)
        {
            const float span = table.domain_max[c] - table.domain_min[c];
            const float x = std::clamp((colour[c] - table.domain_min[c]) / span, 0.f, 1.f) * (float)(n - 1);
            lower[c] = std::min((unsigned)x, n - 2);
            blend[c] = x - (float)lower[c];
        }
        for (unsigned c = 0; c < 3; ++c)
        {
            float sum = 0.f;
            for (unsigned corner = 0; corner < 8; ++corner)
            {
                const unsigned step[3] = { corner & 1, (corner >> 1) & 1, (corner >> 2) & 1 };
                float weight = 1.f;
                for (unsigned k = 0; k < 3; ++k)
                {
                    weight *= step[k] ? blend[k] : 1.f - blend[k];
                }
                const size_t entry = ((size_t)(lower[2] + step[2]) * n + lower[1] + step[1]) * n + lower[0] + step[0];
                sum += weight * table.rgb[entry * 3 + c];
            }
            out[c] = sum;
        }
    }

    LutTable resample(const LutTable& table, unsigned size)
    {
        LutTable out;
        out.size = std::clamp(size, MIN_LUT_SIZE, MAX_STRIP_SIZE);
        out.title = table.title;
        // The finished image's colours are 0..1: a wider domain (HDR or log
        // LUTs) is narrowed to that, so no entry is spent where no colour is.
        for (unsigned c = 0; c < 3; ++c)
        {
            const float low = std::max(table.domain_min[c], 0.f);
            const float high = std::min(table.domain_max[c], 1.f);
            const bool overlaps = high > low;
            out.domain_min[c] = overlaps ? low : table.domain_min[c];
            out.domain_max[c] = overlaps ? high : table.domain_max[c];
        }
        const unsigned n = out.size;
        out.rgb.resize((size_t)n * n * n * 3);
        size_t entry = 0;
        for (unsigned b = 0; b < n; ++b)
        {
            for (unsigned g = 0; g < n; ++g)
            {
                for (unsigned r = 0; r < n; ++r, ++entry)
                {
                    const unsigned index[3] = { r, g, b };
                    float colour[3];
                    for (unsigned c = 0; c < 3; ++c)
                    {
                        colour[c] = out.domain_min[c] + (float)index[c] / (float)(n - 1) * (out.domain_max[c] - out.domain_min[c]);
                    }
                    apply(table, colour, &out.rgb[entry * 3]);
                }
            }
        }
        return out;
    }

    std::vector<std::uint8_t> toStrip8(const LutTable& table)
    {
        const unsigned n = table.size;
        std::vector<std::uint8_t> strip((size_t)n * n * n * 4);
        size_t entry = 0;
        for (unsigned b = 0; b < n; ++b)
        {
            for (unsigned g = 0; g < n; ++g)
            {
                for (unsigned r = 0; r < n; ++r, ++entry)
                {
                    std::uint8_t* texel = &strip[((size_t)g * n * n + (size_t)b * n + r) * 4];
                    for (unsigned c = 0; c < 3; ++c)
                    {
                        texel[c] = (std::uint8_t)std::lround(std::clamp(table.rgb[entry * 3 + c], 0.f, 1.f) * 255.f);
                    }
                    texel[3] = 255;
                }
            }
        }
        return strip;
    }

    float distanceFromNeutral(const LutTable& table)
    {
        const unsigned n = table.size;
        if (n < MIN_LUT_SIZE)
        {
            return 0.f;
        }
        float largest = 0.f;
        size_t entry = 0;
        for (unsigned b = 0; b < n; ++b)
        {
            for (unsigned g = 0; g < n; ++g)
            {
                for (unsigned r = 0; r < n; ++r, ++entry)
                {
                    const unsigned index[3] = { r, g, b };
                    for (unsigned c = 0; c < 3; ++c)
                    {
                        const float neutral = table.domain_min[c] +
                                              (float)index[c] / (float)(n - 1) * (table.domain_max[c] - table.domain_min[c]);
                        largest = std::max(largest, std::fabs(table.rgb[entry * 3 + c] - neutral));
                    }
                }
            }
        }
        return largest;
    }

    bool looksLogEncoded(const std::string& text)
    {
        std::string lower(text);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        static const char* const LOG_NAMES[] = { "logc", "log-c", "log_c", "slog", "s-log", "s_log", "vlog", "v-log",
                                                 "v_log", "clog", "c-log", "c_log", "flog", "f-log", "f_log", "nlog",
                                                 "n-log", "n_log", "log3g", "redlog" };
        for (const char* name : LOG_NAMES)
        {
            if (lower.find(name) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }
}
