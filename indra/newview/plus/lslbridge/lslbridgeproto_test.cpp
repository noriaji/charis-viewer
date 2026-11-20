// Stand-alone test of lslbridgeproto.h/.cpp (not part of the viewer build):
//   cl /nologo /EHsc /std:c++17 /W4 /WX lslbridgeproto_test.cpp lslbridgeproto.cpp && lslbridgeproto_test.exe
#include "lslbridgeproto.h"

#include <algorithm>
#include <cstdio>
#include <string>

using namespace plus::lslbridge::proto;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static const std::string NONCE = "0b9a6d1e-4c2f-4e8a-9f31-7d5c2a1b6e40";
static const std::string PEER  = "5f2e8c41-9a7b-4d3c-8e16-2b4a9c7d1f03";
static const std::string TOKEN = "c3d1e2f4-a5b6-4c7d-8e9f-0a1b2c3d4e5f";
static const std::string CAP   = "9e8d7c6b-5a49-4382-a1b0-c9d8e7f6a5b4";
static const std::string URL   = "https://simhost-0a1b2c3d4e5f60718.agni.secondlife.io:12043/cap/" + CAP;

static std::string poseLine(const std::string& nonce, const std::string& action, const std::string& peer)
{
    return "<auroraPose>" + nonce + "|" + action + "|" + peer + "</auroraPose>";
}

static std::string announce(const std::string& version, const std::string& url, const std::string& token)
{
    return "<charisBridge>" + version + "|" + url + "|" + token + "</charisBridge>";
}

