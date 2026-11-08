/**
 * @file vkcheck.cpp
 * @brief Vulkan renderer start-up: ends the viewer with a message when this PC
 *        cannot run Vulkan (saying what the GPU lacks), and keeps the GL
 *        program binary cache out of the way.
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

// research/82, 85: the viewer renders with Vulkan only. The window and Vulkan
// are set up inside LLAppViewer::init(), after the plus features' init: init()
// hands LLRHI the exit below, which LLRHIVKMirror::install() calls when
// LLVKDevice::init(), the frame or the swapchain fails. That is before the
// "RenderInitError" mark is set, so the next start keeps the graphics settings.
// fix-submitfail C: the same exit, with its own message, when the device or
// the window's surface is lost while the viewer runs (called after a present,
// on the main thread).
// require-depthclip (research/124 V1): a GPU with Vulkan 1.3 that lacks a
// required feature (VK_EXT_depth_clip_control above all) gets a message that
// names the feature, the GPU and the driver update (MissingFeatures).

#include "llviewerprecompiledheaders.h"

#include "plus.h"
#include "vkcheck.h"
#include "plus/gpudgc/gpudgc.h"
#include "plus/ao/aoengine.h"

#include "llappviewer.h"
#include "lldir.h"
#include "llerrorcontrol.h"
#include "llgl.h"
#include "llrhi.h"
#include "llstartup.h"
#include "lltrans.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llwindow.h"

#include <algorithm>
#include <sstream>
#include <vector>

namespace
{
    // reasons: one per line, each shown as a list item.
    std::string reason_list(const std::string& reasons)
    {
        std::string list;
        size_t start = 0;
        while (start < reasons.size())
        {
            size_t end = reasons.find('\n', start);
            if (end == std::string::npos)
            {
                end = reasons.size();
            }
            list += "- " + reasons.substr(start, end - start) + "\n";
            start = end + 1;
        }
        return list;
    }

    // MissingFeatures: the feature names of the "<GPU>: missing <name> (<what
    // has it>), ..." lines (LLRHI::VulkanUnavailable), each once, joined with
    // separator; other lines are skipped.
    std::string missing_feature_names(const std::string& reasons, const std::string& separator)
    {
        static const std::string MISSING = ": missing ";
        std::vector<std::string> names;
        std::istringstream lines(reasons);
        std::string line;
        while (std::getline(lines, line))
        {
            const size_t at = line.rfind(MISSING);
            if (at == std::string::npos)
            {
                continue;
            }
            size_t start = at + MISSING.size();
            while (start < line.size())
            {
                size_t end = line.find(", ", start);
                if (end == std::string::npos)
                {
                    end = line.size();
                }
                std::string name = line.substr(start, end - start);
                name = name.substr(0, name.find(" ("));
                if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end())
                {
                    names.push_back(name);
                }
                start = end + 2;
            }
        }
        std::string joined;
        for (const std::string& name : names)
        {
            joined += (joined.empty() ? "" : separator) + name;
        }
        return joined;
    }

    // fix-round52 5: the log's full path, so the message says where it is: the file this run
    // logs to (a second viewer running at once logs to another file), else the usual one.
    std::string log_file()
    {
        std::string file = LLError::logFileName();
        if (file.empty() && gDirUtilp)
        {
            file = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "SecondLife.log");
        }
        return file.empty() ? std::string("SecondLife.log") : file;
    }

    std::string message(LLRHI::VulkanUnavailable why, const std::string& reasons)
    {
        const bool japanese = LLUI::getLanguage().rfind("ja", 0) == 0;
        const std::string log_path = log_file();
        // fix-submitfail C: while running. The reasons (in English) go to the
        // log only: the first sentence already says what happened.
        if (why == LLRHI::VulkanUnavailable::DeviceLost)
        {
            // The GPU stopped (Windows reset it, the driver was updated, it
            // faulted); nothing renders again.
            return japanese
                ? "GPU が応答しなくなったため、ビューワを終了します。\n\n"
                  "グラフィックドライバーを更新・再起動したときや、GPU に負荷をかけ過ぎたとき、高温になったときに、"
                  "Windows が GPU をリセットすると起きます。ビューワを起動し直してください。"
                  "何度も起きる場合は、グラフィックドライバーを最新にするか、PC を再起動してください。"
                  "詳しい理由はログ（" + log_path + "）にあります。"
                : "The viewer has to close: the GPU stopped responding.\n\n"
                  "This happens when Windows resets the GPU: after a graphics driver update or restart, or when the GPU "
                  "is overloaded or overheating. Please start the viewer again. If it keeps happening, update the graphics "
                  "driver or restart the computer. The log (" + log_path + ") has the details.";
        }
        if (why == LLRHI::VulkanUnavailable::SubmitFailed)
        {
            // fix-round6 1: the GPU did not take a frame's work (most often
            // out of GPU or system memory); what it carried is lost.
            return japanese
                ? "GPU が描画の処理を受け付けなかったため、ビューワを終了します。\n\n"
                  "多くは GPU のメモリかパソコンのメモリが足りなくなったときに起きます。"
                  "ほかのアプリを閉じるか、グラフィックの設定を軽いものにしてから、ビューワを起動し直してください。"
                  "何度も起きる場合は、グラフィックドライバーを最新にするか、PC を再起動してください。"
                  "詳しい理由はログ（" + log_path + "）にあります。"
                : "The viewer has to close: the GPU did not accept the work for a frame.\n\n"
                  "This most often happens when the GPU or the computer runs out of memory. Close other applications or "
                  "choose lighter graphics settings, then start the viewer again. If it keeps happening, update the graphics "
                  "driver or restart the computer. The log (" + log_path + ") has the details.";
        }
        if (why == LLRHI::VulkanUnavailable::SurfaceLost)
        {
            // The window lost its Vulkan surface, and a new one could not be made.
            return japanese
                ? "ウィンドウに映像を表示できなくなったため、ビューワを終了します。\n\n"
                  "グラフィックドライバーを更新・再起動したときや、ディスプレイをつなぎ替えたときに起きることがあります。"
                  "ビューワを起動し直してください。何度も起きる場合は、グラフィックドライバーを最新にするか、PC を再起動してください。"
                  "詳しい理由はログ（" + log_path + "）にあります。"
                : "The viewer has to close: its window can no longer display anything.\n\n"
                  "This can happen after a graphics driver update or restart, or when displays are reconnected. "
                  "Please start the viewer again. If it keeps happening, update the graphics driver or restart the computer. "
                  "The log (" + log_path + ") has the details.";
        }
        if (why == LLRHI::VulkanUnavailable::SetupFailed)
        {
            // research/90 G1: the GPU has what the renderer needs; the driver
            // could not set up the frame or show it in the window.
            return japanese
                ? "Vulkan の準備に失敗したため、ビューワを起動できません。\n\n原因:\n" + reason_list(reasons) +
                  "\nこの GPU は Vulkan に対応していますが、ドライバーが描画の準備やウィンドウへの表示を受け付けませんでした。"
                  "グラフィックドライバーを最新にすると直る場合があります。詳しい理由はログ（" + log_path + "）にあります。"
                : "This viewer cannot start: Vulkan could not be set up.\n\nCause:\n" + reason_list(reasons) +
                  "\nThis GPU supports Vulkan, but its driver could not set up rendering or show it in the window. "
                  "Updating the graphics driver may help. The log (" + log_path + ") has the details.";
        }
        if (why == LLRHI::VulkanUnavailable::MissingFeatures)
        {
            // research/124 V1: the GPU has Vulkan 1.3 but lacks a required
            // feature (VK_EXT_depth_clip_control and the others in
            // LLVKDevice's list). The GPU and the features are in the list.
            const std::string names = missing_feature_names(reasons, japanese ? "、" : ", ");
            const bool several = names.find(japanese ? "、" : ", ") != std::string::npos;
            std::string text = japanese
                ? "この PC の GPU は Vulkan の必須機能" + (names.empty() ? std::string() : "（" + names + "）") +
                      "に対応していないため、ビューワを起動できません。\n\n足りないもの:\n"
                : "This viewer cannot start: this computer's GPU does not support " +
                      std::string(several ? "required Vulkan features" : "a required Vulkan feature") +
                      (names.empty() ? std::string() : " (" + names + ")") + ".\n\nMissing:\n";
            text += reason_list(reasons);
            // Facts only (depthclip-wording): whether a GPU has the feature
            // depends on its driver, and some generations are unchecked (Intel
            // 6th to 10th generation, on Intel's legacy driver, may lack it), so
            // the guide below is a rough one and does not promise the feature.
            // The same guide as the README and research/82, 124.
            text += japanese
                ? std::string("\n") + (several ? "これらの機能" : "この機能") +
                      "に対応した最新のグラフィックドライバーが必要です。GPU のメーカーのサイトでドライバーを更新して、"
                      "それでもこの案内が出る場合は、お使いの GPU はこのビューワに対応していません。"
                      "OpenGL で動く他のビューワなど、お使いの PC に合ったビューワをお使いください。\n\n"
                      "対応する GPU の目安: NVIDIA GeForce GTX 10 番台以降、AMD Radeon RX 400 番台以降、"
                      "Intel 第 6 世代 Core 以降の内蔵 GPU（目安です。Intel 第 6〜10 世代など、確かめていない世代もあります）。"
                      "詳しい理由はログ（" + log_path + "）にあります。"
                : std::string("\nAn up-to-date graphics driver that supports ") + (several ? "these features" : "this feature") +
                      " is needed. Please update the graphics driver from the GPU maker's website. If this message still "
                      "appears after that, your GPU does not support this viewer: please use another viewer that suits "
                      "your computer, such as one that runs on OpenGL.\n\n"
                      "Rough guide to supported GPUs: NVIDIA GeForce GTX 10 series or later, AMD Radeon RX 400 series or later, "
                      "Intel 6th generation Core integrated graphics or later (some generations, such as Intel's 6th to 10th, "
                      "have not been checked). The log (" + log_path + ") has the details.";
            return text;
        }
        std::string text = japanese
            ? "このパソコンでは Vulkan で描画できないため、ビューワを起動できません。\n\n足りないもの:\n"
            : "This viewer cannot start: Vulkan rendering is not available on this computer.\n\nMissing:\n";
        text += reason_list(reasons);
        text += japanese
            ? "\nVulkan 1.3 に対応した GPU と新しいドライバーが必要です（目安: NVIDIA GeForce GTX 10 番台以降、"
              "AMD Radeon RX 400 番台以降、Intel 第 6 世代 Core 以降の内蔵 GPU。Intel 第 6〜10 世代など、確かめていない世代もあります）。"
              "グラフィックドライバーを更新すると直る場合があります。"
              "対応していない PC では、OpenGL で動く他のビューワなど、お使いの PC に合ったビューワをお使いください。"
            : "\nThe viewer needs a GPU and driver with Vulkan 1.3 (roughly: NVIDIA GeForce GTX 10 series or later, "
              "AMD Radeon RX 400 series or later, Intel 6th generation Core integrated graphics or later; some generations, "
              "such as Intel's 6th to 10th, have not been checked). Updating the graphics "
              "driver may help. On other computers, please use another viewer that suits your computer, such as one that "
              "runs on OpenGL.";
        return text;
    }

    // fix-round9b 4, fix-round10 3: added to the message when plus/gpudgc counted the loss (the
    // first start in a row) or turned DGC off (the second).
    std::string dgc_note(plus::gpudgc::Loss loss)
    {
        const bool japanese = LLUI::getLanguage().rfind("ja", 0) == 0;
        if (loss == plus::gpudgc::Loss::Counted)
        {
            return japanese
                ? "\n\nDGC（GPU が描画命令を作る機能）を使っていました。次の起動でも同じように終了した場合は、"
                  "DGC を安全のため自動でオフにします。"
                : "\n\nDGC (the GPU writes the draw commands) was in use. If the next start ends the same way, "
                  "DGC will be turned off to be safe.";
        }
        if (loss == plus::gpudgc::Loss::TurnedOff)
        {
            return japanese
                ? "\n\nDGC（GPU が描画命令を作る機能）を使っている時に 2 回続けてこの終了が起きたため、"
                  "DGC を安全のためオフにしました。Charis メニュー > 画質の詳細 で戻せます。"
                : "\n\nThis happened twice in a row with DGC (the GPU writes the draw commands) in use, so DGC has been "
                  "turned off to be safe. It can be turned back on in Charis > Graphics Details.";
        }
        return std::string();
    }

    void exit_with_message(LLRHI::VulkanUnavailable why, const std::string& reasons)
    {
        // fix-round9b 4, fix-round10 3: a GPU that DGC brings down would end every start: DGC
        // goes off when the device was lost with it in use in two starts in a row (the setting
        // is saved below). Not for a failed submit (most often out of memory).
        const plus::gpudgc::Loss dgc_loss = why == LLRHI::VulkanUnavailable::DeviceLost
                                                ? plus::gpudgc::onDeviceLost()
                                                : plus::gpudgc::Loss::NotCounted;
        if (why == LLRHI::VulkanUnavailable::DeviceLost || why == LLRHI::VulkanUnavailable::SurfaceLost ||
            why == LLRHI::VulkanUnavailable::SubmitFailed)
        {
            // fix-submitfail C: while running, fastQuit would drop what was
            // changed this session: it _exit()s, so LLAppViewer::cleanup()
            // saves nothing. The per-account settings too once logged in (as
            // cleanup() saves them only after a login that succeeded, which
            // reaches STATE_STARTED; fix-round9a).
            // A photo compare still held ends inside the save (compareEnd()): safe on a lost
            // device, as it gives back only the settings that rebuild nothing (the catalog's
            // rebuild 0: no shaders, pipeline or targets made), so the stopped GPU gets no work.
            plus::saveSettingsFile(gSavedSettings, gSavedSettings.getString("ClientSettingsFile"), true);
            const std::string account_file = gSavedSettings.getString("PerAccountSettingsFile");
            if (LLStartUp::getStartupState() == STATE_STARTED && !account_file.empty())
            {
                // What the AO's "quitting" would have written (a settings value only; its
                // inventory saving and the detached windows' "quitting" are left out here).
                if (plus::ao::AOEngine::instanceExists())
                {
                    plus::ao::AOEngine::instance().rememberCurrentState();
                }
                gSavedPerAccountSettings.setU32("LastLogoff", (U32)time_corrected());   // as cleanup() does
                plus::saveSettingsFile(gSavedPerAccountSettings, account_file, true);
            }
        }
        OSMessageBox(message(why, reasons) + dgc_note(dgc_loss), LLTrans::getString("APP_NAME"), OSMB_OK);
        LLAppViewer::instance()->fastQuit(1);   // like a failed window creation; does not return
    }

    void init()
    {
        LLRHI::sVulkanUnavailableExit = &exit_with_message;
        // The GL program binary cache gives GL linked programs without their
        // sources, which the Vulkan translation is built from (llvkgl's
        // glProgramBinary). Off for this run, not saved: like
        // --set RenderShaderCacheEnabled 0. The SPIR-V and pipeline caches
        // stand in for it.
        if (LLControlVariable* cache = gSavedSettings.getControl("RenderShaderCacheEnabled"))
        {
            cache->setValue(LLSD(false), false);
        }
    }

    plus::FeatureRegistration sRegistration({ "vkcheck", &init, {} });

    // The GPU string without what only tells the bus and CPU (NVIDIA's GL
    // driver ends its GL_RENDERER with "/PCIe/SSE2", older ones "/AGP",
    // "/PCI", "/SSE", "/3DNOW!"), lower case, spaces collapsed.
    std::string gpu_key(const std::string& gpu)
    {
        std::string key;
        for (char c : gpu)
        {
            const char lower = (char)tolower((unsigned char)c);
            if (isspace((unsigned char)lower))
            {
                if (!key.empty() && key.back() != ' ')
                {
                    key += ' ';
                }
            }
            else
            {
                key += lower;
            }
        }
        static const char* const suffixes[] = { "/pcie", "/pci", "/agp", "/sse2", "/sse", "/3dnow!" };
        bool stripped = true;
        while (stripped)
        {
            stripped = false;
            while (!key.empty() && key.back() == ' ')
            {
                key.pop_back();
            }
            for (const char* suffix : suffixes)
            {
                const size_t length = strlen(suffix);
                if (key.size() > length && key.compare(key.size() - length, length, suffix) == 0)
                {
                    key.resize(key.size() - length);
                    stripped = true;
                    break;
                }
            }
        }
        return key;
    }
}

bool plus::vkcheck::sameGPU(const std::string& a, const std::string& b)
{
    return a == b || gpu_key(a) == gpu_key(b);
}

void plus::vkcheck::keepSameGPU(const std::string& gpu)
{
    const std::string last = gSavedSettings.getString("LastGPUString");
    if (!last.empty() && last != gpu && sameGPU(last, gpu))
    {
        LL_INFOS("RenderInit") << "Same GPU as last time (\"" << last << "\" named as \"" << gpu
                               << "\"): graphics settings kept (research/117)" << LL_ENDL;
        gSavedSettings.setString("LastGPUString", gpu);
    }
}

bool plus::vkcheck::gpuBenchmark(float& gbps)
{
    if (!LLRHI::sGPUBenchmark)
    {
        return false;
    }
    const F32 measured = LLRHI::sGPUBenchmark();
    gbps = measured;
    if (measured <= 0.f)
    {
        LL_WARNS("RenderInit") << "GPU benchmark on Vulkan failed (research/117)" << LL_ENDL;
        return true;    // as gpu_benchmark's failures: class 0
    }

    // research/117 9.6: the same GPU measured again within BAND of what it
    // measured before keeps the higher of the two, so a result that wobbles
    // across a class line (the RTX 2080 measures 190-193 here, the line is
    // 192) does not change the class, its feature masks and the recommended
    // settings from start to start. A benchmark only ever comes out low
    // (clocks, other work, heat), never above what the GPU can do. Outside
    // the band (a new driver, another GPU, a real change) the new result
    // replaces it. PlusGPUBenchmarkGBps holds the raw Vulkan result (before
    // the scale below), so the scale is applied once, after this, and a
    // change of the scale never compounds with what was saved.
    constexpr F32 BAND = 0.08f;
    const std::string gpu = gGLManager.getRawGLString();
    const std::string saved_gpu = gSavedSettings.getString("PlusGPUBenchmarkGPU");
    const F32 saved = gSavedSettings.getF32("PlusGPUBenchmarkGBps");
    F32 raw = measured;
    if (saved > 0.f && !saved_gpu.empty() && sameGPU(saved_gpu, gpu) && fabsf(measured - saved) <= saved * BAND)
    {
        raw = llmax(saved, measured);
    }
    gSavedSettings.setString("PlusGPUBenchmarkGPU", gpu);
    gSavedSettings.setF32("PlusGPUBenchmarkGBps", raw);

    // research/117 9.7: LLFeatureManager's class lines (RenderClass1MemoryBandwidth
    // 48 x 1, 2, 4, 8) were set for GL's gpu_benchmark. The same draws timed
    // on Vulkan measure lower on NVIDIA: RTX 2080, driver 610.88, GL 201.0
    // GB/s; Vulkan, warmed up (clocks steady at SM 1890-1920 / memory 7000
    // MHz, nothing holding them), 190.45, 191.23, 193.03 over three starts:
    // GL / Vulkan = 1.05. So on NVIDIA the Vulkan result is scaled by 1.05
    // (PlusGPUBenchmarkVulkanScaleNVIDIA) and stays a GL-equivalent value.
    // AMD and Intel are not measured yet: no scale (PlusGPUBenchmarkVulkanScaleOther
    // 1) rather than a guess that could lift a GPU a class above what GL gave.
    const char* scale_setting = gGLManager.mIsNVIDIA ? "PlusGPUBenchmarkVulkanScaleNVIDIA" : "PlusGPUBenchmarkVulkanScaleOther";
    const F32 scale = gSavedSettings.getF32(scale_setting);
    gbps = scale > 0.f ? raw * scale : raw;
    LL_INFOS("RenderInit") << "GPU benchmark on Vulkan (research/117): measured " << measured << " GB/sec, kept from before "
                           << (saved > 0.f ? llformat("%.3f", saved) : std::string("none")) << " -> " << raw << ", x " << scale
                           << " (" << scale_setting << ") = " << gbps << " GB/sec as GL would measure it" << LL_ENDL;
    return true;
}
