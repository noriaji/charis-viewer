/**
 * @file translatearchive.h
 * @brief Subtitles (plus/subtitle, research/217): the archive a local translation model comes in.
 *
 * One archive per direction ("en-ja.ctm", "ja-en.ctm"), made by tools/translate-helper/pack.py:
 *   "CTM1", u32 count, then per file: u16 name length, name, u64 size; then the contents one after another.
 * Only the five files charis-translate.exe reads are accepted (and charis.json, the target-language token of a model
 * that needs one, as a sixth), so a bad archive cannot write anywhere else.
 * Standard C++ only (it is tested on its own in subtitle_test.cpp).
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

#ifndef PLUS_SUBTITLE_TRANSLATEARCHIVE_H
#define PLUS_SUBTITLE_TRANSLATEARCHIVE_H

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace plus
{
    namespace subtitle
    {
        // A path from UTF-8 text (the viewer's folders are UTF-8; a user name can be Japanese).
        inline std::filesystem::path pathOf(const std::string& utf8)
        {
#if defined(__cpp_char8_t)
            return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
#else
            return std::filesystem::u8path(utf8);
#endif
        }

        // fopen that takes the wide path on Windows.
        inline std::FILE* openFile(const std::filesystem::path& path, bool write)
        {
#ifdef _WIN32
            return _wfopen(path.c_str(), write ? L"wb" : L"rb");
#else
            return std::fopen(path.c_str(), write ? "wb" : "rb");
#endif
        }

        // The files of one direction, in the order the archive holds them.
        inline const std::vector<std::string>& translateModelFiles()
        {
            static const std::vector<std::string> sFiles = { "config.json", "model.bin", "shared_vocabulary.json", "source.spm", "target.spm" };
            return sFiles;
        }

        // True when every file of the direction is in the folder.
        inline bool translateModelInstalled(const std::string& folder)
        {
            std::error_code ec;
            for (const std::string& name : translateModelFiles())
            {
                if (!std::filesystem::is_regular_file(pathOf(folder) / name, ec))
                {
                    return false;
                }
            }
            return true;
        }

        namespace detail
        {
            inline bool readExact(std::FILE* f, void* p, size_t n)
            {
                return std::fread(p, 1, n, f) == n;
            }
        }

        // Unpacks the archive into the folder: the files are written into a "<folder>.tmp" folder first and the folder
        // takes its name only when all of them are in, so a stopped unpack never leaves a half model that looks installed.
        // Returns false and says why (English, for the log).
        // stop: when it becomes true the unpack ends at once (between two pieces of a file), nothing is left behind, and false is
        // returned (error "cancelled"): cancelling a fetch does not wait for 500 MB to be written.
        inline bool unpackTranslateArchive(const std::string& archive, const std::string& folder, std::string& error,
                                           const std::atomic<bool>* stop = nullptr)
        {
            namespace fs = std::filesystem;
            const uint64_t MAX_FILE = 512ull << 20;
            std::FILE* in = openFile(pathOf(archive), false);
            if (!in)
            {
                error = "cannot open the archive";
                return false;
            }
            struct Closer { std::FILE* f; ~Closer() { std::fclose(f); } } closer{ in };

            char magic[4];
            uint32_t count = 0;
            if (!detail::readExact(in, magic, 4) || std::string(magic, 4) != "CTM1" || !detail::readExact(in, &count, 4))
            {
                error = "not a translation model archive";
                return false;
            }
            // the five files of the model, and charis.json after them for a model that wants a target-language token
            std::vector<std::string> allowed = translateModelFiles();
            if (count == allowed.size() + 1)
            {
                allowed.push_back("charis.json");
            }
            if (count != allowed.size())
            {
                error = "unexpected number of files in the archive";
                return false;
            }
            std::vector<uint64_t> sizes;
            for (uint32_t i = 0; i < count; ++i)
            {
                uint16_t len = 0;
                if (!detail::readExact(in, &len, 2) || len == 0 || len > 64)
                {
                    error = "bad file name in the archive";
                    return false;
                }
                std::string name(len, '\0');
                uint64_t size = 0;
                if (!detail::readExact(in, &name[0], len) || !detail::readExact(in, &size, 8))
                {
                    error = "the archive ends inside its list";
                    return false;
                }
                if (name != allowed[i] || size > MAX_FILE || (name == "charis.json" && size > 4096))
                {
                    error = "unexpected file in the archive: " + name;
                    return false;
                }
                sizes.push_back(size);
            }

            const fs::path dest = pathOf(folder);
            const fs::path tmp = pathOf(folder + ".tmp");
            std::error_code ec;
            fs::remove_all(tmp, ec);
            fs::create_directories(tmp, ec);
            if (ec)
            {
                error = "cannot make " + folder + ".tmp";
                return false;
            }
            std::vector<char> buf(1 << 20);
            for (size_t i = 0; i < allowed.size(); ++i)
            {
                std::FILE* out = openFile(tmp / allowed[i], true);
                if (!out)
                {
                    error = "cannot write " + allowed[i];
                    fs::remove_all(tmp, ec);
                    return false;
                }
                uint64_t left = sizes[i];
                bool ok = true;
                while (left > 0 && ok)
                {
                    if (stop && stop->load())
                    {
                        std::fclose(out);
                        error = "cancelled";
                        fs::remove_all(tmp, ec);
                        return false;
                    }
                    const size_t n = size_t(left < buf.size() ? left : buf.size());
                    ok = detail::readExact(in, buf.data(), n) && std::fwrite(buf.data(), 1, n, out) == n;
                    left -= n;
                }
                ok = (std::fclose(out) == 0) && ok;
                if (!ok)
                {
                    error = "the archive ends inside " + allowed[i] + " (or the disk is full)";
                    fs::remove_all(tmp, ec);
                    return false;
                }
            }
            fs::remove_all(dest, ec);
            fs::rename(tmp, dest, ec);
            if (ec)
            {
                error = "cannot name the folder: " + ec.message();
                fs::remove_all(tmp, ec);
                return false;
            }
            return true;
        }
    }
}

#endif // PLUS_SUBTITLE_TRANSLATEARCHIVE_H
