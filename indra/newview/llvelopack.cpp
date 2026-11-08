/**
 * @file llvelopack.cpp
 * @brief Velopack installer and update framework integration
 *
 * $LicenseInfo:firstyear=2025&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2025, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#if LL_VELOPACK

#include "llviewerprecompiledheaders.h"
#include "llvelopack.h"
#include "llstring.h"
#include "llcorehttputil.h"
#include "llversioninfo.h"
#include "llversioninfovars.h" // LL_VIEWER_CHANNEL

#include <boost/json.hpp>
#include <fstream>
#include <unordered_map>
#include "llnotificationsutil.h"
#include "llviewercontrol.h"
#include "llappviewer.h"
#include "llcoros.h"
// <plus> research/311: Charis's package id, publisher and update source, the address checks,
// and putting back the secondlife:// registration at uninstall
#include "llhttpconstants.h"
#include "plus/independence/independence.h"
#include "plus/updatecheck/updateversion.h"
#include "plus/subtitle/subtitlehash.h"   // plus::subtitle::Sha256 (the Windows crypto API)
#if LL_WINDOWS
#include "plus/slurlhandler/slurlregistry.h"
#endif
// </plus>

#include "Velopack.h"

#if LL_WINDOWS
#include "llappviewerwin32.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <objbase.h>
#include <filesystem>
#include <propkey.h>
#include <propvarutil.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "shell32.lib")
#endif // LL_WINDOWS

// Common state
// <plus> research/311 section 2.3: no sUpdateUrl. The feed is fixed in the build
// (feed_base_url()); velopack_set_update_url() does not change it.
// How this exe was started, set once by velopack_initialize() on the main thread
// before any other thread runs; Unknown until then (counts as not installed). On
// macOS the folder is not looked at: Velopack, the official behaviour.
enum class EInstallKind { Unknown, Velopack, NotVelopack };
static EInstallKind sInstallKind = EInstallKind::Unknown;
// </plus>
static std::function<void(int)> sProgressCallback;
static vpkc_update_manager_t* sUpdateManager = nullptr;
static vpkc_update_info_t* sPendingUpdate = nullptr;        // Downloaded, ready to apply
static vpkc_update_info_t* sPendingCheckInfo = nullptr;     // Checked, awaiting user response
static vpkc_update_source_t* sUpdateSource = nullptr;
static LLNotificationPtr sDownloadingNotification;
static bool sRestartAfterUpdate = false;
static bool sIsRequired = false;                             // Is the pending check a required update?
static std::string sReleaseNotesUrl;
static std::string sTargetVersion;                           // Velopack's actual target version

// Forward declarations
static void show_required_update_prompt();
static void show_downloading_notification(const std::string& version);
static void ensure_update_manager(bool allow_downgrade);
static void velopack_download_pending_update();
static std::unordered_map<std::string, std::string> sAssetUrlMap; // basename -> original absolute URL

// <plus> research/311 section 2.3, research/262 section 5-5. The feed and the packages come
// from GitHub only: every address, the first one and each redirect (followed here, not by
// libcurl), must be https on a host of plus::independence::UPDATE_HOSTS. The feed names the
// packages' addresses, and those pass the same check. Nothing received changes where the
// feed is read from.
static constexpr int    UPDATE_MAX_REDIRECTS     = 5;                              // github.com -> tag -> asset host
static constexpr size_t UPDATE_MAX_FEED_BYTES    = size_t(4) * 1024 * 1024;        // releases.win.json: a few KB
static constexpr size_t UPDATE_MAX_PACKAGE_BYTES = size_t(1024) * 1024 * 1024;     // a full package: about 300 MB
static constexpr unsigned int UPDATE_PACKAGE_TRANSFER_TIMEOUT_S = 1200;            // the official value

static std::string feed_base_url()
{
    return std::string(plus::independence::PROJECT_URL) + "/releases/latest/download";
}

// The address without its query: the asset hosts put a short-lived signature there,
// which does not go into the log.
static std::string url_for_log(const std::string& url)
{
    return url.substr(0, url.find_first_of("?#"));
}

static bool is_update_url_allowed(const std::string& url)
{
    const std::string normal = plus::updatecheck::webURL(url);   // https (or http to this computer), no user name, ASCII
    static constexpr char HTTPS[] = "https://";
    constexpr size_t https_len = sizeof(HTTPS) - 1;
    if (normal.compare(0, https_len, HTTPS) != 0
        || !plus::updatecheck::hostInList(plus::updatecheck::urlHost(normal), plus::independence::UPDATE_HOSTS))
    {
        return false;
    }
    // No port, or the https one (the hosts in the list are names, never [IPv6] literals)
    const std::string authority = normal.substr(https_len, normal.find_first_of("/?#", https_len) - https_len);
    const size_t colon = authority.find(':');
    return colon == std::string::npos || authority.compare(colon, std::string::npos, ":443") == 0;
}

// The packages' addresses in the feed (before any redirect): this project's own releases
// only. github.com serves every repository's release files, and the host check alone
// would let a feed point at someone else's.
// No "..", '%', '?' or '#' either: libcurl removes dot segments ("/a/../b"), so
// ".../charis-viewer/releases/download/../../../other/..." would leave the project.
static bool is_project_release_url(const std::string& url)
{
    const std::string prefix = std::string(plus::independence::PROJECT_URL) + "/releases/download/";
    return url.compare(0, prefix.size(), prefix) == 0
        && url.find("..") == std::string::npos
        && url.find_first_of("%?#") == std::string::npos
        && is_update_url_allowed(url);
}

// The update feed and the packages are fetched only while certificates are checked: with
// NoVerifySSLCert on, the viewer's certificate callback accepts any certificate
// (LLBasicCertificateStore::validate), and an update is code that will run.
static bool update_certificates_checked()
{
    static LLCachedControl<bool> no_verify_ssl_cert(gSavedSettings, "NoVerifySSLCert", false);
    if (no_verify_ssl_cert())
    {
        LL_WARNS("Velopack") << "Not fetched: NoVerifySSLCert is on (certificates are not checked); "
                             << "updates are fetched only with it off" << LL_ENDL;
        return false;
    }
    return true;
}

// GETs url (a coroutine waits), following redirects to allowed addresses only.
// True with the adapter's result in result (the body: update_body(result), not
// copied: a package is hundreds of MB); false (a log line says why) for anything else.
static bool get_from_update_hosts(const std::string& url, const char* adapter_name,
                                  size_t max_bytes, unsigned int transfer_timeout_s, LLSD& result)
{
    if (!update_certificates_checked())
    {
        return false;
    }
    LLCore::HttpRequest::policy_t httpPolicy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
    auto httpAdapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>(adapter_name, httpPolicy);
    auto httpRequest = std::make_shared<LLCore::HttpRequest>();
    auto httpOpts = std::make_shared<LLCore::HttpOptions>();
    httpOpts->setFollowRedirects(false);
    httpOpts->setWantHeaders(true);   // the Location of a redirect (llcorehttp keeps no headers otherwise)
    httpOpts->setSSLVerifyPeer(true);
    httpOpts->setSSLVerifyHost(true);
    httpOpts->setMaxReplyBytes(max_bytes);
    if (transfer_timeout_s > 0)
    {
        httpOpts->setTransferTimeout(transfer_timeout_s);
    }
    // No LLSD Accept/Content-Type and no simulator port (the adapter adds them unless given;
    // libcurl leaves out a header whose value is empty).
    auto headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_ACCEPT, "*/*");
    headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "");
    headers->append("X-SecondLife-UDP-Listen-Port", "");

    std::string address = url;
    for (int redirects = 0; ; ++redirects)
    {
        if (!is_update_url_allowed(address))
        {
            LL_WARNS("Velopack") << "Not fetched: " << url_for_log(address) << " is not an https address on "
                                 << plus::independence::UPDATE_HOSTS << LL_ENDL;
            return false;
        }

        result = httpAdapter->getRawAndSuspend(httpRequest, address, httpOpts, headers);
        const LLSD& httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        const LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);
        if (status)
        {
            return true;
        }

        const S32 code = status.isHttpStatus() ? (S32)status.getType() : 0;
        const bool redirect = code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
        if (!redirect)
        {
            LL_WARNS("Velopack") << "HTTP request failed for " << url_for_log(address) << ": " << status.toString() << LL_ENDL;
            return false;
        }
        if (redirects >= UPDATE_MAX_REDIRECTS)
        {
            LL_WARNS("Velopack") << "Not fetched: more than " << UPDATE_MAX_REDIRECTS << " redirects from "
                                 << url_for_log(url) << LL_ENDL;
            return false;
        }
        // Checked at the top of the loop: a relative address is not a web address and stops here.
        address = httpResults[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_HEADERS][HTTP_IN_HEADER_LOCATION].asString();
        LL_DEBUGS("Velopack") << "Redirect " << code << " to " << url_for_log(address) << LL_ENDL;
    }
}

