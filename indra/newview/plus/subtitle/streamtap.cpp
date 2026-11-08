/**
 * @file streamtap.cpp
 * @brief Subtitles for the parcel's internet radio (research/192): process loopback capture.
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

#include "linden_common.h"

#include "streamtap.h"
#include "subtitlering.h"

#include <algorithm>
#include <atomic>
#include <thread>

#if LL_WINDOWS
#define PLUS_STREAMTAP_AVAILABLE 1
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#endif

namespace plus
{
    namespace subtitle
    {
        namespace streamtap
        {
            namespace
            {
                constexpr size_t RING_SECONDS = 20;

                // Never destroyed, like the voice tap's ring: nothing can use it after the statics are gone.
                Ring& ring()
                {
                    static Ring* const sRing = new Ring(size_t(RATE) * RING_SECONDS);
                    return *sRing;
                }

                std::atomic<uint32_t> sPid{ 0 };
                std::atomic<uint64_t> sGeneration{ 0 };
            }

            uint64_t generation()
            {
                return sGeneration.load(std::memory_order_acquire);
            }

            size_t read(int16_t* dst, size_t max)
            {
                return ring().read(dst, max);
            }

            void discard()
            {
                ring().clear();
            }

            uint32_t pid()
            {
                return sPid.load(std::memory_order_relaxed);
            }

#ifdef PLUS_STREAMTAP_AVAILABLE

            namespace
            {
                // What the capture asks for. The format of a process loopback cannot be asked of the
                // device, it is given: CD quality stereo is accepted everywhere. Converter brings it to 16 kHz.
                constexpr int  CAPTURE_RATE     = 44100;
                constexpr int  CAPTURE_CHANNELS = 2;
                constexpr int  CAPTURE_BITS     = 16;
                constexpr DWORD ACTIVATE_WAIT_MS = 5000;
                constexpr DWORD LOOP_WAIT_MS     = 200;   // the thread looks at the stop flag at least this often
                constexpr size_t CHUNK_FRAMES    = 1024;  // input frames converted at a time (the output fits out[])

                // audioclientactivationparams.h hides these below the Windows version the viewer is built for
                // (it asks for Windows 7 compatibility); the layout is fixed by the OS, so they are declared here.
                enum LoopbackActivationType { ACTIVATION_PROCESS_LOOPBACK = 1 };
                enum LoopbackMode { LOOPBACK_INCLUDE_TARGET_PROCESS_TREE = 0 };
                struct LoopbackParams
                {
                    DWORD targetProcessId;
                    LoopbackMode mode;
                };
                struct ActivationParams
                {
                    LoopbackActivationType type;
                    LoopbackParams loopback;
                };
                constexpr wchar_t PROCESS_LOOPBACK_DEVICE[] = L"VAD\\Process_Loopback";

                using ActivateFn = HRESULT(WINAPI*)(LPCWSTR, REFIID, PROPVARIANT*, IActivateAudioInterfaceCompletionHandler*,
                                                    IActivateAudioInterfaceAsyncOperation**);

                // The call is in Mmdevapi.dll; it is looked up here so that the viewer links and starts on a
                // Windows that has no process loopback (the subtitles of the radio are off there).
                ActivateFn activateFn()
                {
                    static ActivateFn const fn = [] {
                        HMODULE dll = LoadLibraryW(L"Mmdevapi.dll");
                        return dll ? reinterpret_cast<ActivateFn>(GetProcAddress(dll, "ActivateAudioInterfaceAsync")) : nullptr;
                    }();
                    return fn;
                }

                // The completion handler of ActivateAudioInterfaceAsync: only says "done".
                class ActivateDone final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject
                {
                public:
                    explicit ActivateDone(HANDLE ev) : mEvent(ev) {}

                    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
                    {
                        if (iid == __uuidof(IUnknown) || iid == __uuidof(IActivateAudioInterfaceCompletionHandler))
                        {
                            *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
                        }
                        else if (iid == __uuidof(IAgileObject))
                        {
                            *out = static_cast<IAgileObject*>(this);
                        }
                        else
                        {
                            *out = nullptr;
                            return E_NOINTERFACE;
                        }
                        AddRef();
                        return S_OK;
                    }
                    ULONG STDMETHODCALLTYPE AddRef() override { return ++mRefs; }
                    ULONG STDMETHODCALLTYPE Release() override
                    {
                        const ULONG n = --mRefs;
                        if (n == 0)
                        {
                            delete this;
                        }
                        return n;
                    }
                    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override
                    {
                        SetEvent(mEvent);
                        return S_OK;
                    }

                private:
                    ~ActivateDone() = default;
                    std::atomic<ULONG> mRefs{ 1 };
                    HANDLE mEvent;
                };

                template <class T>
                void release(T*& p)
                {
                    if (p)
                    {
                        p->Release();
                        p = nullptr;
                    }
                }

                struct Capture
                {
                    std::thread thread;
                    HANDLE stopEvent = nullptr;
                    // set by the thread when it has started (or could not): start() waits for it to report
                    HANDLE startedEvent = nullptr;
                    std::string error;   // written before startedEvent is set
                    bool ok = false;
                };

                Capture* sCapture = nullptr;   // main thread only

                void feed(Converter& conv, const int16_t* frames, size_t count, bool silent)
                {
                    static const int16_t zeros[CHUNK_FRAMES * CAPTURE_CHANNELS] = {};
                    int16_t out[CHUNK_FRAMES];
                    size_t done = 0;
                    while (done < count)
                    {
                        const size_t n = std::min(CHUNK_FRAMES, count - done);
                        const int16_t* in = silent ? zeros : frames + done * CAPTURE_CHANNELS;
                        const size_t got = conv.convert(in, n, CAPTURE_CHANNELS, CAPTURE_RATE, out, sizeof(out) / sizeof(out[0]));
                        ring().write(out, got);
                        done += n;
                    }
                }

                void captureMain(Capture* cap, uint32_t pid)
                {
                    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
                    IAudioClient* client = nullptr;
                    IAudioCaptureClient* capture = nullptr;
                    HANDLE bufferEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    HANDLE doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                    bool started = false;

                    auto fail = [&](const char* what, HRESULT hr) {
                        cap->error = std::string(what) + " (HRESULT 0x" + llformat("%08x", (unsigned)hr) + ")";
                    };

                    do
                    {
                        if (!bufferEvent || !doneEvent)
                        {
                            cap->error = "cannot create events";
                            break;
                        }
                        ActivateFn activate = activateFn();
                        if (!activate)
                        {
                            cap->error = "process loopback capture is not available on this Windows";
                            break;
                        }

                        ActivationParams params = {};
                        params.type = ACTIVATION_PROCESS_LOOPBACK;
                        params.loopback.targetProcessId = pid;
                        params.loopback.mode = LOOPBACK_INCLUDE_TARGET_PROCESS_TREE;
                        PROPVARIANT prop = {};
                        prop.vt = VT_BLOB;
                        prop.blob.cbSize = sizeof(params);
                        prop.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

                        ActivateDone* handler = new ActivateDone(doneEvent);
                        IActivateAudioInterfaceAsyncOperation* op = nullptr;
                        HRESULT hr = activate(PROCESS_LOOPBACK_DEVICE, __uuidof(IAudioClient), &prop, handler, &op);
                        if (FAILED(hr))
                        {
                            handler->Release();
                            fail("ActivateAudioInterfaceAsync failed", hr);
                            break;
                        }
                        const DWORD waited = WaitForSingleObject(doneEvent, ACTIVATE_WAIT_MS);
                        HRESULT result = E_FAIL;
                        IUnknown* unknown = nullptr;
                        if (waited == WAIT_OBJECT_0)
                        {
                            hr = op->GetActivateResult(&result, &unknown);
                        }
                        release(op);
                        handler->Release();
                        if (waited != WAIT_OBJECT_0 || FAILED(hr) || FAILED(result) || !unknown)
                        {
                            release(unknown);
                            if (waited != WAIT_OBJECT_0)
                            {
                                cap->error = "the activation of the capture did not answer";
                            }
                            else
                            {
                                fail("the capture cannot be activated", FAILED(hr) ? hr : result);
                            }
                            break;
                        }
                        hr = unknown->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&client));
                        release(unknown);
                        if (FAILED(hr))
                        {
                            fail("no IAudioClient", hr);
                            break;
                        }

                        WAVEFORMATEX format = {};
                        format.wFormatTag = WAVE_FORMAT_PCM;
                        format.nChannels = CAPTURE_CHANNELS;
                        format.nSamplesPerSec = CAPTURE_RATE;
                        format.wBitsPerSample = CAPTURE_BITS;
                        format.nBlockAlign = WORD(format.nChannels * format.wBitsPerSample / 8);
                        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
                        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                200000 /* 20 ms in 100 ns units */, 0, &format, nullptr);
                        if (FAILED(hr))
                        {
                            fail("the capture cannot be initialized", hr);
                            break;
                        }
                        hr = client->SetEventHandle(bufferEvent);
                        if (FAILED(hr))
                        {
                            fail("SetEventHandle failed", hr);
                            break;
                        }
                        hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture));
                        if (FAILED(hr))
                        {
                            fail("no IAudioCaptureClient", hr);
                            break;
                        }
                        hr = client->Start();
                        if (FAILED(hr))
                        {
                            fail("the capture cannot start", hr);
                            break;
                        }
                        started = true;
                    } while (false);

                    cap->ok = started;
                    SetEvent(cap->startedEvent);

                    if (started)
                    {
                        Converter conv;   // this thread is the ring's one writer
                        HANDLE waits[2] = { cap->stopEvent, bufferEvent };
                        for (;;)
                        {
                            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, LOOP_WAIT_MS);
                            if (w == WAIT_OBJECT_0)
                            {
                                break;   // stop()
                            }
                            // WAIT_TIMEOUT: nothing was played in that time, the loop looks at the stop flag again
                            for (;;)
                            {
                                UINT32 packet = 0;
                                if (FAILED(capture->GetNextPacketSize(&packet)) || packet == 0)
                                {
                                    break;
                                }
                                BYTE* data = nullptr;
                                UINT32 frames = 0;
                                DWORD flags = 0;
                                if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                                {
                                    break;
                                }
                                feed(conv, reinterpret_cast<const int16_t*>(data), frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
                                capture->ReleaseBuffer(frames);
                            }
                        }
                        client->Stop();
                    }
                    release(capture);
                    release(client);
                    if (bufferEvent)
                    {
                        CloseHandle(bufferEvent);
                    }
                    if (doneEvent)
                    {
                        CloseHandle(doneEvent);
                    }
                    if (com)
                    {
                        CoUninitialize();
                    }
                }
            }

            bool start(uint32_t target, std::string& error)
            {
                stop();
                if (target == 0)
                {
                    error = "no process to listen to";
                    return false;
                }
                if (!activateFn())
                {
                    error = "process loopback capture is not available on this Windows (needs Windows 10 2004 or later)";
                    return false;
                }
                Capture* cap = new Capture;
                cap->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                cap->startedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (!cap->stopEvent || !cap->startedEvent)
                {
                    error = "cannot create events";
                    if (cap->stopEvent)
                    {
                        CloseHandle(cap->stopEvent);
                    }
                    if (cap->startedEvent)
                    {
                        CloseHandle(cap->startedEvent);
                    }
                    delete cap;
                    return false;
                }
                cap->thread = std::thread(captureMain, cap, target);
                // The thread answers within the activation's wait; the main thread waits no longer than that.
                const DWORD waited = WaitForSingleObject(cap->startedEvent, ACTIVATE_WAIT_MS + 2000);
                if (waited != WAIT_OBJECT_0 || !cap->ok)
                {
                    error = waited != WAIT_OBJECT_0 ? std::string("the capture did not start in time") : cap->error;
                    SetEvent(cap->stopEvent);
                    cap->thread.join();
                    CloseHandle(cap->stopEvent);
                    CloseHandle(cap->startedEvent);
                    delete cap;
                    return false;
                }
                sCapture = cap;
                sPid = target;
                sGeneration.fetch_add(1, std::memory_order_release);
                return true;
            }

            void stop()
            {
                if (!sCapture)
                {
                    return;
                }
                SetEvent(sCapture->stopEvent);
                sCapture->thread.join();
                CloseHandle(sCapture->stopEvent);
                CloseHandle(sCapture->startedEvent);
                delete sCapture;
                sCapture = nullptr;
                sPid = 0;
            }

#else   // not Windows, or an SDK without process loopback

            bool start(uint32_t, std::string& error)
            {
                error = "the subtitles of the radio are available on Windows only";
                return false;
            }
            void stop() {}

#endif
        }
    }
}
