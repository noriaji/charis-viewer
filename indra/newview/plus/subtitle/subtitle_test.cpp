// Standalone test of subtitlering.h (not part of the viewer build). Build and run:
//   cl /nologo /EHsc /std:c++17 /utf-8 subtitle_test.cpp && subtitle_test.exe
#include "subtitledownload.h"
#include "subtitledialect.h"
#include "subtitlehash.h"
#include "subtitlering.h"
#include "subtitlespeaker.h"
#include "subtitletranslate.h"
#include "translatearchive.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <string>
#include <cstdio>
#include <thread>

using namespace plus::subtitle;

static int sFailed = 0;
#define CHECK(c)                                                              \
    do                                                                        \
    {                                                                         \
        if (!(c))                                                             \
        {                                                                     \
            printf("FAIL line %d: %s\n", __LINE__, #c);                       \
            ++sFailed;                                                        \
        }                                                                     \
    } while (0)

static void ring_basic()
{
    Ring r(8);
    int16_t a[6] = { 1, 2, 3, 4, 5, 6 };
    CHECK(r.write(a, 6) == 6);
    int16_t b[8];
    CHECK(r.read(b, 4) == 4 && b[0] == 1 && b[3] == 4);
    CHECK(r.write(a, 6) == 6);  // wraps around
    CHECK(r.read(b, 8) == 8 && b[0] == 5 && b[1] == 6 && b[2] == 1 && b[7] == 6);
    CHECK(r.read(b, 8) == 0);
    int16_t big[20] = {};
    CHECK(r.write(big, 20) == 8);  // capacity is 8: the rest is refused and counted
    CHECK(r.refused() == 12);
    r.clear();
    CHECK(r.read(b, 8) == 0);
}

static void ring_threads()
{
    Ring r(1 << 12);
    const int total = 200000;
    std::thread w([&] {
        int16_t v = 0;
        int sent = 0;
        while (sent < total)
        {
            int16_t chunk[160];
            for (auto& s : chunk)
            {
                s = v++;
            }
            size_t kept = r.write(chunk, 160);
            if (kept < 160)
            {
                v = int16_t(v - int16_t(160 - kept));  // refused: send those again
            }
            sent += int(kept);
        }
    });
    int16_t expect = 0;
    int got = 0;
    bool ok = true;
    while (got < total)
    {
        int16_t buf[100];
        size_t n = r.read(buf, 100);
        for (size_t i = 0; i < n; ++i)
        {
            ok = ok && buf[i] == expect++;
        }
        got += int(n);
    }
    w.join();
    CHECK(ok);
}

// A 440 Hz tone in, the same tone out at 16 kHz mono, whatever the buffer size.
static void converter_tone(int rate, int channels, size_t buffer_frames)
{
    const int total = rate;  // one second
    std::vector<int16_t> in(size_t(total) * channels);
    for (int i = 0; i < total; ++i)
    {
        for (int c = 0; c < channels; ++c)
        {
            in[size_t(i) * channels + c] = int16_t(8000.0 * std::sin(2.0 * 3.14159265358979 * 440.0 * i / rate));
        }
    }
    Converter conv;
    std::vector<int16_t> out;
    int16_t tmp[4096];
    for (size_t pos = 0; pos < size_t(total); pos += buffer_frames)
    {
        size_t n = buffer_frames < size_t(total) - pos ? buffer_frames : size_t(total) - pos;
        size_t got = conv.convert(&in[pos * channels], n, channels, rate, tmp, 4096);
        out.insert(out.end(), tmp, tmp + got);
    }
    CHECK(out.size() >= size_t(RATE) - 4 && out.size() <= size_t(RATE) + 4);
    double err = 0;
    size_t m = out.size() < size_t(RATE) ? out.size() : size_t(RATE);
    for (size_t i = 2; i + 2 < m; ++i)
    {
        double want = 8000.0 * std::sin(2.0 * 3.14159265358979 * 440.0 * double(i) / RATE);
        double d = std::fabs(want - double(out[i]));
        if (d > err)
        {
            err = d;
        }
    }
    printf("  %6d Hz %d ch, buffers of %4zu frames: %zu samples out, largest error %.0f\n", rate, channels, buffer_frames,
           out.size(), err);
    CHECK(err < 600);  // linear interpolation of a 440 Hz tone at 16 kHz is within a few percent of 8000
}

static void converter_edges()
{
    Converter conv;
    int16_t out[64];
    CHECK(conv.convert(nullptr, 0, 2, 48000, out, 64) == 0);
    int16_t one[2] = { 100, 100 };
    CHECK(conv.convert(one, 1, 2, 48000, out, 64) <= 1);
    CHECK(conv.convert(one, 1, 0, 48000, out, 64) == 0);
    CHECK(conv.convert(one, 1, 2, 0, out, 64) == 0);
    // loud input never wraps around
    int16_t loud[8] = { 32767, 32767, 32767, 32767, -32768, -32768, -32768, -32768 };
    size_t n = conv.convert(loud, 4, 2, 16000, out, 64);
    for (size_t i = 0; i < n; ++i)
    {
        CHECK(out[i] == 32767 || out[i] == -32768 || (out[i] > -32768 && out[i] < 32767));
    }
}


// ---- who spoke ----------------------------------------------------------------------------------

typedef SpeakerTracker::Voice V;

// One entry every 0.1 s from `from` to `to`; `who` speaks (others are listed but silent).
static void feed(SpeakerTracker& tr, double from, double to, const std::vector<std::string>& all, const std::vector<std::string>& who,
                 float power = 0.5f)
{
    for (double t = from; t < to - 1e-9; t += 0.1)
    {
        std::vector<V> v;
        for (const std::string& id : all)
        {
            bool sp = std::find(who.begin(), who.end(), id) != who.end();
            v.push_back({ id, sp, sp ? power : 0.0f });
        }
        tr.add(t, v);
    }
}

static void speaker_basic()
{
    SpeakerTracker tr;
    const std::vector<std::string> all = { "alice", "bob" };
    feed(tr, 0.0, 3.0, all, { "alice" });
    feed(tr, 3.0, 6.0, all, {});
    feed(tr, 6.0, 9.0, all, { "bob" });
    auto a = tr.attribute(0.2, 2.8, 0.0, 0.3);
    CHECK(a.id == "alice" && !a.others && a.share > 0.99);
    auto b = tr.attribute(6.2, 8.9, 0.0, 0.3);
    CHECK(b.id == "bob" && !b.others);
    auto none = tr.attribute(3.5, 5.5, 0.0, 0.3);
    CHECK(none.id.empty());
    // a span that mostly lies in bob's turn
    auto mostly = tr.attribute(5.5, 8.5, 0.0, 0.0);
    CHECK(mostly.id == "bob");
}

static void speaker_overlap()
{
    SpeakerTracker tr;
    const std::vector<std::string> all = { "alice", "bob" };
    feed(tr, 0.0, 4.0, all, { "alice" });
    feed(tr, 4.0, 6.0, all, { "alice", "bob" });   // they talk over each other
    auto a = tr.attribute(3.0, 6.0, 0.0, 0.0);
    CHECK(a.id == "alice" && a.others);
    // one loud voice against a quiet one
    SpeakerTracker t2;
    for (double t = 0.0; t < 3.0 - 1e-9; t += 0.1)
    {
        t2.add(t, { { "alice", true, 0.9f }, { "bob", true, 0.05f } });
    }
    auto b = t2.attribute(0.0, 3.0, 0.0, 0.0);
    CHECK(b.id == "alice");
}

static void speaker_latency()
{
    // The news that bob spoke arrives 0.5 s after the sound: the sound at 6.0-9.0 shows up at 6.5-9.5.
    SpeakerTracker tr;
    const std::vector<std::string> all = { "alice", "bob" };
    feed(tr, 0.0, 3.5, all, { "alice" });
    feed(tr, 3.5, 6.5, all, {});
    feed(tr, 6.5, 9.5, all, { "bob" });
    CHECK(tr.attribute(6.0, 9.0, 0.5, 0.0).id == "bob");
    // without the latency the same span reaches into the gap but still finds bob; a span just before it must not
    CHECK(tr.attribute(3.6, 5.9, 0.5, 0.0).id.empty());
    CHECK(tr.attribute(0.0, 3.0, 0.5, 0.0).id == "alice");
}

static void speaker_forget()
{
    SpeakerTracker tr;
    feed(tr, 0.0, 100.0, { "alice" }, { "alice" });
    CHECK(tr.size() == 1000);
    tr.forget(60.0);
    CHECK(tr.size() <= 410 && tr.size() >= 400);
    CHECK(tr.attribute(70.0, 72.0, 0.0, 0.0).id == "alice");
    tr.add(10.0, {});   // going back in time is ignored
    CHECK(tr.attribute(99.0, 99.5, 0.0, 0.0).id == "alice");
    SpeakerTracker empty;
    CHECK(empty.attribute(0.0, 1.0, 0.0, 0.3).id.empty());
}

static void speaker_clock_stopped()
{
    // No voice is played, so the clock stands still, but the window keeps looking ten times a second.
    SpeakerTracker tr;
    feed(tr, 0.0, 3.0, { "alice", "bob" }, { "alice" });
    const size_t before = tr.size();
    for (int i = 0; i < 100000; ++i)   // almost three hours of looks
    {
        tr.add(3.0, { { "alice", false, 0.0f }, { "bob", i % 2 == 0, 0.5f } });
    }
    CHECK(tr.size() == before + 1);
    CHECK(tr.attribute(0.2, 2.8, 0.0, 0.0).id == "alice");
    // the newest look at the stopped time is the one kept, and it counts once the clock goes on
    tr.add(3.0, { { "alice", false, 0.0f }, { "bob", true, 0.5f } });
    feed(tr, 3.1, 5.0, { "alice", "bob" }, { "bob" });
    CHECK(tr.attribute(3.0, 4.9, 0.0, 0.0).id == "bob");
    // without forget() the list still has an upper bound
    SpeakerTracker big;
    for (int i = 0; i < 20000; ++i)
    {
        big.add(i * 0.1, { { "alice", true, 0.5f } });
    }
    CHECK(big.size() <= 6000);
    CHECK(big.attribute(1990.0, 1995.0, 0.0, 0.0).id == "alice");
}

static void translate_rules()
{
    CHECK(localPairSupported("en", "ja") && localPairSupported("ja", "en"));
    CHECK(localPairSupported("EN", "ja-JP") && localPairSupported("ja", "en_US"));
    CHECK(!localPairSupported("en", "en") && !localPairSupported("zh", "ja") && !localPairSupported("", "ja"));
    // research/222: English into the other languages that have a model, Japanese into English only
    for (const char* t : { "ja", "zh", "es", "fr", "de", "ru", "it", "pt" })
    {
        CHECK(localPairSupported("en", t));
        CHECK(localPairSupported("en-US", t));
    }
    CHECK(!localPairSupported("en", "ar") && !localPairSupported("en", ""));
    // Korean (the multilingual model): ja, en <-> ko, and ko into ja or en only
    CHECK(localPairSupported("ja", "ko") && localPairSupported("ko", "ja") && localPairSupported("en", "ko") && localPairSupported("ko", "en"));
    CHECK(localPairSupported("ko-KR", "ja-JP") && !localPairSupported("ko", "zh") && !localPairSupported("zh", "ko") && !localPairSupported("ko", "ko"));
    CHECK(!localPairSupported("ko", "fr") && !localPairSupported("fr", "ko") && !localPairSupported("ru", "ko"));
    CHECK(!localPairSupported("ja", "zh") && !localPairSupported("ja", "fr") && !localPairSupported("fr", "en") && !localPairSupported("zh", "en"));
    CHECK(needsTranslation("en", "ja"));
    CHECK(needsTranslation("ja", "en"));
    CHECK(!needsTranslation("ja", "ja"));
    CHECK(!needsTranslation("zh", "zh-CN"));     // the regions of one language: left to the service
    CHECK(!needsTranslation("ZH", "zh_TW"));
    CHECK(needsTranslation("en", "zh-CN"));
    CHECK(!needsTranslation("", "ja"));          // language not known
    CHECK(!needsTranslation("en", ""));
    CHECK(primaryLanguage("pt-BR") == "pt");
}

static void hash_known()
{
    {
        Sha256 h;
        h.add("abc", 3);
        CHECK(h.hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }
    {
        Sha256 h;   // in pieces gives the same
        h.add("a", 1);
        h.add("bc", 2);
        CHECK(h.hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }
    {
        Sha256 h;
        CHECK(h.hex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    }
    // a file, longer than the 1 MiB reading piece
    const std::string path = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "\\subtitle_hash_test.bin";
    FILE* f = fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    if (f)
    {
        Sha256 want;
        std::vector<unsigned char> block(1000, 7);
        for (int i = 0; i < 1500; ++i)   // 1.5 MB
        {
            fwrite(block.data(), 1, block.size(), f);
            want.add(block.data(), block.size());
        }
        fclose(f);
        CHECK(sha256File(path) == want.hex() && !want.hex().empty());
        std::atomic<bool> go{ false };
        CHECK(sha256File(path, &go) == want.hex());   // a flag that stays down changes nothing
        std::atomic<bool> stop{ true };
        CHECK(sha256File(path, &stop).empty());       // a cancel stops it before the next piece
        remove(path.c_str());
    }
    CHECK(sha256File("no such file.bin").empty());
}

// An archive as pack.py writes it, with small files.
static std::string makeArchive(const std::vector<std::string>& names, const std::vector<std::string>& data, uint32_t count)
{
    std::string a = "CTM1";
    a.append((const char*)&count, 4);
    for (size_t i = 0; i < names.size(); ++i)
    {
        const uint16_t len = (uint16_t)names[i].size();
        const uint64_t size = data[i].size();
        a.append((const char*)&len, 2);
        a += names[i];
        a.append((const char*)&size, 8);
    }
    for (const std::string& d : data)
    {
        a += d;
    }
    return a;
}

static void write_file(const std::string& path, const std::string& bytes)
{
    FILE* f = fopen(path.c_str(), "wb");
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
}

static std::string read_file(const std::string& path)
{
    std::string out;
    if (FILE* f = fopen(path.c_str(), "rb"))
    {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        {
            out.append(buf, n);
        }
        fclose(f);
    }
    return out;
}

static void translate_archive()
{
    const std::string dir = std::string(getenv("TEMP") ? getenv("TEMP") : ".") + "/subtitle_archive_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::vector<std::string> names = translateModelFiles();
    const std::vector<std::string> data = { "{}", std::string(3000000, 'm'), "[1]", "src", "tgt" };
    write_file(dir + "/ok.ctm", makeArchive(names, data, 5));
    std::string error;
    CHECK(!translateModelInstalled(dir + "/model"));
    CHECK(unpackTranslateArchive(dir + "/ok.ctm", dir + "/model", error));
    CHECK(translateModelInstalled(dir + "/model"));
    CHECK(read_file(dir + "/model/model.bin") == data[1] && read_file(dir + "/model/target.spm") == "tgt");
    CHECK(!std::filesystem::exists(dir + "/model.tmp"));
    // unpacking again replaces the folder
    CHECK(unpackTranslateArchive(dir + "/ok.ctm", dir + "/model", error));
    // refusals leave nothing behind: wrong magic, wrong name (a path), wrong count, cut short
    std::string good = makeArchive(names, data, 5);
    std::string bad = good;
    bad[0] = 'X';
    write_file(dir + "/magic.ctm", bad);
    CHECK(!unpackTranslateArchive(dir + "/magic.ctm", dir + "/m2", error) && !std::filesystem::exists(dir + "/m2"));
    std::vector<std::string> evil = names;
    evil[0] = "../evil";
    write_file(dir + "/evil.ctm", makeArchive(evil, data, 5));
    CHECK(!unpackTranslateArchive(dir + "/evil.ctm", dir + "/m3", error) && !std::filesystem::exists(dir + "/m3"));
    // a model that wants a target-language token has charis.json as a sixth file (and only that name is allowed there)
    {
        std::vector<std::string> six = names;
        std::vector<std::string> six_data = data;
        six.push_back("charis.json");
        six_data.push_back("{\"prefix\": \">>pob<<\"}");
        write_file(dir + "/six.ctm", makeArchive(six, six_data, 6));
        CHECK(unpackTranslateArchive(dir + "/six.ctm", dir + "/m7", error));
        CHECK(translateModelInstalled(dir + "/m7") && read_file(dir + "/m7/charis.json") == six_data[5]);
        six[5] = "other.json";
        write_file(dir + "/six_bad.ctm", makeArchive(six, six_data, 6));
        CHECK(!unpackTranslateArchive(dir + "/six_bad.ctm", dir + "/m8", error) && !std::filesystem::exists(dir + "/m8"));
        six[5] = "charis.json";
        six_data[5] = std::string(5000, 'x');   // far more than a token
        write_file(dir + "/six_big.ctm", makeArchive(six, six_data, 6));
        CHECK(!unpackTranslateArchive(dir + "/six_big.ctm", dir + "/m9", error) && !std::filesystem::exists(dir + "/m9"));
    }
    // a stop flag that is set ends the unpack at once and leaves nothing behind (cancelling a fetch does not wait for the write)
    {
        std::atomic<bool> stop(true);
        CHECK(!unpackTranslateArchive(dir + "/ok.ctm", dir + "/m10", error, &stop) && error == "cancelled");
        CHECK(!std::filesystem::exists(dir + "/m10") && !std::filesystem::exists(dir + "/m10.tmp"));
        std::atomic<bool> go(false);   // not set: it unpacks as before
        CHECK(unpackTranslateArchive(dir + "/ok.ctm", dir + "/m11", error, &go) && translateModelInstalled(dir + "/m11"));
    }
    // a folder with the five files and no charis.json (en-ja as pack.py packs it, or as install-local.ps1 copies it) is a model;
    // one file missing and it is not (2026-11-02: the chat translation looks the model up this way)
    {
        const std::string five = dir + "/en-ja";
        std::filesystem::create_directories(five);
        for (const std::string& name : names)
        {
            write_file(five + "/" + name, "x");
        }
        CHECK(translateModelInstalled(five) && !std::filesystem::exists(five + "/charis.json"));
        std::filesystem::remove(five + "/target.spm");
        CHECK(!translateModelInstalled(five));
        std::filesystem::create_directories(five + "/target.spm");   // a folder of the name is not the file
        CHECK(!translateModelInstalled(five));
    }
    write_file(dir + "/count.ctm", makeArchive(names, data, 4));
    CHECK(!unpackTranslateArchive(dir + "/count.ctm", dir + "/m4", error));
    write_file(dir + "/cut.ctm", good.substr(0, good.size() - 2));
    CHECK(!unpackTranslateArchive(dir + "/cut.ctm", dir + "/m5", error));
    CHECK(!std::filesystem::exists(dir + "/m5") && !std::filesystem::exists(dir + "/m5.tmp"));
    CHECK(!unpackTranslateArchive(dir + "/none.ctm", dir + "/m6", error));
    // an old model folder survives a failed unpack
    CHECK(!unpackTranslateArchive(dir + "/cut.ctm", dir + "/model", error) && translateModelInstalled(dir + "/model"));
    std::filesystem::remove_all(dir);
}

// The address a download may be started with (security audit C, L2).
static void download_url()
{
    CHECK(validDownloadUrl("https://github.com/noriaji/charis-models/releases/download/models-1/en-ja.ctm"));   // the address built in
    CHECK(validDownloadUrl("https://github.com/owner/repo/releases/download/models-1/"));
    CHECK(validDownloadUrl("https://example.org:8443/a/b.ctm?x=1&y=2#z"));
    CHECK(!validDownloadUrl(""));
    CHECK(!validDownloadUrl("https://"));
    CHECK(!validDownloadUrl("http://example.org/a"));
    CHECK(!validDownloadUrl("ftp://example.org/a"));
    CHECK(!validDownloadUrl("file:///C:/x"));
    CHECK(!validDownloadUrl("HTTPS://example.org/a"));   // only the lower case scheme: nothing odd reaches curl
    // a quote could end curl's argument and begin another; so could a space; a backslash and control characters are no address
    CHECK(!validDownloadUrl("https://x/\" -o \"C:/Users/a/Startup/a.exe\" \"https://evil/a.exe"));
    CHECK(!validDownloadUrl("https://example.org/a b"));
    CHECK(!validDownloadUrl("https://example.org/a\\b"));
    CHECK(!validDownloadUrl(std::string("https://example.org/a") + char(1) + "b"));
    CHECK(!validDownloadUrl(std::string("https://example.org/a") + '\n' + "b"));
    CHECK(!validDownloadUrl(std::string("https://example.org/a") + char(0) + "b"));
    CHECK(!validDownloadUrl("https://example.org/\xE3\x81\x82"));   // a non-ASCII byte (a character is not escaped here)
    CHECK(!validDownloadUrl("https://example.org/{a,b}"));         // the braces of URL globbing
    CHECK(!validDownloadUrl("https://example.org/" + std::string(2100, 'a')));
    CHECK(validDownloadUrl("https://example.org/" + std::string(2000, 'a')));
}

// Japanese dialects (research/222): endings and a few words, where a clause ends; nothing else is touched.
static void dialect_rules()
{
    // standard speech, an unknown dialect, an empty text and text that is not Japanese stay as they are
    CHECK(applyDialect("ありがとうございます。", "") == "ありがとうございます。");
    CHECK(applyDialect("ありがとうございます。", "osaka") == "ありがとうございます。");
    CHECK(applyDialect("", "kansai") == "");
    CHECK(applyDialect("Hello there, how are you?", "kansai") == "Hello there, how are you?");
    CHECK(knownDialect("kansai") && knownDialect("hakata") && knownDialect("tohoku") && !knownDialect("") && !knownDialect("osaka"));

    // Kansai
    CHECK(applyDialect("ありがとうございます。", "kansai") == "おおきに。");
    CHECK(applyDialect("これは本当にいいですね。", "kansai") == "これはほんまにええなぁ。");
    CHECK(applyDialect("そうです。", "kansai") == "そうや。");
    CHECK(applyDialect("そうですか？", "kansai") == "そうなん？");
    CHECK(applyDialect("あなたは知らない。", "kansai") == "あんたは知らん。");
    CHECK(applyDialect("それは違います。", "kansai") == "それはちゃいます。");
    CHECK(applyDialect("今日は雨です。", "kansai") == "今日は雨や。");
    CHECK(applyDialect("雨でした", "kansai") == "雨やった");
    // a word is protected from a shorter rule; a rule that needs the end of a clause does not fire in the middle of one
    CHECK(applyDialect("どうぞ、こちらへ。", "kansai") == "どうぞ、こちらへ。");
    CHECK(applyDialect("どうもすみません", "kansai") == "どうもすんません");
    CHECK(applyDialect("元気ですか？", "kansai") == "元気ですか？");
    CHECK(applyDialect("雨ですが、行きます。", "kansai") == "雨ですが、行きます。");
    CHECK(applyDialect("知らないので", "kansai") == "知らないので");

    // Hakata
    CHECK(applyDialect("そうですか？", "hakata") == "そうと？");
    CHECK(applyDialect("これは本当に好きです。", "hakata") == "これはほんなこつ好いとうよ。");
    CHECK(applyDialect("とても大きいです。", "hakata") == "ばり大きいばい。");
    CHECK(applyDialect("どうして来たの？", "hakata") == "なして来たの？");

    // Tohoku
    CHECK(applyDialect("私はそうです。", "tohoku") == "おらはんだ。");
    CHECK(applyDialect("それはかわいいですね。", "tohoku") == "それはめんこいだなぁ。");
    CHECK(applyDialect("あなたは知らない。", "tohoku") == "おめえは知らねえ。");

    // a rule starts at a character boundary: no rule fires inside a character (the bytes of a longer one)
    CHECK(applyDialect("だ", "kansai") == "や");
    CHECK(applyDialect("ですよ", "tohoku") == "だべさ");
    // a long text with several sentences is rewritten all through
    CHECK(applyDialect("ありがとう。本当にすごいです。また会いましょう。", "kansai") == "おおきに。ほんまにすごいや。また会いましょう。");

    // the word before a rule: "だ" of a past tense, "まだ", "いい" of "言い…", "私" of "私立" stay as they are
    CHECK(applyDialect("本を読んだ。", "kansai") == "本を読んだ。");
    CHECK(applyDialect("急いだ。", "kansai") == "急いだ。");
    CHECK(applyDialect("まだ。", "kansai") == "まだ。");
    CHECK(applyDialect("まだ。", "hakata") == "まだ。");
    CHECK(applyDialect("まだ。", "tohoku") == "まだ。");
    CHECK(applyDialect("読んだ。", "tohoku") == "読んだ。");
    CHECK(applyDialect("そういいます。", "kansai") == "そういいます。");
    CHECK(applyDialect("いいわけ", "hakata") == "いいわけ");
    CHECK(applyDialect("それでいいわ。", "kansai") == "それでええわ。");
    CHECK(applyDialect("私立の学校", "tohoku") == "私立の学校");
    CHECK(applyDialect("下僕", "tohoku") == "下僕");
    // ... while the copula and the words themselves still change
    CHECK(applyDialect("雨だ。", "kansai") == "雨や。");
    CHECK(applyDialect("好きなんだ。", "kansai") == "好きなんや。");
    CHECK(applyDialect("それはいい。", "kansai") == "それはええ。");
    CHECK(applyDialect("それはいい。", "hakata") == "それはよか。");
    CHECK(applyDialect("私は行かない。", "tohoku") == "おらは行がねえ。");
    CHECK(applyDialect("僕です。", "tohoku") == "おらだべ。");
    // "まだ" only as a word, and the copula after "い" / "ん" where it is one
    CHECK(applyDialect("そのままだ。", "kansai") == "そのままや。");
    CHECK(applyDialect("じゃまだ。", "kansai") == "じゃまや。");
    CHECK(applyDialect("ひまだ。", "hakata") == "ひまたい。");
    CHECK(applyDialect("まだ来ない。", "kansai") == "まだけえへん。");
    CHECK(applyDialect("雨みたいだ。", "kansai") == "雨みたいや。");
    CHECK(applyDialect("それは嫌いだ。", "kansai") == "それは嫌いや。");
    CHECK(applyDialect("きれいだ。", "hakata") == "きれいたい。");
    CHECK(applyDialect("もう行くんだ。", "kansai") == "もう行くんや。");
    CHECK(applyDialect("言ったんだ。", "tohoku") == "言ったんだべ。");
    CHECK(applyDialect("本を読んだ。", "hakata") == "本を読んだ。");
    // the past of a verb in む written in kana is not the "…んだ" that explains (research/275 L094)
    CHECK(applyDialect("服をたたんだ。", "kansai") == "服をたたんだ。");
    CHECK(applyDialect("ねじがゆるんだ。", "kansai") == "ねじがゆるんだ。");
    CHECK(applyDialect("服をたたんだ。", "hakata") == "服をたたんだ。");
    CHECK(applyDialect("ねじがゆるんだ。", "tohoku") == "ねじがゆるんだ。");
    CHECK(applyDialect("腕をくんだ。", "kansai") == "腕をくんだ。");
    CHECK(applyDialect("荷物をつんだ。", "hakata") == "荷物をつんだ。");
    CHECK(applyDialect("紙でつつんだ。", "tohoku") == "紙でつつんだ。");
    CHECK(applyDialect("目が涙ぐんだ。", "kansai") == "目が涙ぐんだ。");
    CHECK(applyDialect("口をつぐんだ。", "kansai") == "口をつぐんだ。");
    CHECK(applyDialect("問題に取りくんだ。", "hakata") == "問題に取りくんだ。");
    CHECK(applyDialect("子をはぐくんだ。", "tohoku") == "子をはぐくんだ。");
    CHECK(applyDialect("足がすくんだ。", "kansai") == "足がすくんだ。");
    CHECK(applyDialect("水をふくんだ。", "kansai") == "水をふくんだ。");
    // kana that is both (来る / 包む, 塗る / 温む): the standard form; with the kanji the dialect
    CHECK(applyDialect("彼がくるんだ。", "kansai") == "彼がくるんだ。");
    CHECK(applyDialect("彼が来るんだ。", "kansai") == "彼が来るんや。");
    CHECK(applyDialect("ペンキを塗るんだ。", "hakata") == "ペンキを塗るんたい。");
    // ... while the one that explains still changes
    CHECK(applyDialect("家に帰るんだ。", "kansai") == "家に帰るんや。");
    CHECK(applyDialect("急ぐんだ。", "hakata") == "急ぐんたい。");
    CHECK(applyDialect("そう言うんだ。", "tohoku") == "そう言うんだべ。");
    CHECK(applyDialect("ここで待つんだ。", "kansai") == "ここで待つんや。");
    CHECK(applyDialect("見たんだ。", "hakata") == "見たんたい。");
}

int main()
{
    ring_basic();
    ring_threads();
    printf("converter:\n");
    converter_tone(48000, 2, 480);
    converter_tone(48000, 1, 7);
    converter_tone(44100, 2, 441);
    converter_tone(16000, 1, 160);
    converter_tone(24000, 2, 240);
    converter_edges();
    speaker_basic();
    speaker_overlap();
    speaker_latency();
    speaker_forget();
    speaker_clock_stopped();
    translate_rules();
    hash_known();
    translate_archive();
    download_url();
    dialect_rules();
    printf(sFailed ? "FAILED (%d)\n" : "all passed\n", sFailed);
    return sFailed ? 1 : 0;
}