static const LLSD::Binary& update_body(const LLSD& result)
{
    return result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
}

// A package name from the feed, which becomes a file name in the temp folder and in
// Velopack's packages folder: "<VELOPACK_PACK_ID>-...nupkg" of letters, digits, '.', '-',
// '_', '+', no "..". The fixed start keeps out folders, drives and reserved names (CON...).
static bool is_package_file_name(const std::string& name)
{
    // Also guards the registry: an empty id makes Uninstall\<id> the whole list (get_pack_id())
    static_assert(sizeof(plus::independence::VELOPACK_PACK_ID) > 1, "the package id is not empty");
    static constexpr size_t MAX_NAME = 200;
    static constexpr char SUFFIX[] = ".nupkg";
    constexpr size_t suffix_len = sizeof(SUFFIX) - 1;
    const std::string prefix = std::string(plus::independence::VELOPACK_PACK_ID) + "-";
    if (name.size() <= prefix.size() + suffix_len || name.size() > MAX_NAME
        || name.compare(0, prefix.size(), prefix) != 0
        || name.find("..") != std::string::npos
        || name.compare(name.size() - suffix_len, suffix_len, SUFFIX) != 0)
    {
        return false;
    }
    for (char c : name)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '.' || c == '-' || c == '_' || c == '+';
        if (!ok)
        {
            return false;
        }
    }
    return true;
}
// </plus>

//
// Custom update source helpers
//

static std::string extract_basename(const std::string& url)
{
    // Strip query params / fragment
    std::string path = url;
    auto qpos = path.find('?');
    if (qpos != std::string::npos) path = path.substr(0, qpos);
    auto fpos = path.find('#');
    if (fpos != std::string::npos) path = path.substr(0, fpos);

    auto spos = path.rfind('/');
    if (spos != std::string::npos && spos + 1 < path.size())
        return path.substr(spos + 1);
    return path;
}

// <plus> research/311: every FileName is checked (a plain package name, and an address only
// of this project's releases); ok turns false at the first that is not, and the feed is dropped
static void rewrite_asset_urls(boost::json::value& jv, bool& ok)
// </plus>
{
    if (jv.is_object())
    {
        auto& obj = jv.as_object();
        auto it = obj.find("FileName");
        // <plus> research/311
        if (it != obj.end() && !it->value().is_string())
        {
            LL_WARNS("Velopack") << "Release feed: a FileName that is not text" << LL_ENDL;
            ok = false;
            return;
        }
        // </plus>
        if (it != obj.end() && it->value().is_string())
        {
            std::string filename(it->value().as_string());
            if (filename.find("://") != std::string::npos)
            {
                std::string basename = extract_basename(filename);
                // <plus> research/311
                if (!is_project_release_url(filename) || !is_package_file_name(basename))
                {
                    LL_WARNS("Velopack") << "Release feed: package address " << url_for_log(filename)
                                         << " is not a package of " << plus::independence::PROJECT_URL << "/releases" << LL_ENDL;
                    ok = false;
                    return;
                }
                // </plus>
                sAssetUrlMap[basename] = filename;
                it->value() = basename;
                LL_DEBUGS("Velopack") << "Rewrote FileName: " << basename << LL_ENDL;
            }
            // <plus> research/311
            else if (!is_package_file_name(filename))
            {
                LL_WARNS("Velopack") << "Release feed: package name \"" << filename << "\" is not a plain "
                                     << plus::independence::VELOPACK_PACK_ID << "-*.nupkg name" << LL_ENDL;
                ok = false;
                return;
            }
            // </plus>
        }
        for (auto& kv : obj)
        {
            rewrite_asset_urls(kv.value(), ok); // <plus> research/311 </plus>
        }
    }
    else if (jv.is_array())
    {
        for (auto& elem : jv.as_array())
        {
            rewrite_asset_urls(elem, ok); // <plus> research/311 </plus>
        }
    }
}

static std::string rewrite_release_feed(const std::string& json_str)
{
    boost::json::value jv = boost::json::parse(json_str);
    // <plus> research/311: a feed with any unacceptable FileName is not used at all
    sAssetUrlMap.clear();
    bool ok = true;
    rewrite_asset_urls(jv, ok);
    if (!ok)
    {
        sAssetUrlMap.clear();
        throw std::runtime_error("a package in the feed is not acceptable");
    }
    // </plus>
    return boost::json::serialize(jv);
}

static std::string download_url_raw(const std::string& url)
{
    // <plus> research/311: allowed hosts only, redirects checked one by one
    LLSD result;
    if (!get_from_update_hosts(url, "VelopackSource", UPDATE_MAX_FEED_BYTES, 0, result))
    {
        return {};
    }
    const LLSD::Binary& rawBody = update_body(result);
    // </plus>
    return std::string(rawBody.begin(), rawBody.end());
}

// <plus> research/311: expected_size and expected_sha256 (lower or upper case hex) are the
// feed's for the package; what arrived is compared with them before it is written
static bool download_url_to_file(const std::string& url, const std::string& local_path,
                                 uint64_t expected_size, const std::string& expected_sha256)
// </plus>
{
    // <plus> research/311: allowed hosts only, redirects checked one by one; the size and the
    // SHA-256 the feed gives are checked here (Velopack is handed a file that matches)
    LLSD result;
    if (!get_from_update_hosts(url, "VelopackDownload", UPDATE_MAX_PACKAGE_BYTES, UPDATE_PACKAGE_TRANSFER_TIMEOUT_S, result))
    {
        return false;
    }
    const LLSD::Binary& rawBody = update_body(result);
    if (rawBody.size() != expected_size)
    {
        LL_WARNS("Velopack") << "Downloaded package has " << rawBody.size() << " bytes, the feed says "
                             << expected_size << ": not used" << LL_ENDL;
        return false;
    }
    plus::subtitle::Sha256 sha;
    sha.add(rawBody.data(), rawBody.size());
    // Lower case hex, "" when it could not be computed. Sha256 uses the Windows crypto API:
    // on macOS it is always "" and every package is refused (to solve before a Mac release).
    const std::string got_sha256 = sha.hex();
    std::string want_sha256 = expected_sha256;
    LLStringUtil::toLower(want_sha256);
    if (got_sha256.empty() || got_sha256 != want_sha256)
    {
        LL_WARNS("Velopack") << "Downloaded package's SHA-256 " << (got_sha256.empty() ? "(not computed)" : got_sha256)
                             << " is not the feed's " << expected_sha256 << ": not used" << LL_ENDL;
        return false;
    }
    // </plus>
    llofstream outFile(local_path, std::ios::binary | std::ios::trunc);
    if (!outFile.is_open())
    {
        LL_WARNS("Velopack") << "Failed to open file for writing: " << local_path << LL_ENDL;
        return false;
    }
    outFile.write(reinterpret_cast<const char*>(rawBody.data()), rawBody.size());
    outFile.close();
    // <plus> research/311: a full disk leaves a cut-short file: not handed on
    if (outFile.fail())
    {
        LL_WARNS("Velopack") << "Failed to write the downloaded package to " << local_path << LL_ENDL;
        LLFile::remove(local_path);
        return false;
    }
    // </plus>
    return true;
}

//
// Custom source callbacks
//

static char* custom_get_release_feed(void* user_data, const char* releases_name)
{
    std::string base = feed_base_url(); // <plus> fixed in the build (research/311) </plus>
    if (!base.empty() && base.back() == '/')
        base.pop_back();
    std::string url = base + "/" + releases_name;
    LL_INFOS("Velopack") << "Fetching release feed: " << url << LL_ENDL;

    std::string json_str = download_url_raw(url);
    if (json_str.empty())
    {
        return nullptr;
    }

    try
    {
        std::string rewritten = rewrite_release_feed(json_str);
        char* result = static_cast<char*>(malloc(rewritten.size() + 1));
        if (result)
        {
            memcpy(result, rewritten.c_str(), rewritten.size() + 1);
        }
        return result;
    }
    catch (const std::exception& e)
    {
        LL_WARNS("Velopack") << "Failed to parse/rewrite release feed: " << e.what() << LL_ENDL;
        // <plus> research/311: no fallback to the unmodified feed, whose packages were not
        // checked: no feed, no update
        return nullptr;
        // </plus>
    }
}

static void custom_free_release_feed(void* user_data, char* feed)
{
    free(feed);
}

static std::string sPreDownloadedAssetPath;

