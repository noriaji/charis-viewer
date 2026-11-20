// Charis Viewer pose bridge, version 1.2 (plus/lslbridge, research/336, research/366, research/374, research/374b).
// The viewer wears it on the HUD (Center 2) when PlusPoseBridgeEnable is on, and nothing else uses it.
//
// It carries the pose-consent messages of the viewers that speak Aurora's "pose1" (channel -184735220):
//   - the viewer posts "pose|<token>|<action>|<nonce>|<target>" to this script's HTTP-in URL, and the script
//     says "pose1|<nonce>|<action>|<target>" to the target avatar only (llRegionSayTo);
//   - a "pose1" message for the owner from another avatar's attachment is checked and told to the owner's
//     viewer as "<auroraPose><nonce>|<action>|<peer></auroraPose>", the peer being the simulator's owner of
//     the sending attachment (never a name or key written in the message).
// The bridge never speaks first (research/366): another viewer would show its llOwnerSay lines, with the URL and the
// token, in the owner's nearby chat. The Charis Viewer calls first, whispering PROBE_TEXT + "|" + this object's key on
// PROBE_CHANNEL (a positive channel: only scripts hear it). Only the owner avatar is listened to, but what the owner
// says is not only what they type: they also speak when they press the button of someone's dialog, or click a
// secondlife:///app/chat/<channel>/<text> link (any text, from any object's chat line), so the text of a call is no
// secret and the check is that it holds the key of this worn object. That key is on no list while the bridge is worn
// on a HUD point (llGetAttachedList and the sensors leave HUDs out, other avatars' viewers never get them) and it
// changes at every wear; so a call is taken only on a HUD point (a bridge worn on the body has a key anyone can read:
// worn so, it takes itself off after PAIR_WAIT and the viewer wears it on the HUD again; moved there by hand once called,
// it says nothing more, the viewer's wait for its next URL ends and repairs it onto the HUD). The key is known outside only through the viewer's own
// call and the relay of a pose request (said to the other avatar by llRegionSayTo): only while the Charis Viewer
// runs the bridge, which hides the URL lines. Once called
// ("paired", until the next wear or reset) every new URL (a region crossed or restarted, a teleport) comes with a new
// token in "<charisBridge><version>|<url>|<token></charisBridge>", said again every ANNOUNCE_GAP seconds until the
// viewer answers "hello|<token>"; a call is answered at once with the URL held. Until that hello nothing is relayed
// either way. A region that gives no URL is told (once the bridge is called) as
// "<charisBridgeNoUrl><version></charisBridgeNoUrl>". Never called within PAIR_WAIT seconds of a wear (not a Charis
// Viewer): the bridge says nothing and takes itself off.
//
// Version 1.2 also carries three more kinds of message between Charis Viewers, each on its own positive channel and
// only after the viewer's hello (nothing is relayed before it, either way):
//   - share (research/374): the viewer posts "share|<token>|<aud>|<seq>|<payload>" (or "shareend|<token>|<seq>") and
//     the script shouts "share1|<aud>|<seq>|<payload>" on SHARE_CHANNEL (100 m); a share heard from another avatar's
//     attachment is told to the owner's viewer as "<charisShare>1|<peer>|<aud>|<seq>|<payload></charisShare>";
//   - reflect (research/374b): requests and answers like pose1 ("reflect|<token>|<action>|<nonce>|<target>", said to
//     the target on REFLECT_CHANNEL, told as "<charisReflect><nonce>|<action>|<peer></charisReflect>");
//   - push (research/374b): "push|<token>|<target>|<seq>|<payload>" (or "pushend|<token>|<target>|<seq>"), said to the
//     target on PUSH_CHANNEL and told to its viewer as "<charisPush>1|<peer>|<seq>|<payload></charisPush>".
// The script does not read a payload: it checks its length and that it holds no character that could break the line
// (the viewer checks the base64 and the pose). The peer of every line is the simulator's owner of the sending
// attachment (wearerOf), never a name or key in the message. At most SHARE_GAP seconds apart from one avatar's
// attachments, and at most SHOUT_MAX things said a second (the viewer limits itself as well).
//
// Written for Charis; the way the bridge is made, kept out of the world and made to leave other viewers
// follows Firestorm's LSL bridge (see plus/lslbridge/ORIGIN.md). The pose1 format and checks are research/336 §2.
//
// $LicenseInfo:firstyear=2026&license=viewerlgpl$
// This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
// General Public License as published by the Free Software Foundation; version 2.1 of the License only.
// This library is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
// implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
// License for more details.
// $/LicenseInfo$

