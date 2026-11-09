/**
 * @file charis_translate.cpp
 * @brief charis-translate.exe, the local translation helper of Charis Viewer (CTranslate2 and SentencePiece, on the CPU).
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

// charis-translate: the local translation helper the viewer starts.
//
// Reads one JSON object per line on stdin, writes one JSON object per line on stdout (UTF-8).
//   in : {"id":7,"from":"en","to":"ja","text":"Hello!"}      translate one sentence
//        {"cmd":"load","from":"en","to":"ja"}                 load a direction now (otherwise on first use)
//        {"cmd":"quit"}                                       exit at once
//   out: {"type":"ready","version":"..."}                     once, at start (no model is loaded yet)
//        {"type":"loading","pair":"en-ja"} / {"type":"loaded","pair":"en-ja","load_ms":N}
//        {"type":"text","id":7,"pair":"en-ja","text":"...","compute_ms":N}
//        {"type":"error","id":7,"message":"..."}              id is -1 when the line had none
// A closed stdin ends the process. Models: <models>/<from>-<to>/ holds the CTranslate2 model plus
// source.spm and target.spm (Marian: SentencePiece pieces go straight into the model).
//
// Options: --models DIR  -t THREADS  --beam N  --maxlen N  --normalprio  -v

#include <ctranslate2/translator.h>
#include <sentencepiece_processor.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace
{
    const char* const VERSION = "1";

    struct Options
    {
        std::string models;
        size_t      threads = 2;
        size_t      beam = 4;
        size_t      maxLen = 128;
        bool        normalPriority = false;
        bool        verbose = false;
    };

    // ---- JSON (only what the protocol needs) ----

    struct Request
    {
        long long   id = -1;
        std::string cmd;
        std::string from;
        std::string to;
        std::string text;
        bool        hasText = false;
    };

    void appendUtf8(std::string& out, unsigned cp)
    {
        if (cp < 0x80)
        {
            out += char(cp);
        }
        else if (cp < 0x800)
        {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
        else
        {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    struct Reader
    {
        const std::string& s;
        size_t             p = 0;

        explicit Reader(const std::string& str) : s(str) {}

        void ws()
        {
            while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n'))
            {
                ++p;
            }
        }

        bool hex4(unsigned& v)
        {
            if (p + 4 > s.size())
            {
                return false;
            }
            v = 0;
            for (int i = 0; i < 4; ++i)
            {
                const char c = s[p + i];
                v <<= 4;
                if (c >= '0' && c <= '9') v |= unsigned(c - '0');
                else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
                else return false;
            }
            p += 4;
            return true;
        }

        bool string(std::string& out)
        {
            out.clear();
            if (p >= s.size() || s[p] != '"')
            {
                return false;
            }
            ++p;
            while (p < s.size())
            {
                const char c = s[p++];
                if (c == '"')
                {
                    return true;
                }
                if (c != '\\')
                {
                    out += c;
                    continue;
                }
                if (p >= s.size())
                {
                    return false;
                }
                const char e = s[p++];
                switch (e)
                {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case '/': out += '/'; break;
                case '\\': out += '\\'; break;
                case '"': out += '"'; break;
                case 'u':
                {
                    unsigned cp = 0;
                    if (!hex4(cp))
                    {
                        return false;
                    }
                    if (cp >= 0xD800 && cp < 0xDC00 && p + 1 < s.size() && s[p] == '\\' && s[p + 1] == 'u')
                    {
                        p += 2;
                        unsigned lo = 0;
                        if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF)
                        {
                            return false;
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return false;
                }
            }
            return false;
        }

        // Skips any value (numbers, true/false/null, nested objects and arrays).
        bool skipValue()
        {
            ws();
            if (p >= s.size())
            {
                return false;
            }
            if (s[p] == '"')
            {
                std::string tmp;
                return string(tmp);
            }
            if (s[p] == '{' || s[p] == '[')
            {
                const char open = s[p], close = open == '{' ? '}' : ']';
                int depth = 0;
                while (p < s.size())
                {
                    if (s[p] == '"')
                    {
                        std::string tmp;
                        if (!string(tmp))
                        {
                            return false;
                        }
                        continue;
                    }
                    if (s[p] == open) ++depth;
                    if (s[p] == close && --depth == 0)
                    {
                        ++p;
                        return true;
                    }
                    ++p;
                }
                return false;
            }
            while (p < s.size() && s[p] != ',' && s[p] != '}')
            {
                ++p;
            }
            return true;
        }
    };

    bool parseRequest(const std::string& line, Request& req, std::string& error)
    {
        Reader r(line);
        r.ws();
        if (r.p >= line.size() || line[r.p] != '{')
        {
            error = "not a JSON object";
            return false;
        }
        ++r.p;
        for (;;)
        {
            r.ws();
            if (r.p < line.size() && line[r.p] == '}')
            {
                return true;
            }
            std::string key;
            if (!r.string(key))
            {
                error = "bad key";
                return false;
            }
            r.ws();
            if (r.p >= line.size() || line[r.p] != ':')
            {
                error = "missing ':'";
                return false;
            }
            ++r.p;
            r.ws();
            bool ok = true;
            if (key == "id")
            {
                char* end = nullptr;
                req.id = std::strtoll(line.c_str() + r.p, &end, 10);
                r.p = size_t(end - line.c_str());
            }
            else if (key == "cmd" || key == "from" || key == "to" || key == "text")
            {
                std::string v;
                ok = r.string(v);
                if (key == "cmd") req.cmd = v;
                else if (key == "from") req.from = v;
                else if (key == "to") req.to = v;
                else { req.text = v; req.hasText = true; }
            }
            else
            {
                ok = r.skipValue();
            }
            if (!ok)
            {
                error = "bad value for \"" + key + "\"";
                return false;
            }
            r.ws();
            if (r.p < line.size() && line[r.p] == ',')
            {
                ++r.p;
            }
        }
    }

    std::string quote(const std::string& s)
    {
        std::string out = "\"";
        for (const unsigned char c : s)
        {
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else
                {
                    out += char(c);
                }
            }
        }
        out += '"';
        return out;
    }

    void emit(const std::string& line)
    {
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }

    void emitError(long long id, const std::string& message)
    {
        emit("{\"type\":\"error\",\"id\":" + std::to_string(id) + ",\"message\":" + quote(message) + "}");
    }

    // ---- translation ----

    struct Pair
    {
        std::shared_ptr<ctranslate2::Translator>  translator;   // shared: one multilingual model serves several pairs
        sentencepiece::SentencePieceProcessor     source;
        sentencepiece::SentencePieceProcessor     target;
        std::string                               prefix;   // a target-language token the model wants first (">>por<<"), from charis.json
        // A multilingual model (M2M100): the source sentence starts with its language token ("__ja__") and the answer
        // is asked to start with the target's ("__ko__").
        std::string                               sourceToken;
        std::string                               targetToken;
    };

    // The pairs one multilingual model (models/m2m) answers when there is no model of their own (Korean).
    bool multilingualPair(const std::string& pair)
    {
        return pair == "ja-ko" || pair == "ko-ja" || pair == "en-ko" || pair == "ko-en";
    }

    // charis.json beside the model: {"prefix": ">>por<<"}. Only that one string is read; a missing file means no prefix.
    std::string readPrefix(const std::string& dir)
    {
        std::FILE* f = std::fopen((dir + "/charis.json").c_str(), "rb");
        if (!f)
        {
            return std::string();
        }
        std::string text;
        char buf[512];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < 4096)
        {
            text.append(buf, n);
        }
        std::fclose(f);
        const size_t key = text.find("\"prefix\"");
        if (key == std::string::npos)
        {
            return std::string();
        }
        Reader r(text);
        r.p = text.find(':', key);
        if (r.p == std::string::npos)
        {
            return std::string();
        }
        ++r.p;
        r.ws();
        std::string value;
        if (!r.string(value) || value.size() > 32)
        {
            return std::string();
        }
        return value;
    }

    long long msSince(const std::chrono::steady_clock::time_point& t0)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    }

    // ---- cutting a text into pieces the model can do ----
    //
    // The Marian models were trained on single sentences. Given a long run with no punctuation (what the recognizer
    // writes for lyrics and quick speech) they translate the first part and drop the rest. So the text is cut into
    // pieces first, each piece is translated, and the answers are put together.

    constexpr size_t MAX_WORDS = 14;     // words of a piece for a language that spaces its words
    constexpr size_t MAX_CHARS = 36;     // characters (not bytes) of a piece for one that does not
    constexpr size_t MIN_WORDS_BREAK = 4;   // a softer break (comma, a capital letter) is taken only after this many words

    bool at(const std::string& s, size_t i, const char* seq)
    {
        return s.compare(i, std::strlen(seq), seq) == 0;
    }

    size_t charLen(const std::string& s, size_t i)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        return i + n <= s.size() ? n : 1;
    }

    std::string trim(const std::string& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
        {
            ++a;
        }
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
        {
            --b;
        }
        return s.substr(a, b - a);
    }

    // Has a letter or a digit or any non-ASCII character: something to translate (not just "-" or "...").
    bool hasContent(const std::string& s)
    {
        for (const unsigned char c : s)
        {
            if (c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
            {
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> words(const std::string& s)
    {
        std::vector<std::string> out;
        std::string cur;
        for (const char c : s)
        {
            if (c == ' ')
            {
                if (!cur.empty())
                {
                    out.push_back(cur);
                    cur.clear();
                }
            }
            else
            {
                cur += c;
            }
        }
        if (!cur.empty())
        {
            out.push_back(cur);
        }
        return out;
    }

    std::string join(const std::vector<std::string>& w, size_t from, size_t to)
    {
        std::string out;
        for (size_t i = from; i < to; ++i)
        {
            out += (out.empty() ? "" : " ") + w[i];
        }
        return out;
    }

    // Evenly: n words into ceil(n / max) pieces.
    void cutEvenly(const std::vector<std::string>& w, size_t max, std::vector<std::string>& out)
    {
        const size_t parts = (w.size() + max - 1) / max;
        for (size_t k = 0; k < parts; ++k)
        {
            out.push_back(join(w, w.size() * k / parts, w.size() * (k + 1) / parts));
        }
    }

    bool isSubjectI(const std::string& w)
    {
        return w == "I" || w.rfind("I'", 0) == 0 || w.rfind("I\xE2\x80\x99", 0) == 0;
    }

    // A piece that is long for a language with spaces between words: break after a comma, then before a capital
    // letter in the middle (the recognizer leaves out the full stop there), and cut what is still long evenly.
    void softSplitWords(const std::string& piece, std::vector<std::string>& out)
    {
        const std::vector<std::string> w = words(piece);
        if (w.size() <= MAX_WORDS)
        {
            out.push_back(piece);
            return;
        }
        std::vector<std::string> rough;
        size_t start = 0;
        for (size_t i = 0; i < w.size(); ++i)
        {
            const size_t have = i - start;
            const bool after_comma = i > 0 && !w[i - 1].empty() && w[i - 1].back() == ',' && have >= MIN_WORDS_BREAK;
            const bool capital = have >= MIN_WORDS_BREAK && !w[i].empty() && w[i][0] >= 'A' && w[i][0] <= 'Z' && !isSubjectI(w[i]);
            if (i > start && (after_comma || capital))
            {
                rough.push_back(join(w, start, i));
                start = i;
            }
        }
        rough.push_back(join(w, start, w.size()));
        for (const std::string& r : rough)
        {
            const std::vector<std::string> rw = words(r);
            if (rw.size() > MAX_WORDS)
            {
                cutEvenly(rw, MAX_WORDS, out);
            }
            else
            {
                out.push_back(r);
            }
        }
    }

    // The same for a language written without spaces (Japanese): after a comma, then evenly by characters.
    void softSplitChars(const std::string& piece, std::vector<std::string>& out)
    {
        std::vector<size_t> starts;   // byte offset of each character
        for (size_t i = 0; i < piece.size(); i += charLen(piece, i))
        {
            starts.push_back(i);
        }
        if (starts.size() <= MAX_CHARS)
        {
            out.push_back(piece);
            return;
        }
        std::vector<std::string> rough;
        size_t from = 0;   // index into starts
        for (size_t k = 0; k < starts.size(); ++k)
        {
            const bool comma = at(piece, starts[k], "\xE3\x80\x81") || at(piece, starts[k], "\xEF\xBC\x8C") || piece[starts[k]] == ',';
            if (comma && k + 1 - from >= 8 && k + 1 < starts.size())
            {
                rough.push_back(piece.substr(starts[from], starts[k + 1] - starts[from]));
                from = k + 1;
            }
        }
        rough.push_back(piece.substr(starts[from]));
        for (const std::string& r : rough)
        {
            size_t n = 0;
            for (size_t i = 0; i < r.size(); i += charLen(r, i))
            {
                ++n;
            }
            if (n <= MAX_CHARS)
            {
                out.push_back(r);
                continue;
            }
            std::vector<size_t> offsets;   // byte offset of each character, and the end
            for (size_t i = 0; i < r.size(); i += charLen(r, i))
            {
                offsets.push_back(i);
            }
            offsets.push_back(r.size());
            const size_t parts = (n + MAX_CHARS - 1) / MAX_CHARS;
            for (size_t k = 0; k < parts; ++k)
            {
                const size_t a = offsets[n * k / parts], b = offsets[n * (k + 1) / parts];
                out.push_back(r.substr(a, b - a));
            }
        }
    }

    struct Pieces
    {
        std::vector<std::string> list;
        bool note = false;   // the text had a music note: the answer gets them back at both ends
    };

    // spaced: the source language puts spaces between words. Annotations in brackets ("(upbeat music)") are dropped,
    // a music note is a hard break, so are full stops and the like (an ASCII one only when a space or the end follows,
    // so "3.5" stays whole).
    Pieces splitForTranslation(const std::string& text, bool spaced)
    {
        Pieces result;
        std::string cleaned;
        for (size_t i = 0; i < text.size();)
        {
            const char open = text[i];
            const char* close = open == '(' ? ")" : open == '[' ? "]" : at(text, i, "\xEF\xBC\x88") ? "\xEF\xBC\x89" : nullptr;
            if (close || at(text, i, "\xEF\xBC\x88"))
            {
                if (!close)
                {
                    close = "\xEF\xBC\x89";
                }
                const size_t end = text.find(close, i + 1);
                if (end != std::string::npos && end - i < 80)
                {
                    cleaned += '\n';
                    i = end + std::strlen(close);
                    continue;
                }
            }
            cleaned += text[i++];
        }
        std::vector<std::string> hard;
        std::string cur;
        auto flush = [&] {
            const std::string t = trim(cur);
            cur.clear();
            if (hasContent(t))
            {
                hard.push_back(t);
            }
        };
        for (size_t i = 0; i < cleaned.size();)
        {
            if (at(cleaned, i, "\xE2\x99\xAA"))   // the music note
            {
                result.note = true;
                flush();
                i += 3;
                continue;
            }
            const char c = cleaned[i];
            if (c == '\n')
            {
                flush();
                ++i;
                continue;
            }
            const size_t n = charLen(cleaned, i);
            cur.append(cleaned, i, n);
            const bool ascii_stop = (c == '.' || c == '!' || c == '?' || c == ';') && (i + 1 >= cleaned.size() || cleaned[i + 1] == ' ');
            const bool wide_stop = at(cleaned, i, "\xE3\x80\x82") || at(cleaned, i, "\xEF\xBC\x81") || at(cleaned, i, "\xEF\xBC\x9F") ||
                                   at(cleaned, i, "\xEF\xBC\x9B");
            i += n;
            if (ascii_stop || wide_stop)
            {
                flush();
            }
        }
        flush();
        for (const std::string& h : hard)
        {
            if (spaced)
            {
                softSplitWords(h, result.list);
            }
            else
            {
                softSplitChars(h, result.list);
            }
        }
        return result;
    }

    // The Chinese models write ASCII punctuation. In Chinese text that is the wide kind: , ? ! ; : always, and a full stop
    // after a Chinese character (a decimal point or a dot in a Latin word stays).
    std::string fullwidthPunctuation(const std::string& in)
    {
        std::string out;
        for (size_t i = 0; i < in.size(); ++i)
        {
            const char c = in[i];
            const bool after_wide = i > 0 && static_cast<unsigned char>(in[i - 1]) >= 0x80;
            const bool dot = c == '.' && after_wide;
            const char* wide = c == ',' ? "\xEF\xBC\x8C" : c == '?' ? "\xEF\xBC\x9F" : c == '!' ? "\xEF\xBC\x81" : c == ';' ? "\xEF\xBC\x9B" :
                               c == ':' ? "\xEF\xBC\x9A" : dot ? "\xE3\x80\x82" : nullptr;
            if (wide)
            {
                out += wide;
                if (i + 1 < in.size() && in[i + 1] == ' ' && c != '.')
                {
                    ++i;   // the space that followed the ASCII mark is not wanted after the wide one
                }
            }
            else
            {
                out += c;
            }
        }
        return out;
    }

    class Engine
    {
    public:
        explicit Engine(const Options& o) : mOpt(o) {}

        // Loads a direction if it is not loaded yet. false (and an error text) when it cannot.
        bool load(const std::string& pair, std::string& error)
        {
            if (mPairs.count(pair))
            {
                return true;
            }
            if (mFailed.count(pair))
            {
                error = mFailed[pair];
                return false;
            }
            emit("{\"type\":\"loading\",\"pair\":" + quote(pair) + "}");
            const auto t0 = std::chrono::steady_clock::now();
            std::string dir = mOpt.models + "/" + pair;
            bool multilingual = false;
            try
            {
                auto exists = [](const std::string& path) {
                    if (std::FILE* f = std::fopen(path.c_str(), "rb"))
                    {
                        std::fclose(f);
                        return true;
                    }
                    return false;
                };
                if (!exists(dir + "/source.spm") && multilingualPair(pair) && exists(mOpt.models + "/m2m/source.spm"))
                {
                    dir = mOpt.models + "/m2m";
                    multilingual = true;
                }
                if (!exists(dir + "/source.spm"))
                {
                    throw std::runtime_error("no model for this language pair");
                }
                std::unique_ptr<Pair> p(new Pair);
                p->prefix = readPrefix(dir);
                if (multilingual)
                {
                    p->sourceToken = "__" + pair.substr(0, 2) + "__";
                    p->targetToken = "__" + pair.substr(3, 2) + "__";
                }
                if (!p->source.Load(dir + "/source.spm").ok() || !p->target.Load(dir + "/target.spm").ok())
                {
                    throw std::runtime_error("cannot read source.spm / target.spm in " + dir);
                }
                ctranslate2::ReplicaPoolConfig cfg;
                cfg.num_threads_per_replica = mOpt.threads;
                std::shared_ptr<ctranslate2::Translator>& shared = mTranslators[dir];   // loaded once per model folder
                if (!shared)
                {
                    shared.reset(new ctranslate2::Translator(
                        dir, ctranslate2::Device::CPU, ctranslate2::ComputeType::DEFAULT, std::vector<int>{ 0 }, false, cfg));
                }
                p->translator = shared;
                mPairs[pair] = std::move(p);
            }
            catch (const std::exception& e)
            {
                error = pair + ": " + e.what();
                mFailed[pair] = error;
                return false;
            }
            emit("{\"type\":\"loaded\",\"pair\":" + quote(pair) + ",\"load_ms\":" + std::to_string(msSince(t0)) + "}");
            return true;
        }

        bool translate(const std::string& pair, const std::string& text, std::string& out, std::string& error)
        {
            if (!load(pair, error))
            {
                return false;
            }
            Pair& p = *mPairs[pair];
            try
            {
                const bool spaced = pair.compare(0, 2, "ja") != 0;                   // the source language
                const bool target_spaced = pair.size() < 5 || (pair.compare(pair.size() - 2, 2, "ja") != 0 && pair.compare(pair.size() - 2, 2, "zh") != 0);   // ja and zh run on without spaces
                const Pieces cut = splitForTranslation(text, spaced);
                std::vector<std::vector<std::string>> batch;
                for (const std::string& part : cut.list)
                {
                    std::vector<std::string> pieces;
                    if (!p.source.Encode(part, &pieces).ok())
                    {
                        error = "tokenization failed";
                        return false;
                    }
                    if (pieces.size() > MAX_SOURCE_PIECES)
                    {
                        pieces.resize(MAX_SOURCE_PIECES);
                    }
                    if (!p.prefix.empty())
                    {
                        pieces.insert(pieces.begin(), p.prefix);
                    }
                    if (!p.sourceToken.empty())
                    {
                        pieces.insert(pieces.begin(), p.sourceToken);
                    }
                    pieces.push_back("</s>");
                    batch.push_back(std::move(pieces));
                }
                out.clear();
                if (batch.empty())
                {
                    return true;
                }
                ctranslate2::TranslationOptions to;
                to.beam_size = mOpt.beam;
                // A piece is short, so its answer is too: a cap tied to the longest piece stops a loop early.
                size_t longest = 0;
                for (const auto& b : batch)
                {
                    longest = std::max(longest, b.size());
                }
                to.max_decoding_length = std::min(mOpt.maxLen, longest * 3 + 8);
                to.no_repeat_ngram_size = 0;
                to.length_penalty = 2.0f;   // a higher value lets longer answers win: fewer cut-off translations
                std::vector<std::vector<std::string>> target_prefix;   // the multilingual model is told what to write
                if (!p.targetToken.empty())
                {
                    target_prefix.assign(batch.size(), std::vector<std::string>{ p.targetToken });
                }
                const auto results = target_prefix.empty() ? p.translator->translate_batch(batch, to)
                                                           : p.translator->translate_batch(batch, target_prefix, to);
                for (const auto& r : results)
                {
                    std::vector<std::string> hyp;
                    for (const std::string& t : r.output())
                    {
                        // an unknown piece (an emoji, say) would print as " ⁇ "; the target-language token is not text
                        if (t != "</s>" && t != "<unk>" && !(!p.targetToken.empty() && t == p.targetToken))
                        {
                            hyp.push_back(t);
                        }
                    }
                    std::string one;
                    if (!p.target.Decode(hyp, &one).ok())
                    {
                        error = "detokenization failed";
                        return false;
                    }
                    one = trim(one);
                    if (!one.empty())
                    {
                        // a piece that does not end in a stop (the source had none) is set apart by a space
                        const bool stopped = out.empty() || out.back() == '.' || out.back() == '!' || out.back() == '?' || out.back() == ',' ||
                                             (out.size() >= 3 && (out.compare(out.size() - 3, 3, "ã") == 0 || out.compare(out.size() - 3, 3, "ã") == 0 ||
                                                                  out.compare(out.size() - 3, 3, "ï¼") == 0 || out.compare(out.size() - 3, 3, "ï¼") == 0));
                        out += (out.empty() || (!target_spaced && stopped) ? "" : " ") + one;
                    }
                }
                if (!target_spaced && pair.compare(pair.size() - 2, 2, "zh") == 0)
                {
                    out = fullwidthPunctuation(out);
                }
                if (cut.note && !out.empty())
                {
                    out = "\xE2\x99\xAA " + out + " \xE2\x99\xAA";
                }
            }
            catch (const std::exception& e)
            {
                error = e.what();
                return false;
            }
            return true;
        }

    private:
        static constexpr size_t MAX_SOURCE_PIECES = 400;   // Marian positions: 512

        Options                          mOpt;
        std::map<std::string, std::unique_ptr<Pair>> mPairs;
        std::map<std::string, std::shared_ptr<ctranslate2::Translator>> mTranslators;
        std::map<std::string, std::string>           mFailed;
    };

    bool parseArgs(int argc, char** argv, Options& o)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto need = [&](const char* name) -> const char* {
                if (i + 1 >= argc)
                {
                    std::fprintf(stderr, "%s needs a value\n", name);
                    return nullptr;
                }
                return argv[++i];
            };
            const char* v = nullptr;
            if (a == "--models") { if (!(v = need("--models"))) return false; o.models = v; }
            else if (a == "-t") { if (!(v = need("-t"))) return false; o.threads = size_t(std::max(1, std::atoi(v))); }
            else if (a == "--beam") { if (!(v = need("--beam"))) return false; o.beam = size_t(std::max(1, std::atoi(v))); }
            else if (a == "--maxlen") { if (!(v = need("--maxlen"))) return false; o.maxLen = size_t(std::max(8, std::atoi(v))); }
            else if (a == "--normalprio") { o.normalPriority = true; }
            else if (a == "-v") { o.verbose = true; }
            else
            {
                std::fprintf(stderr, "unknown option %s\n", a.c_str());
                return false;
            }
        }
        if (o.models.empty())
        {
            std::fprintf(stderr, "usage: charis-translate --models DIR [-t N] [--beam N] [--maxlen N] [--normalprio] [-v]\n");
            return false;
        }
        return true;
    }
}

int main(int argc, char** argv)
{
    Options opt;
    if (!parseArgs(argc, argv, opt))
    {
        return 2;
    }
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    if (!opt.normalPriority)
    {
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    }
#endif
    Engine engine(opt);
    emit(std::string("{\"type\":\"ready\",\"version\":\"") + VERSION + "\"}");

    std::string line;
    int c;
    for (;;)
    {
        line.clear();
        while ((c = std::fgetc(stdin)) != EOF && c != '\n')
        {
            line += char(c);
        }
        if (c == EOF && line.empty())
        {
            break;   // stdin closed: nothing is queued, so there is nothing to finish
        }
        if (line.empty() || line == "\r")
        {
            continue;
        }
        Request req;
        std::string error;
        if (!parseRequest(line, req, error))
        {
            emitError(req.id, error);
            continue;
        }
        if (req.cmd == "quit")
        {
            break;
        }
        // from and to become a folder name (<models>/<from>-<to>): two or three small letters, nothing else (audit C, I3)
        auto validLanguage = [](const std::string& s) {
            if (s.size() < 2 || s.size() > 3)
            {
                return false;
            }
            for (const char c : s)
            {
                if (c < 'a' || c > 'z')
                {
                    return false;
                }
            }
            return true;
        };
        if (!validLanguage(req.from) || !validLanguage(req.to))
        {
            emitError(req.id, "bad language code");
            continue;
        }
        const std::string pair = req.from + "-" + req.to;
        if (req.cmd == "split")   // for the tests: what the text is cut into
        {
            const Pieces cut = splitForTranslation(req.text, req.from != "ja");
            std::string list;
            for (const std::string& part : cut.list)
            {
                list += (list.empty() ? "" : ",") + quote(part);
            }
            emit("{\"type\":\"split\",\"note\":" + std::string(cut.note ? "true" : "false") + ",\"pieces\":[" + list + "]}");
            continue;
        }
        if (req.cmd == "load")
        {
            if (!engine.load(pair, error))
            {
                emitError(req.id, error);
            }
            continue;
        }
        if (!req.hasText)
        {
            emitError(req.id, "no text");
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        std::string out;
        if (!engine.translate(pair, req.text, out, error))
        {
            emitError(req.id, error);
            continue;
        }
        emit("{\"type\":\"text\",\"id\":" + std::to_string(req.id) + ",\"pair\":" + quote(pair) +
             ",\"text\":" + quote(out) + ",\"compute_ms\":" + std::to_string(msSince(t0)) + "}");
        if (opt.verbose)
        {
            std::fprintf(stderr, "%s %lldms\n", pair.c_str(), msSince(t0));
        }
    }
    // Tearing a loaded Translator down (worker threads, thread pools) took minutes; every answer is already
    // flushed and nothing else needs closing, so leave without running destructors.
    std::fflush(stdout);
    std::_Exit(0);
}