static bool custom_download_asset(void* user_data,
                                   const vpkc_asset_t* asset,
                                   const char* local_path,
                                   size_t progress_callback_id)
{
    // The asset has already been downloaded at the coroutine level (before vpkc_download_updates).
    // This callback just copies the pre-downloaded file to where Velopack expects it.
    // We cannot use getRawAndSuspend here - coroutine context is lost through the Rust FFI boundary.
    if (sPreDownloadedAssetPath.empty())
    {
        LL_WARNS("Velopack") << "No pre-downloaded asset available" << LL_ENDL;
        return false;
    }

    std::string filename = asset->FileName ? asset->FileName : "";
    LL_INFOS("Velopack") << "Download asset callback: filename=" << filename
        << " local_path=" << local_path
        << " size=" << asset->Size << LL_ENDL;
    vpkc_source_report_progress(progress_callback_id, 0);

    std::ifstream src(sPreDownloadedAssetPath, std::ios::binary);
    llofstream dst(local_path, std::ios::binary | std::ios::trunc);
    if (!src.is_open() || !dst.is_open())
    {
        LL_WARNS("Velopack") << "Failed to open files for copy" << LL_ENDL;
        return false;
    }

    dst << src.rdbuf();
    dst.close();
    src.close();

    vpkc_source_report_progress(progress_callback_id, 100);
    LL_INFOS("Velopack") << "Asset copy complete" << LL_ENDL;
    return true;
}

//
// Platform-specific helpers and hooks
//

#if LL_WINDOWS

// <plus> research/311 section 2.3, research/152 section 2: the installer does not register
// secondlife:// or x-grid-location-info:// (the official viewer keeps them); no PROTOCOL_*,
// register_protocol_handler() or unregister_protocol_handler(). At uninstall,
// plus::slurlhandler::undo() puts back a registration the user made from this exe.
// </plus>
static std::wstring get_viewer_exe_name()
{
    return ll_convert<std::wstring>(gDirUtilp->getExecutableFilename());
}

static std::wstring get_install_dir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}

static std::wstring get_app_name()
{
    // Match viewer_manifest.py app_name() logic: release channel uses "Viewer"
    // suffix instead of "Release" for display purposes (shortcuts, uninstall, etc.)
    std::wstring channel = LL_TO_WSTRING(LL_VIEWER_CHANNEL);
    std::wstring release_suffix = L" Release";
    if (channel.size() >= release_suffix.size() &&
        channel.compare(channel.size() - release_suffix.size(), release_suffix.size(), release_suffix) == 0)
    {
        channel.replace(channel.size() - release_suffix.size(), release_suffix.size(), L" Viewer");
    }
    return channel;
}

static std::wstring get_app_name_oneword()
{
    std::wstring name = get_app_name();
    name.erase(std::remove(name.begin(), name.end(), L' '), name.end());
    return name;
}

// <plus> research/311
// UTF-8 (plus::independence's constants, Velopack's strings; null gives "") to UTF-16.
static std::wstring widen(const char* utf8)
{
    return ll_convert<std::wstring>(ll_safe_string(utf8));
}

// Velopack's package id, the one viewer_manifest.py gives vpk (both read
// plus::independence::VELOPACK_PACK_ID). Velopack's entry in the installed apps list is
// Uninstall\<id>. get_app_name_oneword() stays for the NSIS key.
static std::wstring get_pack_id()
{
    // An empty id would make Uninstall\<id> the whole list of installed apps (deleted below):
    // the static_assert in is_package_file_name() keeps it from building
    return widen(plus::independence::VELOPACK_PACK_ID);
}

// The entry Velopack writes itself (replaced by ours)
static std::wstring get_velopack_uninstall_key_path()
{
    return L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + get_pack_id();
}

static std::wstring get_uninstall_key_path()
{
    // Our own entry, apart from Velopack's (it is removed in register_uninstall_info())
    return L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Vlpk" + get_pack_id();
}

// Whether this exe is Velopack's <root>\current\<exe>: Setup.exe writes sq.version next to
// the exe and Update.exe one folder up (what Velopack's own locator looks for). The zip has
// neither, wherever it is unpacked.
static bool is_velopack_layout()
{
    const std::filesystem::path exe_dir(get_install_dir());
    std::error_code ec;
    return std::filesystem::is_regular_file(exe_dir / L"sq.version", ec)
        && std::filesystem::is_regular_file(exe_dir.parent_path() / L"Update.exe", ec);
}

// Whether the command line carries one of Velopack's hook options (--veloapp-install,
// --veloapp-updated, --veloapp-obsolete, --veloapp-uninstall, ...).
static bool has_velopack_hook_argument()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
    {
        return false;
    }
    bool found = false;
    for (int i = 1; i < argc && !found; ++i)
    {
        found = _wcsnicmp(argv[i], L"--veloapp-", 10) == 0;
    }
    LocalFree(argv);
    return found;
}
// </plus>

static std::wstring get_start_menu_path()
{
    wchar_t path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, path)))
    {
        return path;
    }
    return L"";
}

static std::wstring get_desktop_path()
{
    wchar_t path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, path)))
    {
        return path;
    }
    return L"";
}

static std::wstring get_app_user_model_id()
{
    // Format: CompanyName.ProductName
    // This ID is used for:
    // 1. Registry registration (HKCU\Software\Classes\Applications\{exe}\AppUserModelID)
    // 2. Shortcut property (IPropertyStore::SetValue with PKEY_AppUserModel_ID)
    // 3. Runtime process ID (SetCurrentProcessExplicitAppUserModelID if needed)
    // Must be consistent across all uses for proper taskbar grouping.
    // <plus> Charis groups and pins apart from the official viewer on the taskbar (research/114)
    return plus::independence::APP_USER_MODEL_ID;
    // </plus>
}

static HRESULT create_shortcut(const std::wstring& shortcut_path,
                            const std::wstring& target_path,
                            const std::wstring& arguments,
                            const std::wstring& description,
                            const std::wstring& icon_path)
{
    HRESULT hr = CoInitialize(NULL);
    if (FAILED(hr)) return hr;

    IShellLinkW* shell_link = nullptr;
    hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                          IID_IShellLinkW, (void**)&shell_link);
    if (SUCCEEDED(hr))
    {
        shell_link->SetPath(target_path.c_str());
        shell_link->SetArguments(arguments.c_str());
        shell_link->SetDescription(description.c_str());
        shell_link->SetIconLocation(icon_path.c_str(), 0);

        wchar_t work_dir[MAX_PATH];
        wcscpy_s(work_dir, target_path.c_str());
        PathRemoveFileSpecW(work_dir);
        shell_link->SetWorkingDirectory(work_dir);

        // Set AppUserModelID on the shortcut
        IPropertyStore* prop_store = nullptr;
        hr = shell_link->QueryInterface(IID_IPropertyStore, (void**)&prop_store);
        if (SUCCEEDED(hr))
        {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            std::wstring app_id = get_app_user_model_id();
            hr = InitPropVariantFromString(app_id.c_str(), &pv);
            if (SUCCEEDED(hr))
            {
                hr = prop_store->SetValue(PKEY_AppUserModel_ID, pv);
                PropVariantClear(&pv);
                if (SUCCEEDED(hr))
                {
                    HRESULT hr_commit = prop_store->Commit();
                    if (FAILED(hr_commit)) hr = hr_commit;
                }
            }
            prop_store->Release();
        }

        IPersistFile* persist_file = nullptr;
        hr = shell_link->QueryInterface(IID_IPersistFile, (void**)&persist_file);
        if (SUCCEEDED(hr))
        {
            hr = persist_file->Save(shortcut_path.c_str(), TRUE);
            persist_file->Release();
        }
        shell_link->Release();
    }

    CoUninitialize();
    return hr;
}

// <plus> research/311: removed register_protocol_handler() (the installer does not register secondlife://) </plus>
static void register_app_user_model_id(const std::wstring& exe_name)
{
    // Register AppUserModelID for the executable
    // This allows Windows to properly group taskbar items when users drag-and-drop
    // the exe to pin it, without requiring runtime calls to SetCurrentProcessExplicitAppUserModelID
    std::wstring key_path = L"SOFTWARE\\Classes\\Applications\\" + exe_name;
    HKEY hkey;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, key_path.c_str(), 0, NULL,
        REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hkey, NULL) == ERROR_SUCCESS)
    {
        std::wstring app_user_model_id = get_app_user_model_id();
        LSTATUS status = RegSetValueExW(hkey, L"AppUserModelID", 0, REG_SZ,
            (BYTE*)app_user_model_id.c_str(),
            (DWORD)((app_user_model_id.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hkey);

        if (status == ERROR_SUCCESS)
        {
            LL_DEBUGS("Velopack") << "Registered AppUserModelID: "
                << ll_convert_wide_to_string(app_user_model_id)
                << " for " << ll_convert_wide_to_string(exe_name) << LL_ENDL;
        }
        else
        {
            LL_WARNS("Velopack") << "Failed to set AppUserModelID (error " << status << ") for "
                << ll_convert_wide_to_string(exe_name) << LL_ENDL;
        }
    }
    else
    {
        LL_WARNS("Velopack") << "Failed to register AppUserModelID for "
            << ll_convert_wide_to_string(exe_name) << LL_ENDL;
    }
}

static bool get_shortcut_target(const std::wstring& lnk_path, std::wstring& target_path_str)
{
    // Resolve the shortcut to check its target
    wchar_t target_path[MAX_PATH] = { 0 };
    HRESULT hr = CoInitialize(NULL);
    bool res = false;
    if (SUCCEEDED(hr))
    {
        IShellLinkW* psl = nullptr;
        hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&psl);
        if (SUCCEEDED(hr))
        {
            IPersistFile* ppf = nullptr;
            hr = psl->QueryInterface(IID_IPersistFile, (void**)&ppf);
            if (SUCCEEDED(hr))
            {
                hr = ppf->Load(lnk_path.c_str(), STGM_READ);
                if (SUCCEEDED(hr))
                {
                    // Resolve() in theory may retarget to a different executable,
                    // but we should be fine as long as folder stays the same,
                    // otherwise we shouldn't clear it.
                    hr = psl->Resolve(NULL, SLR_NO_UI | SLR_NOUPDATE | SLR_ANY_MATCH);
                    if (SUCCEEDED(hr))
                    {
                        hr = psl->GetPath(target_path, MAX_PATH, nullptr, 0);
                        if (SUCCEEDED(hr))
                        {
                            target_path_str = std::wstring(target_path);
                            res = true;
                        }
                    }
                }
                ppf->Release();
            }
            psl->Release();
        }
        CoUninitialize();
    }
    return res;
}