string  VERSION       = "1.2";          // proto::BRIDGE_VERSION in lslbridgeproto.h
integer POSE_CHANNEL  = -184735220;
integer PROBE_CHANNEL = 473520184;      // proto::PROBE_CHANNEL
string  PROBE_TEXT    = "charisBridgeProbe|1"; // proto::PROBE_TEXT; the call is this + "|" + our key (proto::probeText(key))
integer SHARE_CHANNEL   = 473520191;    // proto::SHARE_CHANNEL: shouted (a share is for everyone near)
integer REFLECT_CHANNEL = 473520192;    // proto::REFLECT_CHANNEL: said to one avatar (llRegionSayTo)
integer PUSH_CHANNEL    = 473520193;    // proto::PUSH_CHANNEL: said to one avatar
integer PROBE_GAP     = 2;              // seconds before the next call is answered
integer PAIR_WAIT     = 60;             // seconds after a wear without a call before the bridge takes itself off
integer POSE_MAX      = 130;            // the longest pose1 message taken or said
integer BODY_MAX      = 1100;           // the longest body taken from the viewer (proto::SHARE_BODY_MAX)
integer PAYLOAD_MAX   = 900;            // the longest payload (proto::SHARE_PAYLOAD_MAX)
integer SEQ_DIGITS_MAX = 9;             // proto::SHARE_SEQ_DIGITS_MAX
integer SHARE_MAX     = 960;            // the longest share1 or push1 message heard
float   SHARE_GAP     = 0.3;            // seconds before the same avatar's next share or push is taken
integer GAP_MAX       = 32;             // avatars whose last share or push time is kept
integer SHOUT_MAX     = 6;              // things said (a share, an end, a push) in one second
integer ASKERS_MAX    = 64;             // avatars whose last request time is kept
integer ASK_GAP       = 5;              // seconds before the same avatar's next request is taken
float   ANNOUNCE_GAP  = 15.0;           // seconds between announcements while no hello came
integer ANNOUNCE_MAX  = 3;              // announcements of one URL before the bridge takes itself off
float   URL_RETRY_GAP = 10.0;           // seconds before a refused URL is asked for again
integer URL_RETRY_MAX = 30;
list    ACTIONS       = ["request", "accept", "refuse", "cancel", "withdraw"];
list    BAD_CHARS     = ["<", ">", "\n", " ", "\""]; // none of them is in base64; they could break a line

key     gOwner;
string  gUrl;
key     gUrlRequest;
key     gToken;
integer gConnected;                     // the viewer said hello with this URL's token
integer gAnnounced;                     // announcements of this URL
integer gUrlTries;                      // refused URL requests in a row
integer gListen;
integer gProbeListen;
integer gShareListen;
integer gReflectListen;
integer gPushListen;
integer gPaired;                        // the owner's viewer called (reset by every wear and rez)
integer gPairBy;                        // unix time by which a call must have come
integer gLastReply;                     // unix time of the last answer to a call
list    gAskers;                        // the avatars that asked lately (keys)
list    gAskTimes;                      // the unix time of each one's last request, in the same order
list    gGapWho;                        // "<kind><avatar>" of the last shares and pushes taken
list    gGapAt;                         // llGetTime() of each one, in the same order
float   gShoutWindow;                   // llGetTime() when the second of gShoutCount began
integer gShoutCount;                    // things said in that second

// A key written out in full, not NULL_KEY.
integer isKey(string text)
{
    if (llStringLength(text) != 36)
    {
        return FALSE;
    }
    if ((key)text)
    {
        return TRUE;
    }
    return FALSE;
}

releaseUrl()
{
    if (gUrl != "")
    {
        llReleaseURL(gUrl);
    }
    gUrl = "";
    gToken = NULL_KEY;
}

// A new URL and a new token: the viewer says hello again before anything is relayed.
askForUrl()
{
    llSetTimerEvent(0.0);
    releaseUrl();
    gConnected = FALSE;
    gAnnounced = 0;
    gToken = llGenerateKey();
    gUrlRequest = llRequestSecureURL();
    armTimer();
}

