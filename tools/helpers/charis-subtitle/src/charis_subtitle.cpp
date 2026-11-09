/**
 * @file charis_subtitle.cpp
 * @brief charis-subtitle.exe, the speech recognition helper of Charis Viewer's voice subtitles (whisper.cpp, on the CPU).
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

// charis-subtitle — the recognition helper the viewer's subtitle feature (plus/subtitle) starts.
//
// stdin  : frames of  u8 type, u32 length (little endian), payload
//            1 PCM    16 kHz mono signed 16 bit samples
//            2 FLUSH  end the utterance in progress now
//            3 QUIT   stop at once
//            4 VOICED payload u8: the viewer knows whether anyone is speaking (voice
//                     participants' flags): 0 nobody, 1 somebody; it replaces the loudness test
//                     from then on. 2: not known (the parcel's radio): the loudness test again,
//                     until the next 0 or 1 (a helper before this took 2 as 1).
//          End of input finishes the utterance in progress and exits (the viewer died or closed us).
// stdout : one JSON object per line (UTF-8)
//            {"event":"loading"}
//            {"event":"ready","load_ms":N}
//            {"event":"text","t0":S,"t1":S,"lang":"ja","text":"...","compute_ms":N,"cpu_ms":N}
//            {"event":"dropped","seconds":S}      audio thrown away because recognition fell behind
//            {"event":"error","message":"..."}
//          Times are seconds of audio received since start (dropped audio counts).
// stderr : log, with -v

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "whisper.h"

namespace {

constexpr int kRate = 16000;
constexpr size_t kChunk = kRate / 10;  // the segmenter looks at 100 ms at a time

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct Options {
    std::string model;
    std::string lang = "auto";
    int threads = 4;
    float thr = 0.010f;        // loudness (RMS) that counts as speech, until the viewer sends VOICED
    float maxseg = 8.0f;       // longest piece sent to Whisper (s)
    float hang = 0.5f;         // silence that ends a piece (s)
    float minseg = 0.6f;       // shorter pieces are dropped (s)
    float maxbacklog = 12.0f;  // audio waiting longer than this is dropped (s)
    int minctx = 384;          // smallest encoder window, 50 per second (short ones make Whisper loop)
    float tokps = 7.0f;        // cap on decoded tokens per second of audio (stops repetition loops)
    bool lowprio = true;
    bool verbose = false;
};

// ---- output -------------------------------------------------------------------------------------

std::mutex g_out_mutex;

std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += char(c);
                }
        }
    }
    return o;
}

void emit(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_out_mutex);
    fwrite(line.data(), 1, line.size(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

void emit_error(const std::string& msg) { emit("{\"event\":\"error\",\"message\":\"" + json_escape(msg) + "\"}"); }

// ---- input queue --------------------------------------------------------------------------------

enum class Kind { Pcm, Flush, Voiced, End };

struct Item {
    Kind kind = Kind::Pcm;
    std::vector<float> pcm;
    uint8_t voiced = 0;     // Kind::Voiced: the payload (0, 1, 2 not known)
};

class Queue {
public:
    void push(Item&& it) {
        std::lock_guard<std::mutex> lk(m_);
        if (it.kind == Kind::Pcm) samples_ += it.pcm.size();
        q_.push_back(std::move(it));
        cv_.notify_one();
    }
    Item pop() {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return !q_.empty(); });
        Item it = std::move(q_.front());
        q_.pop_front();
        if (it.kind == Kind::Pcm) samples_ -= it.pcm.size();
        return it;
    }
    // Throws away queued audio until at most keep_samples remain; returns how many were thrown away.
    size_t drop_to(size_t keep_samples) {
        std::lock_guard<std::mutex> lk(m_);
        size_t dropped = 0;
        for (auto it = q_.begin(); it != q_.end() && samples_ > keep_samples;) {
            if (it->kind == Kind::Pcm) {
                samples_ -= it->pcm.size();
                dropped += it->pcm.size();
                it = q_.erase(it);
            } else if (it->kind == Kind::End) {
                break;
            } else {
                ++it;
            }
        }
        return dropped;
    }
    void clear_for_quit() {
        std::lock_guard<std::mutex> lk(m_);
        q_.clear();
        samples_ = 0;
        Item e;
        e.kind = Kind::End;
        q_.push_back(std::move(e));
        cv_.notify_one();
    }
    size_t backlog_samples() {
        std::lock_guard<std::mutex> lk(m_);
        return samples_;
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Item> q_;
    size_t samples_ = 0;
};

bool read_exact(FILE* f, void* dst, size_t n) { return fread(dst, 1, n, f) == n; }

void reader(Queue& q) {
    for (;;) {
        uint8_t type;
        uint32_t len;
        if (!read_exact(stdin, &type, 1) || !read_exact(stdin, &len, 4) || len > (1u << 24)) break;
        std::vector<uint8_t> payload(len);
        if (len && !read_exact(stdin, payload.data(), len)) break;
        Item it;
        if (type == 1) {
            it.kind = Kind::Pcm;
            size_t n = len / 2;
            it.pcm.resize(n);
            for (size_t i = 0; i < n; ++i) {
                int16_t v;
                memcpy(&v, &payload[i * 2], 2);
                it.pcm[i] = v / 32768.0f;
            }
        } else if (type == 2) {
            it.kind = Kind::Flush;
        } else if (type == 3) {
            q.clear_for_quit();
            return;
        } else if (type == 4 && len >= 1) {
            it.kind = Kind::Voiced;
            it.voiced = payload[0];
        } else {
            continue;  // unknown frame: skip it
        }
        q.push(std::move(it));
    }
    Item e;
    e.kind = Kind::End;
    q.push(std::move(e));
}

// ---- recognition --------------------------------------------------------------------------------

double cpu_ms() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto v = [](FILETIME f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e4; };
    return v(k) + v(u);
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

void recognize(whisper_context* ctx, const Options& o, const std::vector<float>& piece, double t0, double t1) {
    std::vector<float> a(piece);
    if (a.size() < size_t(kRate * 1.05)) a.resize(size_t(kRate * 1.05), 0.0f);  // Whisper wants > 1 s
    double len_s = double(a.size()) / kRate;

    whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wp.n_threads = o.threads;
    wp.language = o.lang.c_str();
    wp.translate = false;
    wp.no_context = true;
    wp.single_segment = true;
    wp.no_timestamps = true;
    wp.print_progress = wp.print_realtime = wp.print_timestamps = wp.print_special = false;
    wp.temperature_inc = 0.0f;  // no re-decode at a higher temperature: it doubles the cost for little gain
    // Unless told otherwise the encoder works on a fixed 30 s window; 50 positions per second.
    int ctx_len = int(std::ceil((len_s + 1.0) * 50.0 / 64.0)) * 64;
    wp.audio_ctx = std::min(std::max(ctx_len, o.minctx), 1500);
    wp.max_tokens = std::max(12, int(len_s * o.tokps) + 8);

    double cpu0 = cpu_ms();
    auto c0 = Clock::now();
    int rc = whisper_full(ctx, wp, a.data(), int(a.size()));
    double wall = ms_since(c0);
    double cpu = cpu_ms() - cpu0;
    if (rc != 0) {
        emit_error("whisper_full failed (" + std::to_string(rc) + ")");
        return;
    }

    std::string text;
    for (int i = 0; i < whisper_full_n_segments(ctx); ++i) text += whisper_full_get_segment_text(ctx, i);
    text = trim(text);
    if (text.empty()) return;

    std::string lang = o.lang;
    if (lang == "auto") {
        int id = whisper_full_lang_id(ctx);
        const char* s = id >= 0 ? whisper_lang_str(id) : nullptr;
        lang = s ? s : "";
    }
    char head[160];
    snprintf(head, sizeof head, "{\"event\":\"text\",\"t0\":%.2f,\"t1\":%.2f,\"lang\":\"", t0, t1);
    char tail[96];
    snprintf(tail, sizeof tail, "\",\"compute_ms\":%.0f,\"cpu_ms\":%.0f}", wall, cpu);
    emit(std::string(head) + json_escape(lang) + "\",\"text\":\"" + json_escape(text) + tail);
}

// Cuts the stream into pieces and recognizes each one.
void worker(whisper_context* ctx, const Options& o, Queue& q) {
    const size_t hang_chunks = size_t(o.hang * 10);
    const size_t pre_chunks = 3;
    const size_t max_chunks = size_t(o.maxseg * 10);
    const size_t keep_after_drop = size_t(2 * kRate);

    std::deque<std::vector<float>> pre;  // the last few idle chunks, so a piece does not start mid-word
    std::vector<float> piece, accum;
    bool speaking = false, hint_seen = false, hint_on = false;
    size_t silent = 0, chunks_in_piece = 0;
    uint64_t consumed = 0;  // samples taken off the queue, dropped ones included
    double piece_start = 0;

    auto flush = [&](size_t drop_tail_chunks) {
        if (piece.size() > drop_tail_chunks * kChunk) piece.resize(piece.size() - drop_tail_chunks * kChunk);
        if (double(piece.size()) / kRate >= o.minseg)
            recognize(ctx, o, piece, piece_start, piece_start + double(piece.size()) / kRate);
        piece.clear();
        speaking = false;
        silent = 0;
        chunks_in_piece = 0;
    };

    auto on_chunk = [&](const std::vector<float>& c) {
        double e = 0;
        for (float v : c) e += double(v) * v;
        bool voiced = hint_seen ? hint_on : float(std::sqrt(e / double(c.size()))) > o.thr;
        double chunk_start = double(consumed - c.size()) / kRate;
        if (!speaking) {
            if (voiced) {
                speaking = true;
                piece_start = std::max(0.0, chunk_start - double(pre.size()) * 0.1);
                for (auto& p : pre) piece.insert(piece.end(), p.begin(), p.end());
                pre.clear();
                piece.insert(piece.end(), c.begin(), c.end());
                chunks_in_piece = 1;
                silent = 0;
            } else {
                pre.push_back(c);
                if (pre.size() > pre_chunks) pre.pop_front();
            }
            return;
        }
        piece.insert(piece.end(), c.begin(), c.end());
        ++chunks_in_piece;
        silent = voiced ? 0 : silent + 1;
        if (silent >= hang_chunks)
            flush(silent > 2 ? silent - 2 : 0);  // keep 0.2 s of tail
        else if (chunks_in_piece >= max_chunks)
            flush(0);
    };

    for (;;) {
        // Recognition fell behind (a long piece, a busy CPU): throw the oldest audio away.
        if (q.backlog_samples() > size_t(o.maxbacklog * kRate)) {
            size_t n = q.drop_to(keep_after_drop);
            if (n) {
                consumed += n;
                accum.clear();
                piece.clear();
                pre.clear();
                speaking = false;
                silent = chunks_in_piece = 0;
                char b[64];
                snprintf(b, sizeof b, "{\"event\":\"dropped\",\"seconds\":%.1f}", double(n) / kRate);
                emit(b);
            }
        }
        Item it = q.pop();
        if (it.kind == Kind::End) break;
        if (it.kind == Kind::Voiced) {
            hint_seen = it.voiced <= 1;     // 2 (or more): not known, cut by loudness
            hint_on = it.voiced == 1;
        } else if (it.kind == Kind::Flush) {
            if (speaking) flush(0);
        } else {
            accum.insert(accum.end(), it.pcm.begin(), it.pcm.end());
            size_t used = 0;
            while (accum.size() - used >= kChunk) {
                std::vector<float> c(accum.begin() + used, accum.begin() + used + kChunk);
                used += kChunk;
                consumed += kChunk;
                on_chunk(c);
            }
            accum.erase(accum.begin(), accum.begin() + used);
        }
    }
    if (speaking) flush(0);
}

void quiet_log(ggml_log_level, const char*, void*) {}

void usage() {
    fprintf(stderr,
            "usage: charis-subtitle -m MODEL [options]   (frames on stdin, JSON lines on stdout; see the top of the source)\n"
            "  -t N          Whisper threads (default 4)\n"
            "  -l LANG       language code or auto (default auto)\n"
            "  --thr X       loudness that counts as speech, RMS 0..1 (default 0.010)\n"
            "  --maxseg S    longest piece, seconds (default 8)\n"
            "  --hang S      silence that ends a piece (default 0.5)\n"
            "  --maxbacklog S  audio waiting longer than this is dropped (default 12)\n"
            "  --minctx N    smallest encoder window, 50 per second (default 384)\n"
            "  --tokps X     max decoded tokens per audio second (default 7)\n"
            "  --normalprio  do not lower the process priority\n"
            "  -v            log to stderr\n");
}

}  // namespace

int main(int argc, char** argv) {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "-m") o.model = next();
        else if (a == "-t") o.threads = std::max(1, atoi(next()));
        else if (a == "-l") o.lang = next();
        else if (a == "--thr") o.thr = float(atof(next()));
        else if (a == "--maxseg") o.maxseg = float(atof(next()));
        else if (a == "--hang") o.hang = float(atof(next()));
        else if (a == "--maxbacklog") o.maxbacklog = float(atof(next()));
        else if (a == "--minctx") o.minctx = atoi(next());
        else if (a == "--tokps") o.tokps = float(atof(next()));
        else if (a == "--normalprio") o.lowprio = false;
        else if (a == "-v") o.verbose = true;
        else return usage(), 2;
    }
    if (o.model.empty()) return usage(), 2;
    if (o.lowprio) SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    if (!o.verbose) whisper_log_set(quiet_log, nullptr);

    emit("{\"event\":\"loading\"}");
    // Read from the start: the viewer may already be writing while the model loads (a full pipe would stall it).
    Queue q;
    std::thread rd(reader, std::ref(q));

    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = false;  // CPU only, on purpose
    auto t0 = Clock::now();
    whisper_context* ctx = whisper_init_from_file_with_params(o.model.c_str(), cp);
    if (!ctx) {
        emit_error("cannot load model: " + o.model);
        q.clear_for_quit();
        rd.detach();  // blocked in fread; the process is about to exit
        return 1;
    }
    char b[64];
    snprintf(b, sizeof b, "{\"event\":\"ready\",\"load_ms\":%.0f}", ms_since(t0));
    emit(b);

    worker(ctx, o, q);
    whisper_free(ctx);
    rd.join();  // End was queued by the reader itself, so it has returned
    return 0;
}