static bool paths_are_equal(const std::wstring& path1, const std::wstring& path2)
{
    try
    {
        std::error_code ec1, ec2;
        std::filesystem::path p1(path1);
        std::filesystem::path p2(path2);

        // Get canonical (absolute, normalized) paths
        auto canonical1 = std::filesystem::canonical(p1, ec1);
        auto canonical2 = std::filesystem::canonical(p2, ec2);

        // If either path doesn't exist, fall back to string comparison
        if (ec1 || ec2)
        {
            // Normalize case for comparison
            std::wstring lower1 = path1;
            std::wstring lower2 = path2;
            std::transform(lower1.begin(), lower1.end(), lower1.begin(), ::towlower);
            std::transform(lower2.begin(), lower2.end(), lower2.begin(), ::towlower);
            while (!lower1.empty() && (lower1.back() == L'\\' || lower1.back() == L'/'))
            {
                lower1.pop_back();
            }
            while (!lower2.empty() && (lower2.back() == L'\\' || lower2.back() == L'/'))
            {
                lower2.pop_back();
            }
            return lower1 == lower2;
        }

        // Use filesystem::equivalent which handles all path variations
        return std::filesystem::equivalent(canonical1, canonical2, ec1);
    }
    catch (const std::filesystem::filesystem_error&)
    {
        // Fallback to case-insensitive string comparison
        std::wstring lower1 = path1;
        std::wstring lower2 = path2;
        std::transform(lower1.begin(), lower1.end(), lower1.begin(), ::towlower);
        std::transform(lower2.begin(), lower2.end(), lower2.begin(), ::towlower);
        while (!lower1.empty() && (lower1.back() == L'\\' || lower1.back() == L'/'))
        {
            lower1.pop_back();
        }
        while (!lower2.empty() && (lower2.back() == L'\\' || lower2.back() == L'/'))
        {
            lower2.pop_back();
        }
        return lower1 == lower2;
    }
}

static void update_taskbar_shortcut(const std::string& nsis_folder_path, const std::wstring& appdata_path, const std::wstring& app_name, const std::wstring& link_name)
{
    std::wstring taskbar_path = appdata_path;
    taskbar_path += L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar\\";
    taskbar_path += link_name;

    if (!PathFileExistsW(taskbar_path.c_str()))
    {
        // User didn't create one
        return;
    }

    // Verify we are removing or updating the right thing.
    // It is not warranteed that the shortcut points to NSIS.
    std::wstring target_path;
    if (get_shortcut_target(taskbar_path, target_path))
    {
        // Strip the filename part to get just the folder path
        std::filesystem::path trim_path(target_path);
        if (trim_path.has_filename())
        {
            target_path = trim_path.parent_path().wstring();
        }
        std::wstring nsis_path_w = ll_convert<std::wstring>(nsis_folder_path);
        if (!paths_are_equal(nsis_path_w, target_path))
        {
            // Shortcut points to something else, don't mess with it
            LL_INFOS("Velopack") << "Found a matching shortcut, but it points to a different channel. Expected: "
                << ll_convert_wide_to_string(nsis_path_w)
                << ", actual: " << ll_convert_wide_to_string(target_path)
                << LL_ENDL;
            return;
        }
    }

    // First try to overwrite shortcut
    std::wstring current_exe = ll_convert<std::wstring>(gDirUtilp->getExecutablePathAndName());
    HRESULT hr = create_shortcut(taskbar_path, current_exe, L"", app_name, current_exe);
    if (SUCCEEDED(hr))
    {
        LL_INFOS("Velopack") << "Successfully updated taskbar shortcut to point to current exe" << LL_ENDL;
        return;
    }

    // Try deleting and if possible, recreating separately. We should not leave hanging shortcuts behind.
    if (!DeleteFileW(taskbar_path.c_str()))
    {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND)
        {
            LL_WARNS("Velopack") << "Failed to delete NSIS taskbar shortcut: "
                << ll_convert_wide_to_string(taskbar_path)
                << " (error: " << error << ")" << LL_ENDL;
        }
    }
    else
    {
        // Deleted user created link, recreate it with new target if possible.
        LL_INFOS("Velopack") << "Updating taskbar shortcut to point to current exe" << LL_ENDL;
        HRESULT hr = create_shortcut(taskbar_path, current_exe, L"", app_name, current_exe);
        if (FAILED(hr))
        {
            LL_WARNS("Velopack") << "Failed to re-create taskbar shortcut, error: " << std::hex << hr << ", NSIS shortcut was removed" << LL_ENDL;
        }
    }
}

void clear_nsis_links(const std::string& nsis_folder_path)
{
    wchar_t path[MAX_PATH];
    std::wstring app_name = get_app_name();

    // 1. The 'start' shortcuts set by nsis would be global, like app shortcut:
    // C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Second Life Viewer\Second Life Viewer.lnk
    // But it isn't just one link, it's a whole directory that needs to be removed.
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_PROGRAMS, NULL, 0, path)))
    {
        std::wstring start_menu_path = path;
        std::wstring folder_path   = start_menu_path + L"\\" + app_name;

        std::error_code ec;
        std::filesystem::path dir(folder_path);
        if (std::filesystem::exists(dir, ec))
        {
            std::filesystem::remove_all(dir, ec);
            if (ec)
            {
                LL_WARNS("Velopack") << "Failed to remove NSIS start menu directory: "
                    << ll_convert_wide_to_string(folder_path) << LL_ENDL;
            }
        }
    }

    // 2. Desktop link, also a global one.
    // C:\Users\Public\Desktop\Second Life Viewer.lnk
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_DESKTOPDIRECTORY, NULL, 0, path)))
    {
        std::wstring desktop_path = path;
        std::wstring shortcut_path = desktop_path + L"\\" + app_name + L".lnk";
        if (!DeleteFileW(shortcut_path.c_str()))
        {
            DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND)
            {
                LL_WARNS("Velopack") << "Failed to delete NSIS desktop shortcut: "
                    << ll_convert_wide_to_string(shortcut_path)
                    << " (error: " << error << ")" << LL_ENDL;
            }
        }
    }

    // 3. Taskbar links, which are user-specific and located at:
    // %AppData%\Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar\
    // Note that it can be a link to velopack already or to a different NSIS viewer.
    // Name might also be different based on method of creation.
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, path)))
    {
        update_taskbar_shortcut(nsis_folder_path, path, app_name, L"Second Life.lnk"); // Window class name
        update_taskbar_shortcut(nsis_folder_path, path, app_name, L"Second Life(1).lnk"); // Just in case user somehow did it twice and removed first
        update_taskbar_shortcut(nsis_folder_path, path, app_name, L"SecondLifeViewer.lnk"); // Executable name
        update_taskbar_shortcut(nsis_folder_path, path, app_name, L"secondlife-bin.lnk"); // Debug builds
        update_taskbar_shortcut(nsis_folder_path, path, app_name, app_name + L".lnk"); // Default name
    }
}

static void parse_version(const wchar_t* version_str, int& major, int& minor, int& patch, uint64_t& build)
{
    major = minor = patch = 0;
    build = 0;
    if (!version_str) return;
    // Use swscanf for wide strings
    swscanf(version_str, L"%d.%d.%d.%llu", &major, &minor, &patch, &build);
}

