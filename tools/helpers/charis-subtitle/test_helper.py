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

"""Feeds a WAV file to out/charis-subtitle.exe through its pipe protocol and prints the JSON lines.

  python test_helper.py FILE.wav [--lang ja] [--realtime] [--threads 4] [--voiced] [--flush-at S]
                                [--maxbacklog S] [--model models/ggml-small.bin]

--voiced     sends VOICED frames from a simple loudness test (what the viewer will do from the voice
             participants' flags) instead of letting the helper judge by loudness
--flush-at   sends a FLUSH frame at that second of audio
Checks: every line is valid JSON, and the helper exits by itself when the input ends.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import threading
import time
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
RATE = 16000


def load(path):
    with wave.open(path) as w:
        ch, sw, sr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    assert sw == 2, "16 bit WAV only"
    x = np.frombuffer(raw, dtype="<i2").astype(np.float32).reshape(-1, ch).mean(axis=1)
    if sr != RATE:
        t = np.arange(int(len(x) * RATE / sr)) * sr / RATE
        x = np.interp(t, np.arange(len(x)), x)
    return x.astype("<i2")


def frame(kind, payload=b""):
    return struct.pack("<BI", kind, len(payload)) + payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--model", default=os.path.join(HERE, "models", "ggml-small.bin"))
    ap.add_argument("--lang", default="auto")
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--realtime", action="store_true")
    ap.add_argument("--voiced", action="store_true")
    ap.add_argument("--flush-at", type=float, default=None)
    ap.add_argument("--maxbacklog", type=float, default=None)
    a = ap.parse_args()

    pcm = load(a.wav)
    cmd = [os.path.join(HERE, "out", "charis-subtitle.exe"), "-m", a.model, "-t", str(a.threads), "-l", a.lang]
    if a.maxbacklog is not None:
        cmd += ["--maxbacklog", str(a.maxbacklog)]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    lines, bad = [], []

    def read_out():
        for line in p.stdout:
            try:
                lines.append(json.loads(line.decode("utf-8")))
                sys.stdout.buffer.write(line.rstrip() + b"\n")
                sys.stdout.buffer.flush()
            except Exception:
                bad.append(line)

    th = threading.Thread(target=read_out)
    th.start()

    step = RATE // 10
    t0 = time.time()
    flushed = False
    for i in range(0, len(pcm), step):
        chunk = pcm[i:i + step]
        if a.voiced:
            loud = float(np.sqrt(np.mean((chunk.astype(np.float32) / 32768.0) ** 2))) > 0.010
            p.stdin.write(frame(4, b"\x01" if loud else b"\x00"))
        p.stdin.write(frame(1, chunk.tobytes()))
        if a.flush_at is not None and not flushed and i / RATE >= a.flush_at:
            p.stdin.write(frame(2))
            flushed = True
        if a.realtime:
            due = (i + len(chunk)) / RATE
            now = time.time() - t0
            if due > now:
                time.sleep(due - now)
    p.stdin.close()  # end of input: the helper finishes the last piece and exits by itself
    t_end = time.time()
    try:
        rc = p.wait(timeout=120)
    except subprocess.TimeoutExpired:
        p.kill()
        print("FAIL: helper did not exit after end of input")
        return 1
    th.join()
    print(f"-- exit code {rc}, {time.time() - t_end:.1f} s from end of input to exit, {len(lines)} lines")
    if bad:
        print("FAIL: lines that are not JSON:", bad)
        return 1
    return 0 if rc == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