// The one timer: until the viewer calls, the deadline (and a refused URL asked for again before it); after, the
// announcements until the hello (and a refused URL asked for again).
armTimer()
{
    if (!gPaired)
    {
        integer left = gPairBy - llGetUnixTime();
        if (left < 1)
        {
            left = 1;
        }
        if (gUrl == "" && gUrlTries > 0 && URL_RETRY_GAP < left)
        {
            left = (integer)URL_RETRY_GAP;
        }
        llSetTimerEvent((float)left);
    }
    else if (gUrl == "")
    {
        if (gUrlTries > 0 && gUrlTries < URL_RETRY_MAX)
        {
            llSetTimerEvent(URL_RETRY_GAP);
        }
        else
        {
            llSetTimerEvent(0.0);
        }
    }
    else if (!gConnected)
    {
        llSetTimerEvent(ANNOUNCE_GAP);
    }
    else
    {
        llSetTimerEvent(0.0);
    }
}

// Only for a viewer that called (gPaired): nothing secret is said to anyone else. counted: the timer's repeats
// (ANNOUNCE_MAX of them, then the bridge goes); an answer to a call is not counted.
announce(integer counted)
{
    if (!gPaired || !onHud())
    {
        return;
    }
    llOwnerSay("<charisBridge>" + VERSION + "|" + gUrl + "|" + (string)gToken + "</charisBridge>");
    if (counted)
    {
        ++gAnnounced;
    }
    armTimer();
}

// The region gives no URL (no secret in the line); only to a viewer that called.
announceNoUrl()
{
    if (gPaired && onHud())
    {
        llOwnerSay("<charisBridgeNoUrl>" + VERSION + "</charisBridgeNoUrl>");
    }
}

startBridge()
{
    gOwner = llGetOwner();
    gAskers = [];
    gAskTimes = [];
    gGapWho = [];
    gGapAt = [];
    gShoutCount = 0;
    llResetTime(); // llGetTime() is a float: kept small, its steps stay finer than SHARE_GAP however long the script runs
    gShoutWindow = 0.0;
    gUrlTries = 0;
    stopOtherScripts(); // one put in while it was not worn could speak for the bridge from the start
    gPaired = FALSE;
    gLastReply = 0;
    gPairBy = llGetUnixTime() + PAIR_WAIT;
    llListenRemove(gListen);
    gListen = llListen(POSE_CHANNEL, "", NULL_KEY, "");
    llListenRemove(gProbeListen);
    gProbeListen = llListen(PROBE_CHANNEL, "", gOwner, ""); // the owner avatar only; the text holds our key too
    llListenRemove(gShareListen);
    gShareListen = llListen(SHARE_CHANNEL, "", NULL_KEY, "");
    llListenRemove(gReflectListen);
    gReflectListen = llListen(REFLECT_CHANNEL, "", NULL_KEY, "");
    llListenRemove(gPushListen);
    gPushListen = llListen(PUSH_CHANNEL, "", NULL_KEY, "");
    askForUrl();
}

stopBridge()
{
    gConnected = FALSE;
    gPaired = FALSE;
    gAskers = [];
    gAskTimes = [];
    llListenRemove(gListen);
    gListen = 0;
    llListenRemove(gProbeListen);
    gProbeListen = 0;
    llListenRemove(gShareListen);
    gShareListen = 0;
    llListenRemove(gReflectListen);
    gReflectListen = 0;
    llListenRemove(gPushListen);
    gPushListen = 0;
    gGapWho = [];
    gGapAt = [];
    llSetTimerEvent(0.0);
    releaseUrl();
}

// No other script runs in the bridge: one could speak for it.
stopOtherScripts()
{
    string self = llGetScriptName();
    integer n = llGetInventoryNumber(INVENTORY_SCRIPT);
    while (n > 0)
    {
        --n;
        string other = llGetInventoryName(INVENTORY_SCRIPT, n);
        if (other != self)
        {
            llSetScriptState(other, FALSE);
        }
    }
    llAllowInventoryDrop(FALSE);
}

reply(key request, integer status, string text)
{
    llHTTPResponse(request, status, "<llsd><string>" + text + "</string></llsd>");
}