bool get_nsis_version(
    int& nsis_major,
    int& nsis_minor,
    int& nsis_patch,
    uint64_t& nsis_build,
    std::string& nsis_path)
{
    // <plus> research/311: only a Velopack installation takes over from an NSIS one; the zip
    // leaves other installations (and their shortcuts) alone
    if (!velopack_is_installed())
    {
        return false;
    }
    // </plus>
    // Test for presence of NSIS viewer registration, then
    // attempt to read uninstall info
    std::wstring app_name_oneword = get_app_name_oneword();
    std::wstring uninstall_key_path = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + app_name_oneword;
    HKEY hkey;
    LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, uninstall_key_path.c_str(), 0, KEY_READ, &hkey);
    if (result != ERROR_SUCCESS)
    {
        return false;
    }

    // Read DisplayVersion
    wchar_t version_buf[64] = { 0 };
    DWORD version_buf_size = sizeof(version_buf);
    DWORD type = 0;
    LONG ver_rv = RegGetValueW(hkey, nullptr, L"DisplayVersion", RRF_RT_REG_SZ, &type, version_buf, &version_buf_size);

    if (ver_rv != ERROR_SUCCESS)
    {
        RegCloseKey(hkey);
        return false;
    }

    parse_version(version_buf, nsis_major, nsis_minor, nsis_patch, nsis_build);

    // Make sure it actually exists and not a dead entry.
    wchar_t path_buffer[MAX_PATH] = { 0 };
    DWORD path_buf_size = sizeof(path_buffer);
    LONG rv = RegGetValueW(hkey, nullptr, L"UninstallString", RRF_RT_REG_SZ, &type, path_buffer, &path_buf_size);
    RegCloseKey(hkey);
    if (rv != ERROR_SUCCESS)
    {
        return false;
    }
    size_t len = wcslen(path_buffer);
    if (len > 0)
    {
        if (path_buffer[0] == L'\"')
        {
            // Likely to contain leading "
            memmove(path_buffer, path_buffer + 1, len * sizeof(wchar_t));
        }
        wchar_t* pos = wcsstr(path_buffer, L"uninst.exe");
        if (pos)
        {
            // Likely to contain trailing "
            pos[wcslen(L"uninst.exe")] = L'\0';
        }
    }
    std::error_code ec;
    std::filesystem::path path(path_buffer);
    // check if uninstaller exists
    if (!std::filesystem::exists(path, ec))
    {
        return false;
    }

    // We found a valid NSIS installation, trim the path to get the folder
    wchar_t* pos = wcsstr(path_buffer, L"uninst.exe");
    if (pos)
    {
        // Trim uninst.exe from the path by setting null terminator before it
        *pos = L'\0';
    }
    // Note that it has a trailing backslash.
    nsis_path = ll_convert_wide_to_string(path_buffer);

    return true;
}

// <plus> research/311: removed unregister_protocol_handler() (it deleted any viewer's secondlife://) </plus>
static void unregister_app_user_model_id(const std::wstring& exe_name)
{
    // <plus> research/311: an empty name would delete all of HKCU\Software\Classes\Applications
    if (exe_name.empty())
    {
        return;
    }
    // </plus>
    std::wstring key_path = L"SOFTWARE\\Classes\\Applications\\" + exe_name;
    RegDeleteTreeW(HKEY_CURRENT_USER, key_path.c_str());
}

static void register_uninstall_info(const std::wstring& install_dir,
                                    const std::wstring& app_name,
                                    const std::wstring& version)
{
    // <plus> research/311 section 2.3: the package id (as given to vpk), not the channel
    // Clears velopack's recently created 'uninstall' registry entry.
    // We are going to use a custom one.
    // Note that velopack doesn't know about our custom entry.
    std::wstring key_path = get_velopack_uninstall_key_path();
    RegDeleteTreeW(HKEY_CURRENT_USER, key_path.c_str());
    // Use a unique key name to avoid conflicts with any existing NSIS-based uninstall info,
    // which can cause only one of the two entries to show up in the Add/Remove Programs list.
    // The UI will show DisplayName, so the key name itself is not important to be user-friendly.
    key_path = get_uninstall_key_path();
    // </plus>
    HKEY hkey;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, key_path.c_str(), 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hkey, NULL) == ERROR_SUCCESS)
    {
        std::wstring exe_path = install_dir + L"\\" + get_viewer_exe_name();
        // Update.exe lives one level above the current\ directory where the viewer exe runs
        std::filesystem::path update_exe = std::filesystem::path(install_dir).parent_path() / L"Update.exe";
        std::wstring uninstall_cmd = L"\"" + update_exe.wstring() + L"\" --uninstall";

        RegSetValueExW(hkey, L"DisplayName", 0, REG_SZ,
                      (BYTE*)app_name.c_str(), (DWORD)((app_name.size() + 1) * sizeof(wchar_t)));
        RegSetValueExW(hkey, L"DisplayVersion", 0, REG_SZ,
                      (BYTE*)version.c_str(), (DWORD)((version.size() + 1) * sizeof(wchar_t)));
        // <plus> Charis's publisher (the viewer's name), not Linden Lab
        const std::wstring publisher = widen(plus::independence::PUBLISHER);
        RegSetValueExW(hkey, L"Publisher", 0, REG_SZ,
                      (BYTE*)publisher.c_str(), (DWORD)((publisher.size() + 1) * sizeof(wchar_t)));
        // </plus>
        RegSetValueExW(hkey, L"UninstallString", 0, REG_SZ,
                      (BYTE*)uninstall_cmd.c_str(), (DWORD)((uninstall_cmd.size() + 1) * sizeof(wchar_t)));
        RegSetValueExW(hkey, L"DisplayIcon", 0, REG_SZ,
                      (BYTE*)exe_path.c_str(), (DWORD)((exe_path.size() + 1) * sizeof(wchar_t)));

        // <plus> research/311 section 2.3: Linden Lab's support is not Charis's. No HelpLink (no
        // support contact yet, research/227 F6); the project page and its releases instead.
        std::wstring link_url = widen(plus::independence::PROJECT_URL);
        RegSetValueExW(hkey, L"URLInfoAbout", 0, REG_SZ,
            (BYTE*)link_url.c_str(), (DWORD)((link_url.size() + 1) * sizeof(wchar_t)));

        link_url += L"/releases";
        RegSetValueExW(hkey, L"URLUpdateInfo", 0, REG_SZ,
            (BYTE*)link_url.c_str(), (DWORD)((link_url.size() + 1) * sizeof(wchar_t)));
        // </plus>

        DWORD no_modify = 1;
        RegSetValueExW(hkey, L"NoModify", 0, REG_DWORD, (BYTE*)&no_modify, sizeof(DWORD));
        RegSetValueExW(hkey, L"NoRepair", 0, REG_DWORD, (BYTE*)&no_modify, sizeof(DWORD));

        // Format YYYYMMDD
        wchar_t dateStr[9];
        time_t t = time(NULL);
        struct tm tm;
        localtime_s(&tm, &t);
        wcsftime(dateStr, 9, L"%Y%m%d", &tm);
        RegSetValueExW(hkey, L"InstallDate", 0, REG_SZ, (BYTE*)dateStr, (DWORD)((wcslen(dateStr) + 1) * sizeof(wchar_t))); // Let Windows fill in the install date

        // 800 MB, inaccurate, but for a rough idea.
        // We can check folder size here, but it would take time and
        // information is of low importance.
        DWORD estimated_size = 800000;
        RegSetValueExW(hkey, L"EstimatedSize", 0, REG_DWORD, (BYTE*)&estimated_size, sizeof(DWORD));

        RegCloseKey(hkey);
    }
}

static void unregister_uninstall_info()
{
    std::wstring key_path = get_uninstall_key_path(); // <plus> the package id (research/311) </plus>
    RegDeleteTreeW(HKEY_CURRENT_USER, key_path.c_str());
}

static void create_shortcuts(
    const std::wstring& install_dir,
    const std::wstring& app_name)
{
    std::wstring exe_path = install_dir + L"\\" + get_viewer_exe_name();
    std::wstring start_menu_dir = get_start_menu_path() + L"\\" + app_name;
    std::wstring desktop_path = get_desktop_path();

    CreateDirectoryW(start_menu_dir.c_str(), NULL);

    create_shortcut(start_menu_dir + L"\\" + app_name + L".lnk",
                    exe_path, L"", app_name, exe_path);

    create_shortcut(desktop_path + L"\\" + app_name + L".lnk",
                    exe_path, L"", app_name, exe_path);
}

// <plus> research/311 section 2.5: a shortcut of the same name may be the user's own, to an
// unpacked zip in another folder: only one to an exe in install_dir is removed.
static void remove_shortcut_if_ours(const std::wstring& lnk_path, const std::wstring& install_dir)
{
    std::wstring target_path;
    if (!get_shortcut_target(lnk_path, target_path))
    {
        return;   // not there, or not readable: left as it is
    }
    if (!paths_are_equal(std::filesystem::path(target_path).parent_path().wstring(), install_dir))
    {
        // OutputDebugString: the hooks run before the viewer's log is set up (LL_INFOS goes nowhere)
        OutputDebugStringA("[Velopack] a shortcut of the same name is for another folder; left\n");
        return;
    }
    DeleteFileW(lnk_path.c_str());
}
// </plus>