int main()
{
    // ---- UUIDs ----
    CHECK(isUuid(NONCE));
    CHECK(isUuid("0B9A6D1E-4C2F-4E8A-9F31-7D5C2A1B6E40"));
    CHECK(!isUuid("00000000-0000-0000-0000-000000000000"));            // null
    CHECK(!isUuid("0b9a6d1e-4c2f-4e8a-9f31-7d5c2a1b6e4"));             // 35
    CHECK(!isUuid("0b9a6d1e-4c2f-4e8a-9f31-7d5c2a1b6e400"));           // 37
    CHECK(!isUuid("0b9a6d1e4c2f-4e8a-9f31-7d5c2a1b6e40-"));            // hyphens elsewhere
    CHECK(!isUuid("0b9a6d1e-4c2f-4e8a-9f31-7d5c2a1b6e4g"));            // not hex
    CHECK(!isUuid(""));

    // ---- actions ----
    static_assert(ACTION_COUNT == 5, "the five pose1 actions");
    CHECK(actionFromName("request") == 0 && actionFromName("accept") == 1 && actionFromName("refuse") == 2 &&
          actionFromName("cancel") == 3 && actionFromName("withdraw") == 4);
    CHECK(actionFromName("Request") == -1 && actionFromName("") == -1 && actionFromName("requests") == -1);
    CHECK(std::string(actionName(4)) == "withdraw" && actionName(5) == nullptr && actionName(-1) == nullptr);

    // ---- kinds ----
    CHECK(kindOf(poseLine(NONCE, "request", PEER)) == LineKind::Pose);
    CHECK(kindOf(announce("1.0", URL, TOKEN)) == LineKind::Announcement);
    CHECK(kindOf("hello") == LineKind::None && kindOf("") == LineKind::None && kindOf(" <auroraPose>") == LineKind::None);
    CHECK(kindOf("<charisBridgeNoUrl>1.0</charisBridgeNoUrl>") == LineKind::NoUrl);
    CHECK(kindOf("<charisBridgeX>" + URL) == LineKind::OtherBridge && kindOf("<charisBridge") == LineKind::OtherBridge);
    CHECK(kindOf("<charisbridge>") == LineKind::None);

    // ---- the no-URL line ----
    {
        std::string version;
        CHECK(parseNoUrl("<charisBridgeNoUrl>1.0</charisBridgeNoUrl>", version) && version == "1.0");
        std::string untouched = "x";
        CHECK(!parseNoUrl("<charisBridgeNoUrl></charisBridgeNoUrl>", untouched) && untouched == "x");
        CHECK(!parseNoUrl("<charisBridgeNoUrl>1.0|" + TOKEN + "</charisBridgeNoUrl>", untouched));
        CHECK(!parseNoUrl("<charisBridgeNoUrl>1.0</charisBridgeNoUrl> ", untouched));
    }

    // ---- what the log says of an announcement not taken: nothing secret ----
    {
        const std::string described = describeAnnouncement(announce("1.0", URL, TOKEN));
        CHECK(described.find(CAP) == std::string::npos && described.find(TOKEN) == std::string::npos);
        CHECK(described.find("simhost-0a1b2c3d4e5f60718") == std::string::npos);
        CHECK(described.find("secondlife.io") != std::string::npos && described.find("12043") != std::string::npos);
        CHECK(described.find("/cap/ + 36 bytes") != std::string::npos && described.find("fields 3") != std::string::npos);
        CHECK(described.find("token a UUID") != std::string::npos && described.find("scheme https") != std::string::npos);
        const std::string odd = describeAnnouncement("<charisBridge>1.0|http://a.b.example.net/x/" + CAP + "|" + TOKEN + "|z");
        CHECK(odd.find(CAP) == std::string::npos && odd.find(TOKEN) == std::string::npos);
        CHECK(odd.find("scheme http,") != std::string::npos && odd.find("example.net") != std::string::npos);
        CHECK(odd.find("fields 4") != std::string::npos && odd.find("path not /cap/") != std::string::npos);
        CHECK(odd.find("a.b.example") == std::string::npos);                       // only the host's last two labels
        CHECK(!describeAnnouncement("").empty() && !describeAnnouncement("<charisBridge>").empty());
        // A field with no scheme (a line of two fields: the token where the URL should be) and a host of one label
        // (a UUID): neither is written out.
        const std::string two = describeAnnouncement("<charisBridge>1.0|" + TOKEN + "</charisBridge>");
        CHECK(two.find(TOKEN) == std::string::npos && two.find("fields 2") != std::string::npos);
        CHECK(two.find("address not a URL (36 bytes)") != std::string::npos);
        const std::string one = describeAnnouncement("<charisBridge>1.0|https://" + TOKEN + "/cap/" + CAP + "|" + TOKEN + "</charisBridge>");
        CHECK(one.find(TOKEN) == std::string::npos && one.find(CAP) == std::string::npos);
        CHECK(one.find("host of 1 label(s), 36 bytes") != std::string::npos);
        const std::string pair = describeAnnouncement("<charisBridge>1.0|https://secret-host.example:1/cap/" + CAP + "|" + TOKEN + "</charisBridge>");
        CHECK(pair.find("secret-host") == std::string::npos && pair.find("host of 2 label(s)") != std::string::npos);
        // The token as one of the last two labels (with its hyphens, or without them), a number: by their length only.
        const std::string tail = describeAnnouncement("<charisBridge>1.0|https://a.b." + TOKEN + ":1/cap/" + CAP + "|" + TOKEN + "</charisBridge>");
        CHECK(tail.find(TOKEN) == std::string::npos && tail.find("host *.b.[36 bytes] (3 labels)") != std::string::npos);
        std::string bare = TOKEN;
        bare.erase(std::remove(bare.begin(), bare.end(), '-'), bare.end());
        const std::string hex = describeAnnouncement("<charisBridge>1.0|https://a." + bare + ".io/cap/" + CAP + "|" + TOKEN + "</charisBridge>");
        CHECK(hex.find(bare) == std::string::npos && hex.find("host *.[32 bytes].io") != std::string::npos);
        const std::string num = describeAnnouncement("<charisBridge>1.0|https://a.b.c4/cap/" + CAP + "|" + TOKEN + "</charisBridge>");
        CHECK(num.find("c4") == std::string::npos && num.find("host *.b.[2 bytes]") != std::string::npos);
        // A word of 16 letters is shown, one of 17 is not (LABEL_SHOWN_MAX).
        const std::string word16 = describeAnnouncement("<charisBridge>1.0|https://a.abcdefghijklmnop.io/cap/" + CAP + "|" + TOKEN + "</charisBridge>");
        CHECK(word16.find("host *.abcdefghijklmnop.io (3 labels)") != std::string::npos);
        const std::string word17 = describeAnnouncement("<charisBridge>1.0|https://a.abcdefghijklmnopq.io/cap/" + CAP + "|" + TOKEN + "</charisBridge>");
        CHECK(word17.find("abcdefghijklmnopq") == std::string::npos && word17.find("host *.[17 bytes].io (3 labels)") != std::string::npos);
    }

    // ---- pose lines ----
    {
        PoseLine line;
        CHECK(parsePoseLine(poseLine(NONCE, "accept", PEER), line));
        CHECK(line.nonce == NONCE && line.action == 1 && line.peer == PEER);

        PoseLine upper;
        CHECK(parsePoseLine(poseLine("0B9A6D1E-4C2F-4E8A-9F31-7D5C2A1B6E40", "withdraw", PEER), upper));
        CHECK(upper.nonce == NONCE && upper.action == 4);

        // The longest real line (withdraw) is well under the limit; one byte over it is refused.
        CHECK(poseLine(NONCE, "withdraw", PEER).size() <= POSE_LINE_MAX);
        std::string long_line = poseLine(NONCE, "withdraw", PEER);
        long_line.insert(12, std::string(POSE_LINE_MAX + 1 - long_line.size(), 'a'));
        CHECK(long_line.size() == POSE_LINE_MAX + 1);
        PoseLine untouched;
        CHECK(!parsePoseLine(long_line, untouched));
        CHECK(untouched.action == -1 && untouched.nonce.empty());  // left as it was

        PoseLine bad;
        CHECK(!parsePoseLine(poseLine(NONCE, "poke", PEER), bad));
        CHECK(!parsePoseLine(poseLine("00000000-0000-0000-0000-000000000000", "request", PEER), bad));
        CHECK(!parsePoseLine(poseLine(NONCE, "request", "00000000-0000-0000-0000-000000000000"), bad));
        CHECK(!parsePoseLine("<auroraPose>" + NONCE + "|request</auroraPose>", bad));                       // 2 fields
        CHECK(!parsePoseLine("<auroraPose>" + NONCE + "|request|" + PEER + "|x</auroraPose>", bad));       // 4 fields
        CHECK(!parsePoseLine("<auroraPose>" + NONCE + "|request|" + PEER + "</auroraPose> ", bad));        // after the tag
        CHECK(!parsePoseLine("<auroraPose>" + NONCE + "|request|" + PEER, bad));                           // not closed
        CHECK(!parsePoseLine("<auroraPose>" + NONCE + "||" + PEER + "</auroraPose>", bad));                // empty action
        CHECK(!parsePoseLine("<auroraPose></auroraPose>", bad));
        CHECK(!parsePoseLine("<auroraPose>", bad));
    }

    // ---- URLs ----
    CHECK(isBridgeUrl(URL));
    CHECK(isBridgeUrl("https://sim7337.agni.lindenlab.com:12043/cap/" + CAP));
    CHECK(isBridgeUrl("https://simhost-1.aditi.secondlife.io/cap/" + CAP));                  // no port
    CHECK(!isBridgeUrl("http://simhost-1.agni.secondlife.io:12046/cap/" + CAP));             // not https
    CHECK(!isBridgeUrl("https://example.com/cap/" + CAP));
    CHECK(!isBridgeUrl("https://secondlife.io.example.com:12043/cap/" + CAP));
    CHECK(!isBridgeUrl("https://.secondlife.io:12043/cap/" + CAP));                          // no label before
    CHECK(!isBridgeUrl("https://secondlife.io:12043/cap/" + CAP));
    CHECK(!isBridgeUrl("https://a..secondlife.io:12043/cap/" + CAP));
    CHECK(!isBridgeUrl("https://-a.secondlife.io:12043/cap/" + CAP));
    CHECK(!isBridgeUrl("https://SimHost-1.agni.secondlife.io:12043/cap/" + CAP));            // upper case host
    CHECK(!isBridgeUrl("https://x.secondlife.io@example.com/cap/" + CAP));                   // user info
    CHECK(!isBridgeUrl("https://x.secondlife.io:/cap/" + CAP));                              // empty port
    CHECK(!isBridgeUrl("https://x.secondlife.io:123456/cap/" + CAP));                        // 6 digits
    CHECK(!isBridgeUrl("https://x.secondlife.io:12043/cap/" + CAP + "/"));                   // after the id
    CHECK(!isBridgeUrl("https://x.secondlife.io:12043/cap/" + CAP + "?a=b"));
    CHECK(!isBridgeUrl("https://x.secondlife.io:12043/caps/" + CAP));
    CHECK(!isBridgeUrl("https://x.secondlife.io:12043/cap/00000000-0000-0000-0000-000000000000"));
    CHECK(!isBridgeUrl("https://x.secondlife.io:12043/cap/" + CAP.substr(1)));
    CHECK(!isBridgeUrl(""));
    {
        std::string longest = "https://" + std::string(URL_MAX, 'a') + ".secondlife.io/cap/" + CAP;
        CHECK(!isBridgeUrl(longest));
    }

    // ---- announcements ----
    {
        using C = AnnouncementCheck;
        Announcement a;
        CHECK(parseAnnouncement(announce(BRIDGE_VERSION, URL, TOKEN), a) == C::Ok);
        CHECK(a.version == BRIDGE_VERSION && a.url == URL && a.token == TOKEN);

        Announcement upper;
        CHECK(parseAnnouncement(announce("1.0", URL, "C3D1E2F4-A5B6-4C7D-8E9F-0A1B2C3D4E5F"), upper) == C::Ok);
        CHECK(upper.token == TOKEN);

        Announcement bad;
        CHECK(parseAnnouncement(announce("1.0", "https://example.com/cap/" + CAP, TOKEN), bad) == C::Url);
        CHECK(parseAnnouncement(announce("1.0", URL, "00000000-0000-0000-0000-000000000000"), bad) == C::Token);
        CHECK(parseAnnouncement(announce("1.0b", URL, TOKEN), bad) == C::Version);
        CHECK(parseAnnouncement(announce("", URL, TOKEN), bad) == C::Version);
        CHECK(parseAnnouncement(announce("123456789", URL, TOKEN), bad) == C::Version);       // version too long
        CHECK(parseAnnouncement("<charisBridge>1.0|" + URL + "</charisBridge>", bad) == C::Form);   // 2 fields
        CHECK(parseAnnouncement("<charisBridge>1.0|" + URL + "|" + TOKEN + "|x</charisBridge>", bad) == C::Form);
        CHECK(parseAnnouncement(announce("1.0", URL, TOKEN) + "x", bad) == C::Form);
        std::string padded = announce("1.0", URL, TOKEN);
        padded.insert(14, std::string(ANNOUNCE_LINE_MAX + 1 - padded.size(), '1'));            // a long version
        CHECK(padded.size() == ANNOUNCE_LINE_MAX + 1 && parseAnnouncement(padded, bad) == C::Form);
        CHECK(bad.url.empty() && bad.token.empty());  // left as it was
    }

    // ---- bodies ----
    CHECK(helloBody(TOKEN) == "hello|" + TOKEN);
    CHECK(poseBody(TOKEN, 0, NONCE, PEER) == "pose|" + TOKEN + "|request|" + NONCE + "|" + PEER);
    CHECK(poseBody(TOKEN, 4, NONCE, PEER) == "pose|" + TOKEN + "|withdraw|" + NONCE + "|" + PEER);
    CHECK(poseBody(TOKEN, 5, NONCE, PEER).empty() && poseBody(TOKEN, -1, NONCE, PEER).empty());
    // The pose1 message the script makes from the longest body stays within its 130 bytes.
    CHECK(std::string("pose1|" + NONCE + "|withdraw|" + PEER).size() <= 130);

    // The call the viewer whispers before the script says anything (research/366): a positive channel (heard by scripts
    // only, never shown in an avatar's chat), nothing secret in the text, the script's own version is not an old one.
    CHECK(std::to_string(PROBE_CHANNEL).front() != '-' && std::to_string(PROBE_CHANNEL) != "0" &&
          std::to_string(PROBE_CHANNEL) != "2147483647");
    CHECK(std::string(PROBE_TEXT) == "charisBridgeProbe|1");
    CHECK(probeText(CAP) == "charisBridgeProbe|1|" + CAP);
    CHECK(probeText("9E8D7C6B-5A49-4382-A1B0-C9D8E7F6A5B4") == "charisBridgeProbe|1|" + CAP); // lower case, as llGetKey writes it
    // A call is longer than a dialog button's label (24 bytes): someone's llDialog cannot make the owner speak it.
    CHECK(probeText(CAP).size() > PROBE_DIALOG_MAX && std::string(PROBE_TEXT).size() + 1 + 36 == probeText(CAP).size());
    CHECK(probeText("").empty() && probeText("not a key").empty() && probeText("00000000-0000-0000-0000-000000000000").empty());
    CHECK(std::string(BRIDGE_VERSION) == "1.2" && !isOldVersion(BRIDGE_VERSION));
    CHECK(isOldVersion("1.0") && isOldVersion("1.1") && !isOldVersion("1.2") && !isOldVersion("") && !isOldVersion("1.0.0") && !isOldVersion("v1.0"));
    // An old script's announcement still parses (the viewer makes that bridge anew: another version).
    {
        Announcement old;
        CHECK(parseAnnouncement(announce("1.0", URL, TOKEN), old) == AnnouncementCheck::Ok && isOldVersion(old.version));
    }

    // ---- pose sharing, reflect and push (research/374, 374b) ----
    {
        // The three channels: positive, not the debug channel, all different (and from the call's).
        static_assert(SHARE_CHANNEL > 0 && REFLECT_CHANNEL > 0 && PUSH_CHANNEL > 0 && SHARE_CHANNEL < 2147483647 &&
                          REFLECT_CHANNEL < 2147483647 && PUSH_CHANNEL < 2147483647, "positive, not the debug channel");
        static_assert(SHARE_CHANNEL != PROBE_CHANNEL && REFLECT_CHANNEL != PROBE_CHANNEL && PUSH_CHANNEL != PROBE_CHANNEL &&
                          SHARE_CHANNEL != REFLECT_CHANNEL && SHARE_CHANNEL != PUSH_CHANNEL && REFLECT_CHANNEL != PUSH_CHANNEL,
                      "all different");
        static_assert(SHARE_LINE_MAX == 1000 && SHARE_PAYLOAD_MAX < SHARE_LINE_MAX && SHARE_PAYLOAD_MAX % 4 == 0, "limits");

        // base64
        CHECK(isBase64("QUJD") && isBase64("QUI=") && isBase64("QQ==") && isBase64("a+/9Zz0A"));
        CHECK(!isBase64("") && !isBase64("QUJ") && !isBase64("QU=D") && !isBase64("Q===") && !isBase64("QUJD=") &&
              !isBase64("QQ=A") && !isBase64("QU-_") && !isBase64("QU J") && !isBase64("QUJ\n") && !isBase64("====") &&
              !isBase64("QUJD|") && !isBase64("QUJ<"));

        const std::string PAY = "QUJDREVGR0g=";
        auto shareLine = [&](const std::string& ver, const std::string& peer, const std::string& aud, const std::string& seq,
                             const std::string& pay)
        { return "<charisShare>" + ver + "|" + peer + "|" + aud + "|" + seq + "|" + pay + "</charisShare>"; };
        CHECK(kindOf(shareLine("1", PEER, "0", "5", PAY)) == LineKind::Share && kindOf("<charisShare") == LineKind::None);
        ShareLine sl;
        CHECK(parseShareLine(shareLine("1", "5F2E8C41-9A7B-4D3C-8E16-2B4A9C7D1F03", "1", "123456789", PAY), sl) &&
              sl.peer == PEER && sl.aud == 1 && sl.seq == 123456789 && sl.payload == PAY);
        CHECK(parseShareLine(shareLine("1", PEER, "0", "0", PAY), sl) && sl.seq == 0 && sl.aud == 0);
        CHECK(parseShareLine(shareLine("1", PEER, "0", "7", ""), sl) && sl.payload.empty() && sl.seq == 7); // the end
        // refused (and out left as it was)
        sl.peer = "keep";
        CHECK(!parseShareLine(shareLine("2", PEER, "0", "5", PAY), sl));           // version
        CHECK(!parseShareLine(shareLine("1", "not-a-key", "0", "5", PAY), sl));    // peer
        CHECK(!parseShareLine(shareLine("1", "00000000-0000-0000-0000-000000000000", "0", "5", PAY), sl));
        CHECK(!parseShareLine(shareLine("1", PEER, "2", "5", PAY), sl));           // aud
        CHECK(!parseShareLine(shareLine("1", PEER, "-1", "5", PAY), sl));
        CHECK(!parseShareLine(shareLine("1", PEER, "", "5", PAY), sl));
        CHECK(!parseShareLine(shareLine("1", PEER, "01", "5", PAY), sl));
        CHECK(!parseShareLine(shareLine("1", PEER, "0", "", PAY), sl));            // seq
        CHECK(!parseShareLine(shareLine("1", PEER, "0", "-5", PAY), sl));
        CHECK(!parseShareLine(shareLine("1", PEER, "0", "+5", PAY), sl));
        CHECK(!parseShareLine(shareLine("1", PEER, "0", "05", PAY), sl));
        CHECK(!parseShareLine(shareLine("1", PEER, "0", "1000000000", PAY), sl));  // 10 digits
        CHECK(!parseShareLine(shareLine("1", PEER, "0", "1e5", PAY), sl));
        CHECK(!parseShareLine(shareLine("1", PEER, "0", "5", "QUJ"), sl));         // payload not base64
        CHECK(!parseShareLine(shareLine("1", PEER, "0", "5", "QU-_"), sl));
        CHECK(!parseShareLine(shareLine("1", PEER, "1", "5", ""), sl));            // an end only for everyone
        CHECK(!parseShareLine(shareLine("1", PEER, "0", "5", PAY + "|x"), sl));    // a sixth field
        CHECK(!parseShareLine("<charisShare>1|" + PEER + "|0|5</charisShare>", sl)); // four fields
        CHECK(!parseShareLine(" " + shareLine("1", PEER, "0", "5", PAY), sl) &&
              !parseShareLine(shareLine("1", PEER, "0", "5", PAY) + " ", sl) &&
              !parseShareLine("<charisShare>1|" + PEER + "|0|5|" + PAY, sl));
        CHECK(sl.peer == "keep");
        {
            // The longest payload that fits, and one more.
            const std::string longest(SHARE_PAYLOAD_MAX, 'A'); // 900 is a multiple of 4
            CHECK(parseShareLine(shareLine("1", PEER, "1", "999999999", longest), sl) && sl.payload.size() == SHARE_PAYLOAD_MAX);
            CHECK(shareLine("1", PEER, "1", "999999999", longest).size() <= SHARE_LINE_MAX);
            CHECK(!parseShareLine(shareLine("1", PEER, "1", "5", std::string(SHARE_PAYLOAD_MAX + 4, 'A')), sl));
            // over SHARE_LINE_MAX as a whole: not taken
            CHECK(!parseShareLine(shareLine("1", PEER, "1", "5", std::string(1000, 'A')), sl));
        }

        // push: the same, without aud
        auto pushLine = [&](const std::string& ver, const std::string& peer, const std::string& seq, const std::string& pay)
        { return "<charisPush>" + ver + "|" + peer + "|" + seq + "|" + pay + "</charisPush>"; };
        CHECK(kindOf(pushLine("1", PEER, "5", PAY)) == LineKind::Push);
        PushLine pl;
        CHECK(parsePushLine(pushLine("1", PEER, "42", PAY), pl) && pl.peer == PEER && pl.seq == 42 && pl.payload == PAY);
        CHECK(parsePushLine(pushLine("1", PEER, "43", ""), pl) && pl.payload.empty());
        CHECK(!parsePushLine(pushLine("2", PEER, "42", PAY), pl) && !parsePushLine(pushLine("1", "x", "42", PAY), pl) &&
              !parsePushLine(pushLine("1", PEER, "042", PAY), pl) && !parsePushLine(pushLine("1", PEER, "42", "QU="), pl) &&
              !parsePushLine("<charisPush>1|" + PEER + "|0|42|" + PAY + "</charisPush>", pl) &&
              !parsePushLine(shareLine("1", PEER, "0", "42", PAY), pl));
        CHECK(!parseShareLine(pushLine("1", PEER, "42", PAY), sl));

        // reflect: the fields of a pose line, its own tags
        auto reflectLine = [&](const std::string& nonce, const std::string& action, const std::string& peer)
        { return "<charisReflect>" + nonce + "|" + action + "|" + peer + "</charisReflect>"; };
        CHECK(kindOf(reflectLine(NONCE, "request", PEER)) == LineKind::Reflect);
        PoseLine rl;
        CHECK(parseReflectLine(reflectLine(NONCE, "withdraw", PEER), rl) && rl.nonce == NONCE && rl.action == 4 &&
              rl.peer == PEER);
        CHECK(!parseReflectLine(reflectLine(NONCE, "poke", PEER), rl) && !parseReflectLine(reflectLine("x", "accept", PEER), rl) &&
              !parseReflectLine(poseLine(NONCE, "accept", PEER), rl) && !parsePoseLine(reflectLine(NONCE, "accept", PEER), rl));
        CHECK(parsePoseLine(poseLine(NONCE, "accept", PEER), rl)); // pose lines are as they were

        // the bodies
        CHECK(shareBody(TOKEN, 1, 12, PAY) == "share|" + TOKEN + "|1|12|" + PAY);
        CHECK(shareBody(TOKEN, 0, 0, PAY) == "share|" + TOKEN + "|0|0|" + PAY);
        CHECK(shareBody(TOKEN, 2, 12, PAY).empty() && shareBody(TOKEN, -1, 12, PAY).empty() &&
              shareBody(TOKEN, 0, -1, PAY).empty() && shareBody(TOKEN, 0, 1000000000, PAY).empty() &&
              shareBody(TOKEN, 0, 1, "").empty() && shareBody(TOKEN, 0, 1, "a|b=").empty() &&
              shareBody(TOKEN, 0, 1, std::string(SHARE_PAYLOAD_MAX + 4, 'A')).empty());
        CHECK(shareEndBody(TOKEN, 9) == "shareend|" + TOKEN + "|9" && shareEndBody(TOKEN, -1).empty() &&
              shareEndBody(TOKEN, 1000000000).empty());
        CHECK(pushBody(TOKEN, PEER, 3, PAY) == "push|" + TOKEN + "|" + PEER + "|3|" + PAY && pushBody(TOKEN, PEER, 3, "").empty() &&
              pushBody(TOKEN, PEER, -3, PAY).empty());
        CHECK(pushEndBody(TOKEN, PEER, 4) == "pushend|" + TOKEN + "|" + PEER + "|4" && pushEndBody(TOKEN, PEER, -4).empty());
        CHECK(reflectBody(TOKEN, 1, NONCE, PEER) == "reflect|" + TOKEN + "|accept|" + NONCE + "|" + PEER &&
              reflectBody(TOKEN, 5, NONCE, PEER).empty());
        // The biggest bodies, in the 30 bytes of the LLSD string around them, stay within what the script takes (BODY_MAX).
        CHECK(30 + shareBody(TOKEN, 1, 999999999, std::string(SHARE_PAYLOAD_MAX, 'A')).size() <= SHARE_BODY_MAX);
        CHECK(30 + pushBody(TOKEN, PEER, 999999999, std::string(SHARE_PAYLOAD_MAX, 'A')).size() <= SHARE_BODY_MAX);
        CHECK(30 + reflectBody(TOKEN, 4, NONCE, PEER).size() <= SHARE_BODY_MAX);
    }

    if (failures)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