// A sequence number: 1 to SEQ_DIGITS_MAX digits, no sign, no leading zero but "0" (the number written back is the text).
integer seqOk(string text)
{
    integer size = llStringLength(text);
    if (size < 1 || size > SEQ_DIGITS_MAX)
    {
        return FALSE;
    }
    if (llGetSubString(text, 0, 0) == "-")
    {
        return FALSE;
    }
    // Written back as it is: no sign, no leading zero, nothing but digits.
    return ((string)((integer)text) == text);
}

// A payload: at most PAYLOAD_MAX, a multiple of 4 (base64), none of BAD_CHARS. Not empty unless emptyOk (the end).
integer payloadOk(string text, integer emptyOk)
{
    integer size = llStringLength(text);
    if (size == 0)
    {
        return emptyOk;
    }
    if (size > PAYLOAD_MAX || size % 4 != 0)
    {
        return FALSE;
    }
    integer n = llGetListLength(BAD_CHARS);
    while (n > 0)
    {
        --n;
        if (llSubStringIndex(text, llList2String(BAD_CHARS, n)) != -1)
        {
            return FALSE;
        }
    }
    return TRUE;
}

// At most SHOUT_MAX things said in a second (the viewer keeps below it); false: not said.
integer saidAllowed()
{
    float stamp = llGetTime();
    if (stamp - gShoutWindow >= 1.0 || stamp < gShoutWindow)
    {
        gShoutWindow = stamp;
        gShoutCount = 0;
    }
    if (gShoutCount >= SHOUT_MAX)
    {
        return FALSE;
    }
    ++gShoutCount;
    return TRUE;
}

// A body from the viewer: the text of one LLSD string.
takeCommand(key request, string body)
{
    integer from = llSubStringIndex(body, "<string>");
    integer upto = llSubStringIndex(body, "</string>");
    // An empty string is refused too (llGetSubString with its end before its start gives what is outside).
    if (llStringLength(body) > BODY_MAX || from == -1 || upto <= from + 8)
    {
        reply(request, 400, "bad");
        return;
    }
    list words = llParseStringKeepNulls(llGetSubString(body, from + 8, upto - 1), ["|"], []);
    if (!isKey((string)gToken) || llToLower(llList2String(words, 1)) != (string)gToken)
    {
        reply(request, 403, "token");
        return;
    }
    string verb = llList2String(words, 0);
    integer count = llGetListLength(words);
    if (verb == "hello" && count == 2)
    {
        gConnected = TRUE;
        llSetTimerEvent(0.0);
        reply(request, 200, "ok");
        return;
    }
    if (verb == "share" || verb == "shareend" || verb == "push" || verb == "pushend")
    {
        takeSharing(request, verb, words);
        return;
    }
    if ((verb != "pose" && verb != "reflect") || count != 5)
    {
        reply(request, 400, "bad");
        return;
    }
    if (!gConnected)
    {
        reply(request, 403, "hello");
        return;
    }
    string kind = llList2String(words, 2);
    string id = llToLower(llList2String(words, 3));
    string whom = llToLower(llList2String(words, 4));
    if (llListFindList(ACTIONS, [kind]) == -1 || !isKey(id) || !isKey(whom) || whom == (string)gOwner)
    {
        reply(request, 400, "bad");
        return;
    }
    string said = "pose1|" + id + "|" + kind + "|" + whom;
    integer channel = POSE_CHANNEL;
    if (verb == "reflect")
    {
        said = "reflect1|" + id + "|" + kind + "|" + whom;
        channel = REFLECT_CHANNEL;
    }
    if (llStringLength(said) > POSE_MAX)
    {
        reply(request, 400, "bad");
        return;
    }
    llRegionSayTo((key)whom, channel, said);
    reply(request, 200, "ok");
}