static void remove_shortcuts(const std::wstring& app_name)
{
    std::wstring start_menu_dir = get_start_menu_path() + L"\\" + app_name;
    std::wstring desktop_path = get_desktop_path();
    const std::wstring install_dir = get_install_dir(); // <plus> research/311 </plus>

    remove_shortcut_if_ours(start_menu_dir + L"\\" + app_name + L".lnk", install_dir); // <plus> only ours </plus>
    RemoveDirectoryW(start_menu_dir.c_str());
    remove_shortcut_if_ours(desktop_path + L"\\" + app_name + L".lnk", install_dir); // <plus> only ours </plus>
}

static void on_first_run(void* p_user_data, const char* app_version)
{
    // Velopack first executes 'after install' hook, then writes registry,
    // then executes 'on first run' hook.
    // As we need to clear velopack's 'uninstall' registry entry and use
    // our own, clean it here instead of on_after_install.

    std::wstring install_dir = get_install_dir();
    std::wstring app_name = get_app_name();

    // <plus> research/311: as on_after_update() (the official conversion kept the NUL in the string)
    const std::wstring version = widen(app_version);
    // </plus>

    register_uninstall_info(install_dir, app_name, version);
    // Drop install related settings
    // Unfortunately gDirUtilp is not initialized yet and it's shouldn't
    // be possible to change location of the settings. For now it's simpler
    // to hardcode the location.
    std::optional<std::string> app_data = LLStringUtil::getoptenv("APPDATA");
    if (app_data)
    {
        // Strip trailing delimiter if present
        std::string app_data_path = *app_data;
        if (!app_data_path.empty() && (app_data_path.back() == '\\' || app_data_path.back() == '/'))
        {
            app_data_path.pop_back();
        }

        // <plus> our own settings, never the official viewer's (research/114)
        std::string user_settings_path = app_data_path + "\\" + plus::independence::APP_DIR_NAME + "\\user_settings\\settings.xml";
        // </plus>
        LLControlGroup settings("global");
        if (settings.loadFromFile(user_settings_path))
        {
            // If user reinstalls or updates, we want to recheck for nsis leftovers.
            if (settings.controlExists("PreviousInstallChecked"))
            {
                settings.setBOOL("PreviousInstallChecked", false);
            }
            settings.saveToFile(user_settings_path, true);
        }
    }
}

// <plus> research/311: after an update the entry in the installed apps list shows the new
// version, and an entry Velopack may have written again is replaced by ours (as at first run).
static void on_after_update(void* user_data, const char* app_version)
{
    register_uninstall_info(get_install_dir(), get_app_name(), widen(app_version));
}

// At every normal start of the installed viewer (after Velopack's hooks): should Velopack
// have written its own entry again after on_after_update() (only the first run replaces it
// otherwise), ours takes its version and Velopack's is removed: the list keeps one entry.
// Nothing to do when Velopack's entry is not there (the usual case).
static void replace_velopack_uninstall_entry()
{
    const std::wstring key_path = get_velopack_uninstall_key_path();
    HKEY hkey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key_path.c_str(), 0, KEY_READ, &hkey) != ERROR_SUCCESS)
    {
        return;
    }
    wchar_t version[64] = { 0 };
    DWORD version_size = sizeof(version);
    const LONG rv = RegGetValueW(hkey, nullptr, L"DisplayVersion", RRF_RT_REG_SZ, nullptr, version, &version_size);
    RegCloseKey(hkey);
    if (rv == ERROR_SUCCESS && version[0])
    {
        register_uninstall_info(get_install_dir(), get_app_name(), version);   // removes Velopack's too
        // OutputDebugString: this runs at the top of wWinMain, before the viewer's log is set up
        OutputDebugStringA("[Velopack] Velopack's entry in the installed apps list replaced by ours (its version)\n");
    }
    else
    {
        RegDeleteTreeW(HKEY_CURRENT_USER, key_path.c_str());   // ours keeps the version it has
        OutputDebugStringA("[Velopack] Velopack's entry in the installed apps list removed (it had no version; ours kept)\n");
    }
}
// </plus>

static void on_after_install(void* user_data, const char* app_version)
{
    std::wstring install_dir = get_install_dir();
    std::wstring app_name = get_app_name();
    std::wstring exe_name = get_viewer_exe_name();

    // <plus> no secondlife:// registration (research/311 section 2.2): the user makes Charis
    // the program for the links in Preferences if they want (plus/slurlhandler)
    // </plus>

    // Register AppUserModelID for taskbar pinning support
    register_app_user_model_id(exe_name);

    create_shortcuts(install_dir, app_name);
}

static void on_before_uninstall(void* user_data, const char* app_version)
{
    std::wstring app_name = get_app_name();
    std::wstring exe_name = get_viewer_exe_name();

    // <plus> research/311 section 2.2: the installer never registered the links. A registration
    // the user made from this exe is put back to what it was (only while it still names this
    // exe: another viewer's is left alone).
    plus::slurlhandler::undo();
    // </plus>
    unregister_app_user_model_id(exe_name);
    remove_shortcuts(app_name);

    std::wstring install_dir = get_install_dir();
    LLAppViewerWin32::sendShutdownToOtherInstances(install_dir);

    unregister_uninstall_info();
}

static void on_log_message(void* user_data, const char* level, const char* message)
{
    OutputDebugStringA("[Velopack] ");
    OutputDebugStringA(level);
    OutputDebugStringA(": ");
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
}

static void set_runtime_app_user_model_id()
{
    // Ensure taskbar grouping uses a stable ID independent of exe path
    // (wrapper exe vs current\ exe).
    const std::wstring app_user_model_id = get_app_user_model_id();
    HRESULT appid_hr = SetCurrentProcessExplicitAppUserModelID(app_user_model_id.c_str());
    if (FAILED(appid_hr))
    {
        LL_WARNS("Velopack") << "SetCurrentProcessExplicitAppUserModelID failed: 0x"
            << std::hex << appid_hr << LL_ENDL;
    }
}

#elif LL_DARWIN

// macOS-specific hooks
// TODO: Implement protocol handler registration via Launch Services
// TODO: Implement app bundle management

static void on_first_run(void* user_data, const char* app_version)
{
}

static void on_after_install(void* user_data, const char* app_version)
{
    // macOS handles protocol registration via Info.plist CFBundleURLTypes
    // No additional registration needed at runtime
    LL_INFOS("Velopack") << "macOS post-install hook called for version: " << app_version << LL_ENDL;
}

static void on_before_uninstall(void* user_data, const char* app_version)
{
    LL_INFOS("Velopack") << "macOS pre-uninstall hook called for version: " << app_version << LL_ENDL;
}

static void on_log_message(void* user_data, const char* level, const char* message)
{
    LL_INFOS("Velopack") << "[" << level << "] " << message << LL_ENDL;
}

#endif // LL_WINDOWS / LL_DARWIN

//
// Common progress callback
//

static void on_progress(void* user_data, size_t progress)
{
    if (sProgressCallback)
    {
        sProgressCallback(static_cast<int>(progress));
    }
}

static void on_vpk_log(void* p_user_data,
    const char* psz_level,
    const char* psz_message)
{
    LL_DEBUGS("Velopack") << ll_safe_string(psz_message) << LL_ENDL;
}

//
// Version comparison helper
//

// Compare running version against a VVM version string "major.minor.patch.build".
// Returns -1 if running < vvm, 0 if equal, 1 if running > vvm.
static int compare_running_version(const std::string& vvm_version)
{
    S32 major = 0, minor = 0, patch = 0;
    U64 build = 0;
    sscanf(vvm_version.c_str(), "%d.%d.%d.%llu", &major, &minor, &patch, &build);

    const LLVersionInfo& vi = LLVersionInfo::instance();
    S32 cur_major = vi.getMajor();
    S32 cur_minor = vi.getMinor();
    S32 cur_patch = vi.getPatch();
    U64 cur_build = vi.getBuild();

    if (cur_major != major) return cur_major < major ? -1 : 1;
    if (cur_minor != minor) return cur_minor < minor ? -1 : 1;
    if (cur_patch != patch) return cur_patch < patch ? -1 : 1;
    if (cur_build != build) return cur_build < build ? -1 : 1;
    return 0;
}

//
// Update manager lifecycle
//

