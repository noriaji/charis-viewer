/**
 * @file posehistory.cpp
 * @brief Pose history for the poser MCP. See posehistory.h.
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

// No viewer headers on purpose (and so no precompiled header): this file is
// also built by the standalone test in tools/poser-mcp/ik. llrhihash.h (and the
// stdtypes.h it takes) need only the standard library.

#include "posehistory.h"

#include "llrhihash.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace fs = std::filesystem;

namespace plus::posermcp::history
{
    namespace
    {
        constexpr char          FILE_MAGIC[]     = "CharisPoseHistory";
        constexpr int           FILE_VERSION     = 1;
        constexpr char          FILE_PREFIX      = 'h';
        constexpr char          FILE_EXTENSION[] = ".pose";
        constexpr char          TEMP_EXTENSION[] = ".tmp";
        constexpr std::uintmax_t MAX_FILE_BYTES  = 8u * 1024u * 1024u;
        constexpr std::size_t   MAX_JOINTS       = 2000;
        constexpr std::size_t   MAX_TOP          = 5;
        constexpr double        HALF_TURN_DEG    = 180.0;
        constexpr double        HALF_TURN_RAD    = 3.14159265358979323846;

        // FNV-1a 64 from its standard basis (llrhihash.h): the files' checksums,
        // kept on disk, and the poses' hashes.
        constexpr std::uint64_t FNV_OFFSET = LLRHI::FNV1A_BASIS;

        void fnv(std::uint64_t& hash, const void* data, std::size_t size)
        {
            hash = LLRHI::fnv1a(hash, data, size);
        }

        void fnvFloat(std::uint64_t& hash, float value)
        {
            if (value == 0.f)
                value = 0.f; // -0 and +0 are the same pose
            fnv(hash, &value, sizeof(value));
        }

        std::uint64_t fnvText(const char* data, std::size_t size)
        {
            std::uint64_t hash = FNV_OFFSET;
            fnv(hash, data, size);
            return hash;
        }

        double length(double x, double y, double z) { return std::sqrt(x * x + y * y + z * z); }

        double distance(const Vec3& a, const Vec3& b)
        {
            return length((double)a.x - b.x, (double)a.y - b.y, (double)a.z - b.z);
        }

        double maxComponent(const Vec3& a, const Vec3& b)
        {
            return std::max({ std::fabs((double)a.x - b.x), std::fabs((double)a.y - b.y), std::fabs((double)a.z - b.z) });
        }

        // Locale-independent number text (the viewer may set a locale with a
        // decimal comma).
        std::string numberText(double value, int decimals)
        {
            char buffer[64];
            auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, decimals);
            if (ec != std::errc())
                return "0";
            std::string text(buffer, end);
            if (text.find('.') != std::string::npos)
            {
                while (!text.empty() && text.back() == '0')
                    text.pop_back();
                if (!text.empty() && text.back() == '.')
                    text.pop_back();
            }
            if (text == "-0")
                text = "0";
            return text;
        }

        // Shortest text that reads back to the same float / double.
        template <typename T> std::string exactText(T value)
        {
            char buffer[64];
            auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
            return ec == std::errc() ? std::string(buffer, end) : std::string("0");
        }

        template <typename T> bool parseNumber(const std::string& text, T& out)
        {
            if (text.empty())
                return false;
            const char* first = text.data();
            const char* last  = first + text.size();
            if (*first == '+')
                ++first;
            auto [ptr, ec] = std::from_chars(first, last, out);
            if (ec != std::errc() || ptr != last)
                return false;
            if constexpr (std::is_floating_point_v<T>)
                return std::isfinite(out);
            return true;
        }

        std::string escape(const std::string& text)
        {
            std::string out;
            out.reserve(text.size());
            for (char c : text)
            {
                if (c == '\\')
                    out += "\\\\";
                else if (c == '\n')
                    out += "\\n";
                else if (c == '\r')
                    out += "\\r";
                else
                    out += c;
            }
            return out;
        }

        std::string unescape(const std::string& text)
        {
            std::string out;
            out.reserve(text.size());
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                char c = text[i];
                if (c == '\\' && i + 1 < text.size())
                {
                    const char next = text[++i];
                    if (next == 'n')
                        c = '\n';
                    else if (next == 'r')
                        c = '\r';
                    else
                        c = next;
                }
                out += c;
            }
            return out;
        }

        std::vector<std::string> splitSpaces(const std::string& line)
        {
            std::vector<std::string> tokens;
            std::size_t              start = 0;
            while (start <= line.size())
            {
                const std::size_t end = line.find(' ', start);
                if (end == std::string::npos)
                {
                    tokens.push_back(line.substr(start));
                    break;
                }
                tokens.push_back(line.substr(start, end - start));
                start = end + 1;
            }
            return tokens;
        }

        bool normalize(Quat& q)
        {
            const double norm = std::sqrt((double)q.x * q.x + (double)q.y * q.y + (double)q.z * q.z + (double)q.w * q.w);
            if (!(norm > 0.5 && norm < 2.0))
                return false;
            q.x = (float)(q.x / norm);
            q.y = (float)(q.y / norm);
            q.z = (float)(q.z / norm);
            q.w = (float)(q.w / norm);
            return true;
        }

        // A broken or hand-edited history file: parseNumber() already refuses inf/NaN, but a finite 1e30
        // offset would still go straight into the joint on undo. Same limits as the poser's file loading
        // (fsposeranimator.cpp LOADED_*): position +-10 m, scale offset -0.95..19.
        constexpr float LOADED_POSITION_LIMIT  = 10.f;
        constexpr float LOADED_SCALE_DELTA_MIN = -0.95f;
        constexpr float LOADED_SCALE_DELTA_MAX = 19.f;

        void clampLoadedOffsets(JointState& j)
        {
            for (float* v : { &j.position.x, &j.position.y, &j.position.z })
                *v = std::clamp(*v, -LOADED_POSITION_LIMIT, LOADED_POSITION_LIMIT);
            for (float* v : { &j.scale.x, &j.scale.y, &j.scale.z })
                *v = std::clamp(*v, LOADED_SCALE_DELTA_MIN, LOADED_SCALE_DELTA_MAX);
        }

        bool equalsIgnoreCase(const std::string& a, const std::string& b)
        {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(),
                              [](char x, char y)
                              {
                                  auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; };
                                  return lower(x) == lower(y);
                              });
        }

        std::string trim(const std::string& text)
        {
            const std::size_t first = text.find_first_not_of(" \t");
            if (first == std::string::npos)
                return {};
            const std::size_t last = text.find_last_not_of(" \t");
            return text.substr(first, last - first + 1);
        }

        bool isAllDigits(const std::string& text)
        {
            return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
        }

        fs::path toPath(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

        std::string fromPath(const fs::path& path)
        {
            const std::u8string text = path.u8string();
            return std::string(text.begin(), text.end());
        }

        std::string fileName(std::uint64_t id)
        {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%c%010llu", FILE_PREFIX, static_cast<unsigned long long>(id));
            return std::string(buffer) + FILE_EXTENSION;
        }

        // Id from "h0000000012.pose"; 0 if the name is not ours.
        std::uint64_t idFromFileName(const std::string& name)
        {
            const std::size_t ext = sizeof(FILE_EXTENSION) - 1;
            if (name.size() <= ext + 1 || name[0] != FILE_PREFIX || name.compare(name.size() - ext, ext, FILE_EXTENSION) != 0)
                return 0;
            const std::string digits = name.substr(1, name.size() - ext - 1);
            std::uint64_t     id     = 0;
            if (!isAllDigits(digits) || !parseNumber(digits, id))
                return 0;
            return id;
        }

        bool endsWith(const std::string& text, const char* suffix)
        {
            const std::size_t n = std::char_traits<char>::length(suffix);
            return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
        }
    }

    // ---------------------------------------------------------------------------
    // Poses

    const JointState* Pose::find(const std::string& name) const
    {
        for (const JointState& joint : joints)
        {
            if (joint.name == name)
                return &joint;
        }
        return nullptr;
    }

    double angleDeg(const Quat& a, const Quat& b)
    {
        // conj(a) * b; the angle does not depend on the multiplication order
        // convention, and atan2 keeps small angles accurate (acos does not).
        const double ax = a.x, ay = a.y, az = a.z, aw = a.w;
        const double bx = b.x, by = b.y, bz = b.z, bw = b.w;
        const double w  = aw * bw + ax * bx + ay * by + az * bz;
        const double x  = aw * bx - bw * ax - (ay * bz - az * by);
        const double y  = aw * by - bw * ay - (az * bx - ax * bz);
        const double z  = aw * bz - bw * az - (ax * by - ay * bx);
        const double half = std::atan2(length(x, y, z), std::fabs(w));
        return 2.0 * half * HALF_TURN_DEG / HALF_TURN_RAD;
    }

    std::vector<JointDiff> diffPoses(const Pose& from, const Pose& to, const Tolerance& tolerance)
    {
        std::unordered_map<std::string, std::size_t> index;
        index.reserve(to.joints.size());
        for (std::size_t i = 0; i < to.joints.size(); ++i)
            index.emplace(to.joints[i].name, i);

        std::vector<bool>      seen(to.joints.size(), false);
        std::vector<JointDiff> diffs;
        for (const JointState& a : from.joints)
        {
            auto it = index.find(a.name);
            if (it == index.end())
            {
                JointDiff d;
                d.joint   = a.name;
                d.missing = true;
                diffs.push_back(d);
                continue;
            }
            seen[it->second]    = true;
            const JointState& b = to.joints[it->second];

            JointDiff d;
            d.joint          = a.name;
            d.angleDeg       = angleDeg(a.target, b.target);
            d.positionM      = distance(a.position, b.position);
            d.scale          = maxComponent(a.scale, b.scale);
            d.enabledChanged = a.enabled != b.enabled;
            if (d.angleDeg > tolerance.angleDeg || d.positionM > tolerance.positionM || d.scale > tolerance.scale || d.enabledChanged)
                diffs.push_back(d);
        }
        for (std::size_t i = 0; i < to.joints.size(); ++i)
        {
            if (!seen[i])
            {
                JointDiff d;
                d.joint   = to.joints[i].name;
                d.missing = true;
                diffs.push_back(d);
            }
        }

        std::sort(diffs.begin(), diffs.end(),
                  [](const JointDiff& l, const JointDiff& r)
                  {
                      if (l.angleDeg != r.angleDeg)
                          return l.angleDeg > r.angleDeg;
                      if (l.positionM != r.positionM)
                          return l.positionM > r.positionM;
                      if (l.scale != r.scale)
                          return l.scale > r.scale;
                      return l.joint < r.joint;
                  });
        return diffs;
    }

    bool samePose(const Pose& a, const Pose& b, const Tolerance& tolerance)
    {
        return diffPoses(a, b, tolerance).empty();
    }

    std::uint64_t poseHash(const Pose& pose)
    {
        std::uint64_t hash = FNV_OFFSET;
        for (const JointState& joint : pose.joints)
        {
            fnv(hash, joint.name.data(), joint.name.size());
            const char separator = 0;
            fnv(hash, &separator, 1);
            for (float v : { joint.target.x, joint.target.y, joint.target.z, joint.target.w, joint.position.x, joint.position.y,
                             joint.position.z, joint.scale.x, joint.scale.y, joint.scale.z })
                fnvFloat(hash, v);
            const unsigned char flags = (joint.enabled ? 1 : 0) | (joint.rotation ? 2 : 0);
            fnv(hash, &flags, 1);
        }
        return hash;
    }

    ChangeStats changeStats(const Pose& before, const Pose& after)
    {
        ChangeStats                  stats;
        const std::vector<JointDiff> diffs = diffPoses(before, after);
        for (const JointDiff& d : diffs)
        {
            if (d.missing)
                continue; // a different skeleton, not a change of the pose
            ++stats.joints;
            if (d.angleDeg > stats.maxDeg)
            {
                stats.maxDeg   = d.angleDeg;
                stats.maxJoint = d.joint;
            }
            if (stats.top.size() < MAX_TOP)
            {
                std::string item = d.joint + " ";
                if (d.angleDeg >= 0.5)
                    item += numberText(std::round(d.angleDeg), 0) + " deg";
                else if (d.positionM >= 0.0005)
                    item += "moved " + numberText(d.positionM * 100.0, 1) + " cm";
                else if (d.scale > Tolerance().scale)
                    item += "scaled";
                else if (d.enabledChanged)
                    item += "switched on/off";
                else
                    item += "<1 deg";
                stats.top.push_back(item);
            }
        }
        return stats;
    }

    std::string describeChange(const ChangeStats& stats)
    {
        if (stats.joints == 0)
            return "no change";
        std::string text = std::to_string(stats.joints) + (stats.joints == 1 ? " joint: " : " joints: ");
        for (std::size_t i = 0; i < stats.top.size(); ++i)
        {
            if (i)
                text += ", ";
            text += stats.top[i];
        }
        if ((std::size_t)stats.joints > stats.top.size())
            text += ", ...";
        return text;
    }

    std::string checkMarkName(const std::string& name)
    {
        if (name.empty() || name.size() > 40)
            return "a mark name must be 1-40 characters";
        if (name != trim(name))
            return "a mark name must not start or end with a space";
        if (name[0] == '#' || (name[0] >= '0' && name[0] <= '9'))
            return "a mark name must not start with '#' or a digit (those are entry ids)";
        for (char c : name)
        {
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
                return "a mark name must not contain control characters";
        }
        return {};
    }

    // ---------------------------------------------------------------------------
    // Files

    std::string serializeEntry(const Entry& entry, const std::string& avatar_id)
    {
        std::string out;
        out.reserve(128 + entry.pose.joints.size() * 160);
        out += std::string(FILE_MAGIC) + " " + std::to_string(FILE_VERSION) + "\n";
        out += "id " + std::to_string(entry.id) + "\n";
        out += "time " + exactText(entry.time) + "\n";
        out += "avatar " + escape(avatar_id) + "\n";
        out += "source " + escape(entry.source) + "\n";
        out += "summary " + escape(entry.summary) + "\n";
        out += "label " + escape(entry.label) + "\n";
        out += "changed " + std::to_string(entry.change.joints) + " " + exactText(entry.change.maxDeg) + " " +
               (entry.change.maxJoint.empty() ? std::string("-") : entry.change.maxJoint) + "\n";
        for (const std::string& item : entry.change.top)
            out += "top " + escape(item) + "\n";
        out += "joints " + std::to_string(entry.pose.joints.size()) + "\n";
        for (const JointState& j : entry.pose.joints)
        {
            out += "j " + j.name;
            for (float v : { j.target.x, j.target.y, j.target.z, j.target.w, j.own.x, j.own.y, j.own.z, j.own.w, j.position.x,
                             j.position.y, j.position.z, j.scale.x, j.scale.y, j.scale.z })
                out += " " + exactText(v);
            out += j.enabled ? " 1" : " 0";
            out += j.rotation ? " 1" : " 0";
            out += "\n";
        }
        char checksum[32];
        std::snprintf(checksum, sizeof(checksum), "%016llx", static_cast<unsigned long long>(fnvText(out.data(), out.size())));
        out += std::string("end ") + checksum + "\n";
        return out;
    }

    bool parseEntry(const std::string& text, Entry& entry, std::string& avatar_id, std::string& error)
    {
        entry     = Entry();
        avatar_id.clear();

        // The checksum line closes the file; anything cut short has none.
        std::string body = text;
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r'))
            body.pop_back();
        const std::size_t end_line = body.rfind('\n');
        if (end_line == std::string::npos || body.compare(end_line + 1, 4, "end ") != 0)
        {
            error = "no end line (cut short?)";
            return false;
        }
        const std::string checksum = trim(body.substr(end_line + 5));
        char              expected[32];
        std::snprintf(expected, sizeof(expected), "%016llx",
                      static_cast<unsigned long long>(fnvText(text.data(), end_line + 1)));
        if (checksum != expected)
        {
            error = "checksum mismatch (damaged)";
            return false;
        }

        // Lines before the end line.
        std::vector<std::string> lines;
        for (std::size_t start = 0; start < end_line;)
        {
            const std::size_t stop = std::min(text.find('\n', start), end_line);
            std::string       line = text.substr(start, stop - start);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(std::move(line));
            start = stop + 1;
        }
        if (lines.empty())
        {
            error = "empty";
            return false;
        }

        const std::vector<std::string> header = splitSpaces(lines[0]);
        int                            version = 0;
        if (header.size() != 2 || header[0] != FILE_MAGIC || !parseNumber(header[1], version))
        {
            error = "not a pose history file";
            return false;
        }
        if (version != FILE_VERSION)
        {
            error = "format version " + header[1] + " is not supported";
            return false;
        }

        bool        have_id = false, have_time = false;
        std::size_t declared_joints = 0;
        bool        have_joints     = false;
        for (std::size_t i = 1; i < lines.size(); ++i)
        {
            const std::string& line  = lines[i];
            if (line.empty())
                continue;
            const std::size_t  space = line.find(' ');
            const std::string  key   = line.substr(0, space);
            const std::string  value = space == std::string::npos ? std::string() : line.substr(space + 1);

            if (key == "id")
            {
                if (!parseNumber(value, entry.id) || entry.id == 0)
                {
                    error = "bad id";
                    return false;
                }
                have_id = true;
            }
            else if (key == "time")
            {
                if (!parseNumber(value, entry.time))
                {
                    error = "bad time";
                    return false;
                }
                have_time = true;
            }
            else if (key == "avatar")
                avatar_id = unescape(value);
            else if (key == "source")
                entry.source = unescape(value);
            else if (key == "summary")
                entry.summary = unescape(value);
            else if (key == "label")
                entry.label = unescape(value);
            else if (key == "changed")
            {
                const std::vector<std::string> t = splitSpaces(value);
                if (t.size() != 3 || !parseNumber(t[0], entry.change.joints) || !parseNumber(t[1], entry.change.maxDeg) ||
                    entry.change.joints < 0)
                {
                    error = "bad changed line";
                    return false;
                }
                entry.change.maxJoint = t[2] == "-" ? std::string() : t[2];
            }
            else if (key == "top")
            {
                if (entry.change.top.size() < MAX_TOP)
                    entry.change.top.push_back(unescape(value));
            }
            else if (key == "joints")
            {
                if (!parseNumber(value, declared_joints) || declared_joints > MAX_JOINTS)
                {
                    error = "bad joint count";
                    return false;
                }
                have_joints = true;
                entry.pose.joints.reserve(declared_joints);
            }
            else if (key == "j")
            {
                const std::vector<std::string> t = splitSpaces(value);
                if (t.size() != 17 || t[0].empty() || t[0].size() > 64)
                {
                    error = "bad joint line " + std::to_string(i + 1);
                    return false;
                }
                JointState j;
                j.name = t[0];
                float* values[] = { &j.target.x, &j.target.y, &j.target.z, &j.target.w, &j.own.x,
                                    &j.own.y,    &j.own.z,    &j.own.w,    &j.position.x, &j.position.y,
                                    &j.position.z, &j.scale.x, &j.scale.y, &j.scale.z };
                for (std::size_t k = 0; k < 14; ++k)
                {
                    if (!parseNumber(t[k + 1], *values[k]))
                    {
                        error = "bad number in joint " + j.name;
                        return false;
                    }
                }
                if ((t[15] != "0" && t[15] != "1") || (t[16] != "0" && t[16] != "1"))
                {
                    error = "bad flags in joint " + j.name;
                    return false;
                }
                clampLoadedOffsets(j);
                j.enabled  = t[15] == "1";
                j.rotation = t[16] == "1";
                if (!normalize(j.target) || !normalize(j.own))
                {
                    error = "bad rotation in joint " + j.name;
                    return false;
                }
                if (entry.pose.joints.size() >= MAX_JOINTS)
                {
                    error = "too many joints";
                    return false;
                }
                entry.pose.joints.push_back(std::move(j));
            }
            // other keys: written by a later version of this format, ignored
        }

        if (!have_id || !have_time || !have_joints)
        {
            error = "missing id, time or joints";
            return false;
        }
        if (entry.pose.joints.size() != declared_joints)
        {
            error = "joint count mismatch";
            return false;
        }
        return true;
    }

    // ---------------------------------------------------------------------------
    // History

    PoseHistory::PoseHistory(std::size_t capacity) : mCapacity(std::max<std::size_t>(capacity, 2)) {}

    std::size_t PoseHistory::undoAvailable() const
    {
        return mPosition < mEntries.size() ? mPosition : mEntries.size();
    }

    std::size_t PoseHistory::redoAvailable() const
    {
        return mPosition + 1 < mEntries.size() ? mEntries.size() - 1 - mPosition : 0;
    }

    const Entry* PoseHistory::findById(std::uint64_t id) const
    {
        for (const Entry& entry : mEntries)
        {
            if (entry.id == id)
                return &entry;
        }
        return nullptr;
    }

    Entry& PoseHistory::append(Entry entry)
    {
        mEntries.push_back(std::move(entry));
        writeEntry(mEntries.back());
        return mEntries.back();
    }

    void PoseHistory::evict(std::uint64_t keep_a, std::uint64_t keep_b)
    {
        while (mEntries.size() > mCapacity)
        {
            auto removable = [&](std::size_t i, bool spare_marks)
            {
                const Entry& e = mEntries[i];
                if (e.id == keep_a || e.id == keep_b || i == mPosition)
                    return false;
                return !(spare_marks && !e.label.empty());
            };
            std::size_t victim = mEntries.size();
            for (std::size_t i = 0; i < mEntries.size() && victim == mEntries.size(); ++i)
            {
                if (removable(i, true))
                    victim = i;
            }
            for (std::size_t i = 0; i < mEntries.size() && victim == mEntries.size(); ++i)
            {
                if (removable(i, false))
                    victim = i;
            }
            if (victim == mEntries.size())
                break;

            removeFile(mEntries[victim].id);
            mEntries.erase(mEntries.begin() + (std::ptrdiff_t)victim);
            if (victim < mPosition)
                --mPosition;
        }
        if (mPosition > mEntries.size())
            mPosition = mEntries.size();
    }

    std::uint64_t PoseHistory::record(const Pose& before, const Pose& after, const std::string& source,
                                      const std::string& summary, double now)
    {
        if (samePose(before, after))
            return 0;
        const ChangeStats stats = changeStats(before, after);

        // A saved state of exactly this pose at the end of the list (the pose an
        // undo saved and redo came back to, or a mark): the change follows it.
        if (!mEntries.empty() && mPosition + 1 >= mEntries.size())
        {
            Entry& last = mEntries.back();
            if (!last.isChange() && samePose(last.pose, before))
            {
                last.source  = source.empty() ? std::string("change") : source;
                last.summary = summary;
                last.change  = stats;
                last.time    = now;
                last.pose    = before;
                mPosition    = mEntries.size();
                writeEntry(last);
                return last.id;
            }
        }

        Entry entry;
        entry.id      = mNextId++;
        entry.time    = now;
        entry.source  = source.empty() ? std::string("change") : source;
        entry.summary = summary;
        entry.change  = stats;
        entry.pose    = before;
        const std::uint64_t id = entry.id;
        append(std::move(entry));
        mPosition = mEntries.size();
        evict(id, 0);
        return id;
    }

    void PoseHistory::recordDrift(const Pose& live, double now)
    {
        const Pose before = mEntries[mPosition].pose; // copy: record() may change the list
        record(before, live, "manual", describeChange(changeStats(before, live)), now);
        mPosition = mEntries.size();
    }

    std::size_t PoseHistory::basePosition(const Pose& live, double now, bool& pushed)
    {
        pushed = false;
        if (mPosition < mEntries.size() && !samePose(mEntries[mPosition].pose, live))
            recordDrift(live, now);
        if (mPosition < mEntries.size())
            return mPosition;

        // The live pose is newer than every entry: save it so redo can come back.
        if (!mEntries.empty() && !mEntries.back().isChange() && samePose(mEntries.back().pose, live))
        {
            mPosition = mEntries.size() - 1;
            return mPosition;
        }
        Entry tip;
        tip.id      = mNextId++;
        tip.time    = now;
        tip.summary = "the pose before undo";
        tip.pose    = live;
        append(std::move(tip));
        pushed    = true;
        mPosition = mEntries.size() - 1;
        return mPosition;
    }

    void PoseHistory::removeTipIfPushed(bool pushed)
    {
        if (!pushed || mEntries.empty())
            return;
        removeFile(mEntries.back().id);
        mEntries.pop_back();
        mPosition = mEntries.size();
    }

    PoseHistory::Move PoseHistory::undo(int steps, const Pose& live, double now)
    {
        if (steps < 1)
            throw HistoryError("steps must be 1 or more");
        if (mEntries.empty())
            throw HistoryError("The history is empty: no pose change has been recorded yet.");

        bool              pushed = false;
        const std::size_t base   = basePosition(live, now, pushed);
        if ((std::size_t)steps > base)
        {
            removeTipIfPushed(pushed);
            throw HistoryError(base == 0 ? std::string("Already at the oldest entry of the history.")
                                         : "Only " + std::to_string(base) + " step(s) back are in the history.");
        }

        const std::size_t   target    = base - (std::size_t)steps;
        const std::uint64_t target_id = mEntries[target].id;
        const std::uint64_t tip_id    = pushed ? mEntries.back().id : 0;
        mPosition                     = target;
        evict(target_id, tip_id);

        Move move;
        move.entry = findById(target_id);
        move.steps = steps;
        return move;
    }

    PoseHistory::Move PoseHistory::redo(int steps, const Pose& live, double now)
    {
        if (steps < 1)
            throw HistoryError("steps must be 1 or more");

        bool drifted = false;
        if (mPosition < mEntries.size() && !samePose(mEntries[mPosition].pose, live))
        {
            recordDrift(live, now);
            drifted = true;
        }
        if (mPosition + 1 >= mEntries.size())
        {
            throw HistoryError(drifted ? "Nothing to redo: the pose was changed after the undo (that change is now the "
                                         "newest history entry). Use history and undo to:<id> to reach other states."
                                       : "Nothing to redo.");
        }
        const std::size_t available = mEntries.size() - 1 - mPosition;
        if ((std::size_t)steps > available)
            throw HistoryError("Only " + std::to_string(available) + " step(s) forward are in the history.");

        mPosition += (std::size_t)steps;
        Move move;
        move.entry = &mEntries[mPosition];
        move.steps = -steps;
        return move;
    }

    std::size_t PoseHistory::indexOf(const std::string& raw_selector) const
    {
        const std::string selector = trim(raw_selector);
        if (selector.empty())
            throw HistoryError("give an entry id (e.g. \"12\" or \"#12\") or a mark name");

        const std::string digits = selector[0] == '#' ? selector.substr(1) : selector;
        if (isAllDigits(digits))
        {
            std::uint64_t id = 0;
            if (parseNumber(digits, id))
            {
                for (std::size_t i = 0; i < mEntries.size(); ++i)
                {
                    if (mEntries[i].id == id)
                        return i;
                }
            }
            throw HistoryError("No entry #" + digits + " in the history (the history keeps the newest " +
                               std::to_string(mCapacity) + "; see history).");
        }
        for (std::size_t i = 0; i < mEntries.size(); ++i)
        {
            if (equalsIgnoreCase(mEntries[i].label, selector))
                return i;
        }
        throw HistoryError("No mark named '" + selector + "'; see history.");
    }

    const Entry& PoseHistory::find(const std::string& selector) const { return mEntries[indexOf(selector)]; }

    const Entry& PoseHistory::stepsBack(int steps) const
    {
        if (steps < 0)
            throw HistoryError("steps must be 0 or more");
        const std::size_t base = mPosition < mEntries.size() ? mPosition : mEntries.size();
        if ((std::size_t)steps > base || (steps == 0 && base == mEntries.size()))
        {
            throw HistoryError(steps == 0 ? std::string("The current pose is not stored in the history; use to=\"now\".")
                                          : "Only " + std::to_string(base) + " step(s) back are in the history.");
        }
        return mEntries[base - (std::size_t)steps];
    }

    PoseHistory::Move PoseHistory::undoTo(const std::string& selector, const Pose& live, double now)
    {
        const std::uint64_t id = mEntries[indexOf(selector)].id;

        bool              pushed = false;
        const std::size_t base   = basePosition(live, now, pushed);
        std::size_t       index  = 0;
        while (index < mEntries.size() && mEntries[index].id != id)
            ++index;
        if (index == mEntries.size())
        {
            // Recording the unrecorded change made room by dropping this (oldest) entry.
            removeTipIfPushed(pushed);
            throw HistoryError("Entry #" + std::to_string(id) + " was just dropped from the full history; see history.");
        }

        Move move;
        if (index == base)
        {
            removeTipIfPushed(pushed); // cannot happen (the tip is new), kept for safety
            move.entry = findById(id);
            move.steps = 0;
            return move;
        }
        const std::uint64_t tip_id = pushed ? mEntries.back().id : 0;
        mPosition                  = index;
        evict(id, tip_id);
        move.entry = findById(id);
        move.steps = (int)base - (int)index;
        return move;
    }

    const Entry& PoseHistory::mark(const std::string& name, const Pose& live, double now)
    {
        const std::string problem = checkMarkName(name);
        if (!problem.empty())
            throw HistoryError(problem);

        if (mPosition < mEntries.size() && !samePose(mEntries[mPosition].pose, live))
            recordDrift(live, now);

        for (Entry& entry : mEntries)
        {
            if (!entry.label.empty() && equalsIgnoreCase(entry.label, name))
            {
                entry.label.clear();
                writeEntry(entry);
            }
        }

        if (mPosition < mEntries.size())
        {
            Entry& here = mEntries[mPosition];
            here.label  = name;
            writeEntry(here);
            return here;
        }
        if (!mEntries.empty() && !mEntries.back().isChange() && samePose(mEntries.back().pose, live))
        {
            Entry& last = mEntries.back();
            last.label  = name;
            mPosition   = mEntries.size() - 1;
            writeEntry(last);
            return last;
        }

        Entry entry;
        entry.id      = mNextId++;
        entry.time    = now;
        entry.summary = "marked";
        entry.label   = name;
        entry.pose    = live;
        const std::uint64_t id = entry.id;
        append(std::move(entry));
        mPosition = mEntries.size() - 1;
        evict(id, 0);
        return *findById(id);
    }

    // ---- files ----

    std::string PoseHistory::filePath(std::uint64_t id) const
    {
        return fromPath(toPath(mDirectory) / fileName(id));
    }

    void PoseHistory::writeEntry(const Entry& entry)
    {
        if (mDirectory.empty())
            return;

        const fs::path path = toPath(mDirectory) / fileName(entry.id);
        fs::path       temp = path;
        temp += TEMP_EXTENSION;
        {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            if (!file.is_open())
            {
                mFileError = "cannot write " + fromPath(temp);
                return;
            }
            const std::string text = serializeEntry(entry, mAvatarId);
            file.write(text.data(), (std::streamsize)text.size());
            file.close();
            if (!file)
            {
                mFileError = "cannot write " + fromPath(temp);
                std::error_code ignored;
                fs::remove(temp, ignored);
                return;
            }
        }
        std::error_code ec;
        fs::rename(temp, path, ec); // replaces an older version of the file
        if (ec)
        {
            // The older version stays (a file held open elsewhere, say): deleting it first
            // would lose the entry when the second try fails too (fix-round49).
            mFileError = "cannot write " + fromPath(path) + ": " + ec.message();
            std::error_code ignored;
            fs::remove(temp, ignored);
        }
    }

    void PoseHistory::removeFile(std::uint64_t id)
    {
        if (mDirectory.empty())
            return;
        std::error_code ec;
        fs::remove(toPath(mDirectory) / fileName(id), ec);
        if (ec)
            mFileError = "cannot remove " + filePath(id) + ": " + ec.message();
    }

    PoseHistory::LoadReport PoseHistory::attachDirectory(const std::string& directory_utf8, const std::string& avatar_id)
    {
        LoadReport report;
        mAvatarId = avatar_id;

        const fs::path  folder = toPath(directory_utf8);
        std::error_code ec;
        fs::create_directories(folder, ec);
        if (ec || !fs::is_directory(folder, ec))
        {
            mFileError = "cannot create " + directory_utf8 + (ec ? ": " + ec.message() : std::string());
            report.problems.push_back(mFileError);
            return report;
        }
        mDirectory = directory_utf8;

        if (!mEntries.empty())
        {
            for (const Entry& entry : mEntries)
                writeEntry(entry);
            report.problems.push_back("the history already had entries; files were not loaded");
            return report;
        }

        std::vector<Entry> loaded;
        std::uint64_t      max_id = 0;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        {
            std::error_code fec;
            if (!it->is_regular_file(fec))
                continue;
            const std::string name = fromPath(it->path().filename());

            if (endsWith(name, TEMP_EXTENSION))
            {
                // Left by a crash while writing; the previous version (if any) is intact.
                if (fs::remove(it->path(), fec))
                    ++report.removed;
                continue;
            }
            const std::uint64_t file_id = idFromFileName(name);
            if (file_id == 0)
                continue; // not ours
            max_id = std::max(max_id, file_id);

            const std::uintmax_t bytes = it->file_size(fec);
            if (fec || bytes > MAX_FILE_BYTES)
            {
                ++report.skipped;
                report.problems.push_back(name + ": unreadable or too large");
                continue;
            }
            std::ifstream file(it->path(), std::ios::binary);
            std::string   text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (!file.is_open() || file.bad())
            {
                ++report.skipped;
                report.problems.push_back(name + ": cannot read");
                continue;
            }

            Entry       entry;
            std::string owner, error;
            if (!parseEntry(text, entry, owner, error))
            {
                ++report.skipped;
                report.problems.push_back(name + ": " + error);
                continue;
            }
            if (owner != avatar_id)
            {
                ++report.skipped;
                report.problems.push_back(name + ": another avatar");
                continue;
            }
            if (entry.id != file_id)
            {
                ++report.skipped;
                report.problems.push_back(name + ": id does not match the file name");
                continue;
            }
            loaded.push_back(std::move(entry));
        }
        if (ec)
            report.problems.push_back("listing " + directory_utf8 + ": " + ec.message());

        std::sort(loaded.begin(), loaded.end(), [](const Entry& a, const Entry& b) { return a.id < b.id; });
        for (Entry& entry : loaded)
            mEntries.push_back(std::move(entry));
        mNextId   = max_id + 1;
        mPosition = mEntries.size();

        const std::size_t before = mEntries.size();
        evict(0, 0);
        report.removed += before - mEntries.size();
        report.loaded = mEntries.size();
        return report;
    }

    // ---------------------------------------------------------------------------

    void ChangeDebouncer::reset()
    {
        mHasBaseline = false;
        mBaseline    = 0;
        mPending     = false;
        mPendingHash = 0;
        mPendingSince = 0.0;
    }

    void ChangeDebouncer::setBaseline(std::uint64_t hash)
    {
        mHasBaseline = true;
        mBaseline    = hash;
        mPending     = false;
    }

    bool ChangeDebouncer::observe(std::uint64_t hash, double now)
    {
        if (!mHasBaseline)
        {
            setBaseline(hash);
            return false;
        }
        if (hash == mBaseline)
        {
            mPending = false; // changed and changed back
            return false;
        }
        if (!mPending || hash != mPendingHash)
        {
            mPending      = true;
            mPendingHash  = hash;
            mPendingSince = now;
            return false;
        }
        return now - mPendingSince >= mSettle;
    }
}