// share, shareend, push, pushend from the viewer (the token and the hello were checked): the payload is not read, only
// its length and that nothing in it could break a line.
takeSharing(key request, string verb, list words)
{
    integer count = llGetListLength(words);
    integer toOne = (verb == "push" || verb == "pushend");
    integer ending = (verb == "shareend" || verb == "pushend");
    integer expected = 5;
    if (ending)
    {
        expected = 3;
        if (toOne)
        {
            expected = 4; // the target
        }
    }
    if (count != expected)
    {
        reply(request, 400, "bad");
        return;
    }
    if (!gConnected)
    {
        reply(request, 403, "hello");
        return;
    }
    // share|<token>|<aud>|<seq>|<payload>   shareend|<token>|<seq>
    // push|<token>|<target>|<seq>|<payload>  pushend|<token>|<target>|<seq>
    string whom = "";
    string aud = "0";
    integer at = 2;
    if (toOne)
    {
        whom = llToLower(llList2String(words, 2));
        at = 3;
    }
    else if (!ending)
    {
        aud = llList2String(words, 2);
        at = 3;
    }
    string seq = llList2String(words, at);
    string payload = "";
    if (!ending)
    {
        payload = llList2String(words, at + 1);
    }
    if ((aud != "0" && aud != "1") || !seqOk(seq) || !payloadOk(payload, ending))
    {
        reply(request, 400, "bad");
        return;
    }
    if (toOne && (!isKey(whom) || whom == (string)gOwner))
    {
        reply(request, 400, "bad");
        return;
    }
    if (!saidAllowed())
    {
        reply(request, 429, "rate");
        return;
    }
    if (toOne)
    {
        llRegionSayTo((key)whom, PUSH_CHANNEL, "push1|" + seq + "|" + payload);
    }
    else
    {
        llShout(SHARE_CHANNEL, "share1|" + aud + "|" + seq + "|" + payload);
    }
    reply(request, 200, "ok");
}

// The avatar wearing thing when it is another avatar's attachment in this region; NULL_KEY otherwise
// (not here, put on the ground, ours, or an avatar itself).
key wearerOf(key thing)
{
    list point = llGetObjectDetails(thing, [OBJECT_ATTACHED_POINT]);
    if (llGetListLength(point) != 1)
    {
        return NULL_KEY;
    }
    if (llList2Integer(point, 0) < 1)
    {
        return NULL_KEY;
    }
    key wearer = llGetOwnerKey(thing);
    if (wearer == thing || wearer == gOwner || !isKey((string)wearer))
    {
        return NULL_KEY;
    }
    return wearer;
}

// A request of asker (the text of its key, with the kind of message in front of it) is taken when its last one was
// ASK_GAP seconds ago or more. ASKERS_MAX are kept, the one seen first goes when a new one comes.
integer requestTaken(string asker)
{
    integer stamp = llGetUnixTime();
    integer slot = llListFindList(gAskers, [asker]);
    if (slot == -1)
    {
        if (llGetListLength(gAskers) >= ASKERS_MAX)
        {
            gAskers = llDeleteSubList(gAskers, 0, 0);
            gAskTimes = llDeleteSubList(gAskTimes, 0, 0);
        }
        gAskers += [asker];
        gAskTimes += [stamp];
        return TRUE;
    }
    if (stamp - llList2Integer(gAskTimes, slot) < ASK_GAP)
    {
        return FALSE;
    }
    gAskTimes = llListReplaceList(gAskTimes, [stamp], slot, slot);
    return TRUE;
}

// A HUD point (HUD Center 2 is 31, to HUD Bottom Right 38): the only place where this object's key is not public.
integer onHud()
{
    integer point = llGetAttached();
    return (point >= 31 && point <= 38);
}

// The owner's viewer calls (with this object's key, on a HUD point: not a dialog button, a link or anything else typed):
// from now on the bridge may say its URL. The URL held is told at once (at most once
// every PROBE_GAP seconds); a URL still to come is told when it comes (announce() of http_request).
takeProbe(key speaker, string text)
{
    if (speaker != gOwner || !onHud() || text != PROBE_TEXT + "|" + (string)llGetKey())
    {
        return;
    }
    gPaired = TRUE;
    integer stamp = llGetUnixTime();
    if (gLastReply != 0 && stamp - gLastReply < PROBE_GAP)
    {
        armTimer();
        return;
    }
    gLastReply = stamp;
    if (gUrl != "")
    {
        announce(FALSE);
    }
    else
    {
        if (gUrlTries > 0)
        {
            announceNoUrl();
        }
        armTimer();
    }
}