static void ensure_update_manager(bool allow_downgrade)
{
    if (sUpdateManager)
        return;
    // <plus> research/311: no manager (no feed fetched) for the zip
    if (!velopack_is_installed())
        return;
    // </plus>

    vpkc_update_options_t options = {};
    options.AllowVersionDowngrade = allow_downgrade;
    options.ExplicitChannel = nullptr;

    if (!sUpdateSource)
    {
        sUpdateSource = vpkc_new_source_custom_callback(
            custom_get_release_feed,
            custom_free_release_feed,
            custom_download_asset,
            nullptr);
    }

    vpkc_locator_config_t* locator_ptr = nullptr;

#if LL_DARWIN
    // Try auto-detection first (works when the app bundle was packaged by vpk
    // and has UpdateMac + sq.version already present)
    if (!vpkc_new_update_manager_with_source(sUpdateSource, &options, nullptr, &sUpdateManager))
    {
        char err[512];
        vpkc_get_last_error(err, sizeof(err));
        LL_INFOS("Velopack") << "Auto-detect failed (" << ll_safe_string(err)
                             << "), falling back to explicit locator" << LL_ENDL;

        // Auto-detection failed - construct an explicit locator.
        // This handles legacy DMG installs that don't have Velopack's
        // install state (UpdateMac, sq.version) in the bundle.
        vpkc_locator_config_t locator = {};

        // The executable lives at <bundle>/Contents/MacOS/<exe>
        // The app bundle root is two levels up from the executable directory.
        std::string exe_dir = gDirUtilp->getExecutableDir();
        std::string bundle_root = exe_dir + "/../..";
        char resolved[PATH_MAX];
        if (realpath(bundle_root.c_str(), resolved))
        {
            bundle_root = resolved;
        }

        // Construct a version string in Velopack SemVer format: major.minor.patch-build
        const LLVersionInfo& vi = LLVersionInfo::instance();
        std::string current_version = llformat("%d.%d.%d-%llu",
            vi.getMajor(), vi.getMinor(), vi.getPatch(), vi.getBuild());

        // Create a minimal sq.version manifest so Velopack knows our version.
        // Proper vpk-packaged builds have this in the bundle already.
        std::string manifest_path = gDirUtilp->getExpandedFilename(LL_PATH_TEMP, "sq.version");
        {
            std::string app_name = LLVersionInfo::instance().getChannel();
            std::string pack_id = app_name;
            pack_id.erase(std::remove(pack_id.begin(), pack_id.end(), ' '), pack_id.end());

            std::string nuspec = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                "<package xmlns=\"http://schemas.microsoft.com/packaging/2010/07/nuspec.xsd\">\n"
                "  <metadata>\n"
                "    <id>" + pack_id + "</id>\n"
                "    <version>" + current_version + "</version>\n"
                "    <title>" + app_name + "</title>\n"
                "  </metadata>\n"
                "</package>\n";

            llofstream manifest_file(manifest_path, std::ios::trunc);
            if (manifest_file.is_open())
            {
                manifest_file << nuspec;
                manifest_file.close();
            }
        }

        std::string packages_dir = gDirUtilp->getExpandedFilename(LL_PATH_TEMP, "velopack-packages");
        LLFile::mkdir(packages_dir);

        locator.RootAppDir = const_cast<char*>(bundle_root.c_str());
        locator.CurrentBinaryDir = const_cast<char*>(exe_dir.c_str());
        locator.ManifestPath = const_cast<char*>(manifest_path.c_str());
        locator.PackagesDir = const_cast<char*>(packages_dir.c_str());
        locator.UpdateExePath = nullptr;
        locator.IsPortable = false;

        locator_ptr = &locator;

        LL_INFOS("Velopack") << "Explicit locator: RootAppDir=" << bundle_root
                             << " CurrentBinaryDir=" << exe_dir
                             << " Version=" << current_version << LL_ENDL;

        if (!vpkc_new_update_manager_with_source(sUpdateSource, &options, locator_ptr, &sUpdateManager))
        {
            char err2[512];
            vpkc_get_last_error(err2, sizeof(err2));
            LL_WARNS("Velopack") << "Failed to create update manager: " << ll_safe_string(err2) << LL_ENDL;
        }
    }
    return;
#endif

    // Windows: Velopack auto-detection works because the viewer is installed
    // by Velopack's Setup.exe which creates the proper install structure.
    if (!vpkc_new_update_manager_with_source(sUpdateSource, &options, nullptr, &sUpdateManager))
    {
        char err[512];
        vpkc_get_last_error(err, sizeof(err));
        LL_WARNS("Velopack") << "Failed to create update manager: " << ll_safe_string(err) << LL_ENDL;
    }
}

//
// Public API - Cross-platform
//

bool velopack_initialize()
{
#if LL_WINDOWS
    // Needs to be called on each run for consistent taskbar behavior.
    set_runtime_app_user_model_id();
    // <plus> research/311 section 2.2: the zip (unpacked by hand) is not Velopack's. Velopack
    // is not run at all then: no hook from the command line (a --veloapp-uninstall would
    // remove the installed copy's shortcuts and entry), no update applied at start, and
    // nothing here looks for updates (velopack_is_installed()). The same exe from Setup.exe
    // runs everything below.
    sInstallKind = is_velopack_layout() ? EInstallKind::Velopack : EInstallKind::NotVelopack;
    if (sInstallKind != EInstallKind::Velopack)
    {
        // A hook's command line (--veloapp-*) is Setup.exe's or Update.exe's, never a user's:
        // the zip does nothing for it, and does not start the viewer either.
        if (has_velopack_hook_argument())
        {
            OutputDebugStringA("[Velopack] a Velopack hook for a viewer that is not a Velopack installation: exiting\n");
            return false;
        }
        OutputDebugStringA("[Velopack] not a Velopack installation (no sq.version and ..\\Update.exe): Velopack not run\n");
        return true;
    }
#else
    sInstallKind = EInstallKind::Velopack;   // macOS: as the official viewer
    // </plus>
#endif

    vpkc_set_logger(on_log_message, nullptr);
    vpkc_app_set_auto_apply_on_startup(false);

#if LL_WINDOWS || LL_DARWIN
    vpkc_app_set_hook_first_run(on_first_run);
    vpkc_app_set_hook_after_install(on_after_install);
    vpkc_app_set_hook_before_uninstall(on_before_uninstall);
#endif
    // <plus> research/311: the entry in the installed apps list follows an update
#if LL_WINDOWS
    vpkc_app_set_hook_after_update(on_after_update);
#endif
    // </plus>

    vpkc_app_run(nullptr);
    // <plus> research/311: a hook ends the process inside vpkc_app_run(); this is a normal start
#if LL_WINDOWS
    replace_velopack_uninstall_entry();
#endif
    // </plus>
    return true;
}

// Downloads the update that was found during the check phase.
// Operates on sPendingCheckInfo which was set by velopack_check_for_updates.
static void velopack_download_pending_update()
{
    if (!sUpdateManager || !sPendingCheckInfo)
    {
        LL_WARNS("Velopack") << "No pending check info to download" << LL_ENDL;
        return;
    }

    LL_DEBUGS("Velopack") << "Setting up detailed logging";
    vpkc_set_logger(on_vpk_log, nullptr);
    LL_CONT << LL_ENDL;
    LL_INFOS("Velopack") << "Downloading update..." << LL_ENDL;

    // Pre-download the nupkg at the coroutine level where getRawAndSuspend works.
    // The download callback inside the Rust FFI cannot use coroutine HTTP.
    // <plus> research/311: the feed's package: a plain name (it becomes a file in the temp
    // folder) and a SHA-256 to compare the download with (vpk writes one for every package)
    const vpkc_asset_t* target = sPendingCheckInfo->TargetFullRelease;
    if (!target)
    {
        LL_WARNS("Velopack") << "Not downloaded: the update names no full package" << LL_ENDL;
        return;
    }
    const std::string target_sha256 = ll_safe_string(target->SHA256);
    // </plus>
    std::string asset_filename = sPendingCheckInfo->TargetFullRelease->FileName
        ? sPendingCheckInfo->TargetFullRelease->FileName : "";
    // <plus> research/311
    if (!is_package_file_name(asset_filename))
    {
        LL_WARNS("Velopack") << "Not downloaded: the feed names the package \"" << asset_filename
                             << "\", not a plain " << plus::independence::VELOPACK_PACK_ID << "-*.nupkg name" << LL_ENDL;
        return;
    }
    if (target_sha256.size() != 64)
    {
        LL_WARNS("Velopack") << "Not downloaded: the feed gives no SHA-256 for " << asset_filename << LL_ENDL;
        return;
    }
    // </plus>
    std::string asset_url;
    auto url_it = sAssetUrlMap.find(asset_filename);
    if (url_it != sAssetUrlMap.end())
    {
        asset_url = url_it->second;
    }
    else
    {
        std::string base = feed_base_url(); // <plus> fixed in the build (research/311) </plus>
        if (!base.empty() && base.back() == '/')
            base.pop_back();
        asset_url = base + "/" + asset_filename;
    }

    sPreDownloadedAssetPath = gDirUtilp->getExpandedFilename(LL_PATH_TEMP, asset_filename);
    LL_INFOS("Velopack") << "Pre-downloading " << asset_url
        << " to " << sPreDownloadedAssetPath << LL_ENDL;

    if (!download_url_to_file(asset_url, sPreDownloadedAssetPath, target->Size, target_sha256)) // <plus> checked against the feed </plus>
    {
        LL_WARNS("Velopack") << "Failed to pre-download update asset" << LL_ENDL;
        sPreDownloadedAssetPath.clear();
        return;
    }

    LL_INFOS("Velopack") << "Pre-download complete, handing to Velopack" << LL_ENDL;
    if (vpkc_download_updates(sUpdateManager, sPendingCheckInfo, on_progress, nullptr))
    {
        if (sPendingUpdate)
        {
            vpkc_free_update_info(sPendingUpdate);
        }
        sPendingUpdate = sPendingCheckInfo;
        sPendingCheckInfo = nullptr; // Ownership transferred
        LL_INFOS("Velopack") << "Update downloaded and pending" << LL_ENDL;
    }
    else
    {
        char descr[512];
        vpkc_get_last_error(descr, sizeof(descr));
        LL_WARNS("Velopack") << "Failed to download update: " << ll_safe_string((const char*)descr) << LL_ENDL;
    }
}

