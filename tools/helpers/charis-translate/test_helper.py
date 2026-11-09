# $LicenseInfo:firstyear=2026&license=viewerlgpl$
# Second Life Viewer Source Code
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation;
# version 2.1 of the License only.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
# $/LicenseInfo$

# Drives out\charis-translate.exe through its pipe protocol and checks every output line is JSON,
# the answers carry the request ids, bad input gets an error line, and a closed stdin ends the process.
import json, os, subprocess, sys, time
here = os.path.dirname(os.path.abspath(__file__))
exe = os.path.join(here, "out", "charis-translate.exe")
models = os.path.join(here, "models", "pack")
def run(lines, close=True, timeout=60):
    p = subprocess.Popen([exe, "--models", models, "-t", "2"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    out, _ = p.communicate("".join(l + "\n" for l in lines).encode("utf-8"), timeout=timeout)
    return p.returncode, [json.loads(x) for x in out.decode("utf-8").splitlines()]
fail = 0
def check(cond, what):
    global fail
    print(("OK   " if cond else "FAIL ") + what)
    fail += 0 if cond else 1
rc, o = run([
    json.dumps({"id": 1, "from": "en", "to": "ja", "text": "Nice to meet you!"}),
    json.dumps({"id": 2, "from": "ja", "to": "en", "text": "こんにちは、今日は元気ですか？"}),
    json.dumps({"id": 3, "from": "en", "to": "ja", "text": "Quote \"this\" and \ slash 😀"}),
    json.dumps({"id": 4, "from": "en", "to": "xx", "text": "hello"}),
    json.dumps({"id": 5, "from": "en", "to": "ja"}),
    "not json",
    json.dumps({"id": 6, "from": "en", "to": "ja", "text": ""}),
    json.dumps({"id": 8, "from": "../..", "to": "ja", "text": "hello"}),
    json.dumps({"id": 9, "from": "en", "to": "J A", "text": "hello"}),
    json.dumps({"id": 10, "from": "EN", "to": "ja", "text": "hello"}),
    json.dumps({"id": 7, "from": "en", "to": "ja", "text": "\u3053\u3093\u306b\u3061\u306f \ud83d\ude00".replace("\\\\", "\\")}),
])
for x in o: print("  ", x)
check(rc == 0, "exits 0 when stdin closes")
check(o[0]["type"] == "ready", "first line is ready")
t = {x["id"]: x for x in o if x["type"] == "text"}
e = {x["id"]: x for x in o if x["type"] == "error"}
check(1 in t and t[1]["text"], "en->ja answers id 1: %r" % t.get(1, {}).get("text"))
check(2 in t and "Hello" in t[2]["text"], "ja->en answers id 2: %r" % t.get(2, {}).get("text"))
check(3 in t, "quotes / backslash / emoji survive")
check(4 in e, "unsupported direction -> error")
check(5 in e, "missing text -> error")
check(-1 in e, "bad line -> error with id -1")
check(6 in t or 6 in e, "empty text is answered")
check(8 in e and 9 in e and 10 in e and 8 not in t and 9 not in t and 10 not in t, "language codes that are not 2-3 small letters are refused (audit C I3)")
check(7 in t or 7 in e, "unicode escapes are parsed")
# speed: 25 sentences in one session
sents = ["Hi, how are you doing today?", "Can you hear me okay?", "Where did you get that hair?", "I'm just waiting for my friend to log in.",
         "Thanks for the invite, I'll be right there.", "Is this your house? It's beautiful.", "What time is the event starting?", "Let's take a picture together."] * 3
rc, o = run([json.dumps({"id": i, "from": "en", "to": "ja", "text": s}) for i, s in enumerate(sents)])
ms = [x["compute_ms"] for x in o if x["type"] == "text"]
load = [x["load_ms"] for x in o if x["type"] == "loaded"]
check(len(ms) == len(sents), "all %d sentences answered" % len(sents))
print("   load_ms", load, " compute_ms avg %.0f max %d" % (sum(ms) / len(ms), max(ms)))
# quit command ends it at once
p = subprocess.Popen([exe, "--models", models], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
p.stdin.write(b'{"cmd":"quit"}\n'); p.stdin.flush()
t0 = time.time(); rc = p.wait(timeout=10)
check(rc == 0 and time.time() - t0 < 5, "quit exits at once")
sys.exit(1 if fail else 0)