// A message heard on POSE_CHANNEL (head "pose1|", told as <auroraPose>) or on REFLECT_CHANNEL (head "reflect1|", told as
// <charisReflect>), told to the owner's viewer when it is such a message for us.
relayRequest(key thing, string text, string head, string open, string close)
{
    if (!gConnected || !llGetAttached())
    {
        return;
    }
    integer skip = llStringLength(head);
    if (llStringLength(text) > POSE_MAX || llGetSubString(text, 0, skip - 1) != head)
    {
        return;
    }
    list parts = llParseStringKeepNulls(llDeleteSubString(text, 0, skip - 1), ["|"], []);
    if (llGetListLength(parts) != 3)
    {
        return;
    }
    string id = llToLower(llList2String(parts, 0));
    string kind = llList2String(parts, 1);
    string whom = llToLower(llList2String(parts, 2));
    if (whom != (string)gOwner || llListFindList(ACTIONS, [kind]) == -1 || !isKey(id))
    {
        return;
    }
    key asker = wearerOf(thing);
    if (asker == NULL_KEY)
    {
        return;
    }
    // Only a request counts toward the gap (LSL evaluates both sides of && : the call stays in its own if).
    if (kind == "request")
    {
        if (!requestTaken(head + (string)asker))
        {
            return;
        }
    }
    llOwnerSay(open + id + "|" + kind + "|" + (string)asker + close);
}

// An avatar's share or push (the kind in front of its key) is taken when its last one was SHARE_GAP seconds ago or
// more. GAP_MAX are kept; a new avatar takes the place of one whose last time is older than the gap, and is not
// taken when there is none.
integer gapTaken(string who)
{
    float stamp = llGetTime();
    integer slot = llListFindList(gGapWho, [who]);
    if (slot == -1)
    {
        if (llGetListLength(gGapWho) >= GAP_MAX)
        {
            // Only a record older than the gap goes (it no longer holds anyone back); with GAP_MAX avatars all within
            // the gap, the new one is not taken.
            integer old = llGetListLength(gGapWho);
            integer found = -1;
            while (old > 0 && found == -1)
            {
                --old;
                if (stamp - llList2Float(gGapAt, old) >= SHARE_GAP)
                {
                    found = old;
                }
            }
            if (found == -1)
            {
                return FALSE;
            }
            gGapWho = llDeleteSubList(gGapWho, found, found);
            gGapAt = llDeleteSubList(gGapAt, found, found);
        }
        gGapWho += [who];
        gGapAt += [stamp];
        return TRUE;
    }
    if (stamp - llList2Float(gGapAt, slot) < SHARE_GAP)
    {
        return FALSE;
    }
    gGapAt = llListReplaceList(gGapAt, [stamp], slot, slot);
    return TRUE;
}

// A share ("share1|<aud>|<seq>|<payload>", shouted on SHARE_CHANNEL) or a push ("push1|<seq>|<payload>", said to us on
// PUSH_CHANNEL; no aud, so share is whether the first field is one) told to the owner's viewer. An empty payload is
// the end (aud 0).
relayPayload(key thing, string text, integer isShare)
{
    if (!gConnected || !llGetAttached())
    {
        return;
    }
    string head = "push1|";
    if (isShare)
    {
        head = "share1|";
    }
    integer skip = llStringLength(head);
    if (llStringLength(text) > SHARE_MAX || llGetSubString(text, 0, skip - 1) != head)
    {
        return;
    }
    // The fields are cut out with llSubStringIndex, not by splitting the line into a list: this is heard from anyone
    // near before anything is known of the speaker, and a line of 950 bars would make a list of 950 items.
    string rest = llDeleteSubString(text, 0, skip - 1);
    string aud = "0";
    integer bar = 0;
    if (isShare)
    {
        bar = llSubStringIndex(rest, "|");
        if (bar < 1)
        {
            return;
        }
        aud = llGetSubString(rest, 0, bar - 1);
        rest = llDeleteSubString(rest, 0, bar);
    }
    bar = llSubStringIndex(rest, "|");
    if (bar < 1)
    {
        return;
    }
    string seq = llGetSubString(rest, 0, bar - 1);
    string payload = llDeleteSubString(rest, 0, bar);
    if (llSubStringIndex(payload, "|") != -1)
    {
        return; // one field too many
    }
    if ((aud != "0" && aud != "1") || !seqOk(seq) || !payloadOk(payload, aud == "0"))
    {
        return;
    }
    key sender = wearerOf(thing);
    if (sender == NULL_KEY)
    {
        return;
    }
    if (!gapTaken(head + (string)sender))
    {
        return;
    }
    if (isShare)
    {
        llOwnerSay("<charisShare>1|" + (string)sender + "|" + aud + "|" + seq + "|" + payload + "</charisShare>");
    }
    else
    {
        llOwnerSay("<charisPush>1|" + (string)sender + "|" + seq + "|" + payload + "</charisPush>");
    }
}