static void on_downloading_closed(const LLSD& notification, const LLSD& response)
{
    sDownloadingNotification = nullptr;
    if (sIsRequired)
    {
        // User closed the downloading dialog during a required update - re-show it
        show_downloading_notification(sTargetVersion);
    }
}

static void show_downloading_notification(const std::string& version)
{
    LLSD args;
    args["VERSION"] = version;
    sDownloadingNotification = LLNotificationsUtil::add("DownloadingUpdate", args, LLSD(), on_downloading_closed);
}

static void dismiss_downloading_notification()
{
    if (sDownloadingNotification)
    {
        LLNotificationsUtil::cancel(sDownloadingNotification);
        sDownloadingNotification = nullptr;
    }
}

static void on_required_update_response(const LLSD& notification, const LLSD& response)
{
    LL_INFOS("Velopack") << "Required update acknowledged, starting download" << LL_ENDL;
    show_downloading_notification(sTargetVersion);
    LLCoros::instance().launch("VelopackRequiredUpdate", []()
    {
        velopack_download_pending_update();
        dismiss_downloading_notification();
        if (velopack_is_update_pending())
        {
            LL_INFOS("Velopack") << "Required update downloaded, quitting to apply" << LL_ENDL;
            velopack_request_restart_after_update();
            LLAppViewer::instance()->requestQuit();
        }
    });
}

static void on_optional_update_response(const LLSD& notification, const LLSD& response)
{
    S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
    if (option == 0) // "Install"
    {
        LL_INFOS("Velopack") << "User accepted optional update, starting download" << LL_ENDL;
        show_downloading_notification(sTargetVersion);
        LLCoros::instance().launch("VelopackOptionalUpdate", []()
        {
            velopack_download_pending_update();
            dismiss_downloading_notification();
            if (velopack_is_update_pending())
            {
                LL_INFOS("Velopack") << "Optional update downloaded, quitting to apply" << LL_ENDL;
                velopack_request_restart_after_update();
                LLAppViewer::instance()->requestQuit();
            }
        });
    }
    else
    {
        LL_INFOS("Velopack") << "User declined optional update (option=" << option << ")" << LL_ENDL;
        // Free the check info since user declined
        if (sPendingCheckInfo)
        {
            vpkc_free_update_info(sPendingCheckInfo);
            sPendingCheckInfo = nullptr;
        }
    }
}

static void show_required_update_prompt()
{
    LLSD args;
    args["VERSION"] = sTargetVersion;
    args["URL"] = sReleaseNotesUrl;
    LLNotificationsUtil::add("PauseForUpdate", args, LLSD(), on_required_update_response);
}

void velopack_check_for_updates(const std::string& required_version, const std::string& relnotes_url)
{
    // <plus> research/311: the feed is fixed (feed_base_url()); the zip does not look for
    // updates through Velopack (plus/updatecheck tells about new versions there)
    if (!velopack_is_installed())
    {
        LL_INFOS("Velopack") << "Not a Velopack installation (the zip): no update check through Velopack" << LL_ENDL;
        return;
    }
    // </plus>

    // Allow downgrades only for rollbacks: VVM requires a version that's
    // strictly lower than what we're running (e.g., a retracted build).
    bool has_required = !required_version.empty();
    int ver_cmp = has_required ? compare_running_version(required_version) : 0;
    bool allow_downgrade = ver_cmp > 0; // running > required -> rollback scenario
    ensure_update_manager(allow_downgrade);
    if (!sUpdateManager)
        return;

    // Ask Velopack to check its feed - this is the source of truth
    vpkc_update_info_t* update_info = nullptr;
    vpkc_update_check_t result = vpkc_check_for_updates(sUpdateManager, &update_info);

    if (result != UPDATE_AVAILABLE || !update_info)
    {
        LL_INFOS("Velopack") << "No update available from feed (result=" << result << ")" << LL_ENDL;
        return;
    }

    // Extract the actual target version from Velopack's feed
    std::string target_version = update_info->TargetFullRelease->Version
        ? update_info->TargetFullRelease->Version : "";
    LL_INFOS("Velopack") << "Update available: " << target_version
                         << " (required_version=" << required_version << ")" << LL_ENDL;

    // Store state for the prompt/download phase
    sReleaseNotesUrl = relnotes_url;
    sTargetVersion = target_version;
    if (sPendingCheckInfo)
    {
        vpkc_free_update_info(sPendingCheckInfo);
    }
    sPendingCheckInfo = update_info;

    // Determine if this is mandatory: running version is below VVM's required floor
    bool is_required = ver_cmp < 0; // running < required -> must update
    sIsRequired = is_required;

    if (is_required)
    {
        LL_INFOS("Velopack") << "Required update (running below " << required_version
                             << "), prompting user for " << target_version << LL_ENDL;
        show_required_update_prompt();
        return;
    }

    // Optional update - check user preference
    U32 updater_setting = gSavedSettings.getU32("UpdaterServiceSetting");

    if (updater_setting == 3)
    {
        // "Install each update automatically" - download silently, apply on quit
        LL_INFOS("Velopack") << "Optional update to " << target_version
                             << ", downloading automatically (UpdaterServiceSetting=3)" << LL_ENDL;
        velopack_download_pending_update();
        return;
    }

    // Default / value 1: "Ask me when an optional update is ready to install"
    LL_INFOS("Velopack") << "Optional update available (" << target_version << "), prompting user" << LL_ENDL;
    LLSD args;
    args["VERSION"] = target_version;
    args["URL"] = relnotes_url;
    LLNotificationsUtil::add("PromptOptionalUpdate", args, LLSD(), on_optional_update_response);
}

std::string velopack_get_current_version()
{
    if (!sUpdateManager)
    {
        return "";
    }

    char version[64];
    size_t len = vpkc_get_current_version(sUpdateManager, version, sizeof(version));
    if (len > 0)
    {
        return std::string(version, len);
    }
    return "";
}

bool velopack_is_update_pending()
{
    return sPendingUpdate != nullptr;
}

bool velopack_is_required_update_in_progress()
{
    return sIsRequired && sPendingCheckInfo != nullptr;
}

std::string velopack_get_required_update_version()
{
    return sTargetVersion;
}

bool velopack_should_restart_after_update()
{
    return sRestartAfterUpdate;
}

void velopack_request_restart_after_update()
{
    sRestartAfterUpdate = true;
}

void velopack_apply_pending_update(bool restart)
{
    if (!sUpdateManager || !sPendingUpdate || !sPendingUpdate->TargetFullRelease)
    {
        LL_WARNS("Velopack") << "Cannot apply update: no pending update or manager" << LL_ENDL;
        return;
    }

    LL_INFOS("Velopack") << "Applying pending update (restart=" << restart << ")" << LL_ENDL;
    vpkc_wait_exit_then_apply_updates(sUpdateManager,
                                       sPendingUpdate->TargetFullRelease,
                                       false,
                                       restart,
                                       nullptr, 0);
}

void velopack_cleanup()
{
    if (sUpdateManager)
    {
        vpkc_free_update_manager(sUpdateManager);
        sUpdateManager = nullptr;
    }
    if (sUpdateSource)
    {
        vpkc_free_source(sUpdateSource);
        sUpdateSource = nullptr;
    }
    if (sPendingUpdate)
    {
        vpkc_free_update_info(sPendingUpdate);
        sPendingUpdate = nullptr;
    }
    if (sPendingCheckInfo)
    {
        vpkc_free_update_info(sPendingCheckInfo);
        sPendingCheckInfo = nullptr;
    }
    sAssetUrlMap.clear();
}

// <plus> research/311
bool velopack_is_installed()
{
    return sInstallKind == EInstallKind::Velopack;
}
// </plus>

void velopack_set_update_url(const std::string& url)
{
    // <plus> research/311 section 2.3, research/262 section 2.1: an update service's answer
    // (llvvmquery.cpp, only with --update-service) does not move the feed elsewhere
    if (url != feed_base_url())
    {
        LL_WARNS("Velopack") << "Update URL from the update service ignored (" << url_for_log(url)
                             << "); the feed stays " << feed_base_url() << LL_ENDL;
    }
    // </plus>
}

void velopack_set_progress_callback(std::function<void(int)> callback)
{
    sProgressCallback = callback;
}

#endif // LL_VELOPACK
