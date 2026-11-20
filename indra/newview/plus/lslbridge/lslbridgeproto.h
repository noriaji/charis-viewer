/**
 * @file lslbridgeproto.h
 * @brief The texts of the pose bridge (plus/lslbridge, research/336): the lines the worn bridge says to its
 *        owner's viewer and the bodies the viewer posts to it. Plain C++ without viewer types, so that the
 *        stand-alone test (lslbridgeproto_test.cpp) runs it as the viewer does.
 *
 * The bridge's script (app_settings/plus/lslbridge/charis_bridge.lsl) says, with llOwnerSay:
 *   <charisBridge><version>|<url>|<token></charisBridge>     its HTTP-in URL and the token it takes (each new URL)
 *   <charisBridgeNoUrl><version></charisBridgeNoUrl>          the region gives it no URL (no secret in it)
 *   <auroraPose><nonce>|<action>|<peer></auroraPose>          a pose message another avatar's attachment sent
 *                                                             (the tag and fields of Aurora's bridge)
 *   <charisShare>1|<peer>|<aud>|<seq>|<payload></charisShare> a pose shared by another avatar's bridge (research/374;
 *                                                             an empty payload ends the share)
 *   <charisReflect><nonce>|<action>|<peer></charisReflect>    a reflect request or answer (research/374b; the fields
 *                                                             of <auroraPose>, on its own channel)
 *   <charisPush>1|<peer>|<seq>|<payload></charisPush>         a pose pushed to us by another avatar (research/374b;
 *                                                             an empty payload ends it)
 * and takes these bodies (one LLSD string) at its URL:
 *   hello|<token>                                             the viewer is there: the bridge may relay now
 *                                                             (asked again for every new URL)
 *   pose|<token>|<action>|<nonce>|<target>                    say "pose1|<nonce>|<action>|<target>" to target
 *   share|<token>|<aud>|<seq>|<payload>                       shout "share1|<aud>|<seq>|<payload>" on SHARE_CHANNEL
 *   shareend|<token>|<seq>                                    shout "share1|0|<seq>|" (the end of the share)
 *   reflect|<token>|<action>|<nonce>|<target>                 say "reflect1|<nonce>|<action>|<target>" to target
 *                                                             on REFLECT_CHANNEL
 *   push|<token>|<target>|<seq>|<payload>                     say "push1|<seq>|<payload>" to target on PUSH_CHANNEL
 *   pushend|<token>|<target>|<seq>                            say "push1|<seq>|" to target (the end of the push)
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

#ifndef PLUS_LSLBRIDGE_LSLBRIDGEPROTO_H
#define PLUS_LSLBRIDGE_LSLBRIDGEPROTO_H

#include <cstddef>
#include <string>
#include <string_view>

namespace plus::lslbridge::proto
{
    // The version of the bridge's script (VERSION in charis_bridge.lsl). The bridge's inventory name ends with it,
    // so a new version makes a new object. Raise both together, and add the old version to isOldVersion() (lslbridge.cpp
    // takes the old object off and removes it, isOurName()).
    constexpr char BRIDGE_VERSION[] = "1.2";
    // Versions of the script that are replaced: 1.0 said its URL and token on its own (the one worn in another viewer
    // showed them in the owner's chat, research/366); 1.1 has no pose sharing (research/374, 374b). A bridge of such a
    // version is never worn, it is made anew. isOldVersion().
    bool isOldVersion(std::string_view version);

    // The call the viewer whispers on PROBE_CHANNEL (a positive channel: only scripts hear it) before the script says
    // anything (research/366). Nothing secret is in it. PROBE_CHANNEL and PROBE_TEXT are also in charis_bridge.lsl.
    constexpr int PROBE_CHANNEL = 473520184;
    constexpr char PROBE_TEXT[] = "charisBridgeProbe|1";
    // The call: PROBE_TEXT + "|" + the key of the worn bridge object (as the script's llGetKey() writes it: lower case).
    // The key is what keeps others from calling: the owner's avatar also speaks when they press the button of someone's
    // dialog (llDialog, llTextBox) or click a secondlife:///app/chat/<channel>/<text> link (any text; from any object's
    // chat line), so the text of a call is no secret. The key of a bridge worn on a HUD point is on no list (llGetAttachedList
    // and the sensors leave HUDs out, other viewers never get them) and changes at every wear; the script takes a call on
    // a HUD point only, and the viewer calls only a bridge worn there. Empty when objectKey is not a UUID.
    std::string probeText(std::string_view objectKey);
    constexpr std::size_t PROBE_DIALOG_MAX = 24; // the longest label of a dialog button

    // The five actions of the pose1 messages, in the order of plus::lslbridge::PoseAction.
    constexpr int ACTION_COUNT = 5;
    // "request", "accept", "refuse", "cancel", "withdraw"; nullptr for a number out of range.
    const char* actionName(int action);
    // The number of one of the five names (exactly, lower case), -1 for anything else.
    int actionFromName(std::string_view name);

    // Pose sharing (research/374) and the reflect of a pose pushed to its owner (research/374b): positive channels, all
    // three different from PROBE_CHANNEL and from each other; also in charis_bridge.lsl. SHARE_CHANNEL is shouted
    // (100 m), the other two are said to one avatar (llRegionSayTo).
    constexpr int         SHARE_CHANNEL   = 473520191;
    constexpr int         REFLECT_CHANNEL = 473520192;
    constexpr int         PUSH_CHANNEL    = 473520193;
    constexpr std::size_t SHARE_LINE_MAX    = 1000; // <charisShare>...</charisShare>, the whole line
    constexpr std::size_t PUSH_LINE_MAX     = 1000;
    constexpr std::size_t REFLECT_LINE_MAX  = 160;  // <charisReflect>...</charisReflect>
    constexpr std::size_t SHARE_PAYLOAD_MAX = 900;  // the base64 text of a pose (a whole pose takes about 780)
    constexpr std::size_t SHARE_SEQ_DIGITS_MAX = 9; // the sequence number: decimal digits, fits an int
    constexpr int         SEQ_MAX           = 999999999;
    constexpr std::size_t SHARE_BODY_MAX    = 1100; // the longest body the script takes (BODY_MAX in charis_bridge.lsl), the whole
                                                    // LLSD text: the bodies below plus 30 bytes of <llsd><string>...</string></llsd>

    constexpr std::size_t POSE_LINE_MAX     = 160; // <auroraPose>...</auroraPose>, the whole line
    constexpr std::size_t ANNOUNCE_LINE_MAX = 400; // <charisBridge>...</charisBridge>, the whole line
    constexpr std::size_t NO_URL_LINE_MAX   = 64;  // <charisBridgeNoUrl>...</charisBridgeNoUrl>
    constexpr std::size_t URL_MAX           = 255; // an HTTP-in URL
    constexpr std::size_t VERSION_MAX       = 8;   // the version field: digits and dots

    // Which of the bridge's lines `line` opens like (the opening tag only; parse it to know more).
    //   Pose          <auroraPose>
    //   Announcement  <charisBridge>
    //   NoUrl         <charisBridgeNoUrl>
    //   Share         <charisShare>
    //   Reflect       <charisReflect>
    //   Push          <charisPush>
    //   OtherBridge   any other line opening with "<charisBridge" (not one of ours: never shown, never used)
    enum class LineKind { None, Pose, Announcement, NoUrl, OtherBridge, Share, Reflect, Push };
    LineKind kindOf(std::string_view line);

    // 36 characters 8-4-4-4-12 of hexadecimal digits (either case) and not all zero.
    bool isUuid(std::string_view text);

    struct PoseLine
    {
        std::string nonce;  // lower case
        int         action = -1;
        std::string peer;   // lower case
    };
    // "<auroraPose><nonce>|<action>|<peer></auroraPose>": at most POSE_LINE_MAX bytes, nonce and peer isUuid(),
    // the action one of the five. False (out left as it was) for anything else.
    bool parsePoseLine(std::string_view line, PoseLine& out);

    // "<charisReflect><nonce>|<action>|<peer></charisReflect>": the fields and checks of parsePoseLine(), at most
    // REFLECT_LINE_MAX bytes.
    bool parseReflectLine(std::string_view line, PoseLine& out);

    // A pose shared by another avatar's bridge (research/374). payload is empty for the end of a share (then aud is 0).
    struct ShareLine
    {
        std::string peer;    // lower case, isUuid(): the avatar wearing the bridge that shouted it, as the simulator says
        int         aud = 0; // 0 everyone, 1 friends only
        int         seq = 0; // 0 to SEQ_MAX
        std::string payload; // base64 text (isBase64()), at most SHARE_PAYLOAD_MAX; empty: the end
    };
    // A pose pushed to us by another avatar (research/374b); payload empty: the end.
    struct PushLine
    {
        std::string peer;    // lower case, isUuid()
        int         seq = 0;
        std::string payload;
    };
    // Standard base64 (no line breaks, no URL-safe letters): letters, digits, '+', '/', length a multiple of 4, '='
    // only as the last one or two. Not empty.
    bool isBase64(std::string_view text);
    // "<charisShare>1|<peer>|<aud>|<seq>|<payload></charisShare>": at most SHARE_LINE_MAX bytes, five fields, version
    // "1", peer isUuid(), aud "0" or "1", seq 1 to SHARE_SEQ_DIGITS_MAX digits (no sign, no leading zero but "0"),
    // payload isBase64() up to SHARE_PAYLOAD_MAX or empty (the end, only with aud 0). False (out left as it was)
    // for anything else.
    bool parseShareLine(std::string_view line, ShareLine& out);
    // "<charisPush>1|<peer>|<seq>|<payload></charisPush>": the same checks, four fields, at most PUSH_LINE_MAX bytes.
    bool parsePushLine(std::string_view line, PushLine& out);

    struct Announcement
    {
        std::string version; // digits and dots, 1 to VERSION_MAX
        std::string url;     // isBridgeUrl()
        std::string token;   // lower case, isUuid()
    };
    // What was wrong with an announcement (for the log: the URL and the token are never logged).
    enum class AnnouncementCheck { Ok, Form, Version, Url, Token };
    // "<charisBridge><version>|<url>|<token></charisBridge>": at most ANNOUNCE_LINE_MAX bytes and three fields
    // (else Form), each field as above (else the first wrong one). out is set for Ok only.
    AnnouncementCheck parseAnnouncement(std::string_view line, Announcement& out);

    // What an announcement that was not taken looks like, for the log, with nothing secret in it: its length, its
    // number of fields, whether the version is one, the URL's scheme, its host (the last two labels when it has three
    // or more, each of the two only when it is a word: letters and hyphens, at most 16 of them; any other label, such
    // as a UUID or a number, by its length as "[N bytes]"; a host of fewer labels only by how many labels and bytes),
    // its port, its path's first segment (only "/cap/" by name) and the length of the rest, and whether the token is
    // a UUID. A URL field without "<letters>://" is given by its length only. Never the URL's capability id, the
    // token or a whole host name.
    std::string describeAnnouncement(std::string_view line);

    // "<charisBridgeNoUrl><version></charisBridgeNoUrl>" (at most NO_URL_LINE_MAX bytes, the version as above).
    bool parseNoUrl(std::string_view line, std::string& version);

    // An HTTP-in URL of a Second Life simulator as llRequestSecureURL() gives it:
    // "https://<host>[:<port>]/cap/<uuid>", the host a lower-case name (letters, digits, '-', '.') under
    // ".secondlife.io" or ".lindenlab.com", the port 1 to 5 digits, nothing after the UUID, at most URL_MAX bytes.
    // The viewer posts to no other address.
    bool isBridgeUrl(std::string_view url);

    // The bodies posted to the bridge (the text of one LLSD string). token, nonce and target are isUuid() (the
    // caller checked); poseBody() is empty for an action out of range.
    std::string helloBody(std::string_view token);
    std::string poseBody(std::string_view token, int action, std::string_view nonce, std::string_view target);
    // "reflect|<token>|<action>|<nonce>|<target>": as poseBody().
    std::string reflectBody(std::string_view token, int action, std::string_view nonce, std::string_view target);
    // "share|<token>|<aud>|<seq>|<payload>", "shareend|<token>|<seq>", "push|<token>|<target>|<seq>|<payload>" and
    // "pushend|<token>|<target>|<seq>": token and target isUuid() (the caller checked); empty for an aud other than 0 or
    // 1, a seq out of 0..SEQ_MAX, a payload that is not isBase64() or is over SHARE_PAYLOAD_MAX (the bridge refuses an
    // empty body).
    std::string shareBody(std::string_view token, int aud, int seq, std::string_view payload);
    std::string shareEndBody(std::string_view token, int seq);
    std::string pushBody(std::string_view token, std::string_view target, int seq, std::string_view payload);
    std::string pushEndBody(std::string_view token, std::string_view target, int seq);

    // The answer the bridge gives to a body it took (the LLSD string of its reply).
    constexpr char REPLY_OK[] = "ok";
}

#endif // PLUS_LSLBRIDGE_LSLBRIDGEPROTO_H