default
{
    state_entry()
    {
        if (llGetAttached())
        {
            startBridge();
        }
    }

    on_rez(integer start_param)
    {
        gConnected = FALSE; // a new rez: no viewer said hello to it yet
        gPaired = FALSE;    // and none called it
        // Put on the ground instead of being worn: made temporary, it goes by itself after about a minute.
        if (!llGetAttached())
        {
            llSetLinkPrimitiveParamsFast(LINK_THIS, [PRIM_TEMP_ON_REZ, TRUE]);
        }
    }

    attach(key avatar)
    {
        if (avatar)
        {
            startBridge();
        }
        else
        {
            stopBridge();
        }
    }

    changed(integer change)
    {
        if (change & CHANGED_OWNER)
        {
            llResetScript();
        }
        if (change & CHANGED_INVENTORY)
        {
            stopOtherScripts();
        }
        if ((change & (CHANGED_REGION | CHANGED_TELEPORT | CHANGED_REGION_START)) && llGetAttached())
        {
            // The old URL is gone with the region: a new one (and a new token) is announced.
            gUrlTries = 0;
            askForUrl();
        }
    }

    http_request(key request, string method, string body)
    {
        if (method == URL_REQUEST_GRANTED)
        {
            if (request != gUrlRequest || !llGetAttached())
            {
                llReleaseURL(body);
                return;
            }
            gUrl = body;
            gUrlTries = 0;
            announce(TRUE); // nothing is said before the viewer called
        }
        else if (method == URL_REQUEST_DENIED)
        {
            if (request == gUrlRequest)
            {
                if (gUrlTries == 0)
                {
                    announceNoUrl();
                }
                ++gUrlTries;
                armTimer();
            }
        }
        else if (method == "POST")
        {
            takeCommand(request, body);
        }
        else
        {
            reply(request, 405, "method");
        }
    }

    listen(integer channel, string name, key id, string message)
    {
        if (channel == POSE_CHANNEL)
        {
            relayRequest(id, message, "pose1|", "<auroraPose>", "</auroraPose>");
        }
        else if (channel == REFLECT_CHANNEL)
        {
            relayRequest(id, message, "reflect1|", "<charisReflect>", "</charisReflect>");
        }
        else if (channel == SHARE_CHANNEL)
        {
            relayPayload(id, message, TRUE);
        }
        else if (channel == PUSH_CHANNEL)
        {
            relayPayload(id, message, FALSE);
        }
        else if (channel == PROBE_CHANNEL)
        {
            takeProbe(id, message);
        }
    }

    timer()
    {
        llSetTimerEvent(0.0);
        if (!llGetAttached())
        {
            return;
        }
        if (!gPaired && llGetUnixTime() >= gPairBy)
        {
            // No call came: this is not a Charis Viewer, or its bridge is off. The bridge (it said nothing) takes
            // itself off.
            llRequestPermissions(gOwner, PERMISSION_ATTACH);
            return;
        }
        if (gUrl == "")
        {
            if (gUrlTries == 0)
            {
                armTimer(); // the URL request is still on its way
                return;
            }
            askForUrl(); // the last request was refused
            return;
        }
        if (!gPaired || gConnected)
        {
            armTimer();
            return;
        }
        if (gAnnounced < ANNOUNCE_MAX)
        {
            announce(TRUE);
            return;
        }
        // The viewer called but never answered: the bridge takes itself off.
        llRequestPermissions(gOwner, PERMISSION_ATTACH);
    }

    run_time_permissions(integer granted)
    {
        if ((granted & PERMISSION_ATTACH) && !gConnected)
        {
            llDetachFromAvatar();
        }
    }
}
