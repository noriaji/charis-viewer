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
"""Gets ONNX Runtime 1.23.2 (CPU, Windows x64, Microsoft's release) for charis-motion: the headers and
onnxruntime.dll, with its LICENSE and ThirdPartyNotices.txt, into third_party/onnxruntime-win-x64-1.23.2/.

  python -I fetch-onnxruntime.py

The release zip is fetched once (one GET over https from github.com; nothing is sent but the request) into
third_party/, and kept: a later run that finds it with the pinned SHA-256 does not go online. The zip and the dll
must match the pinned size and SHA-256, or nothing is extracted. Only the listed files are taken from the zip.
Redirects are followed to https only. A failed or cut download (a network error, a timeout, a redirect away from
https, the wrong size or hash) leaves no .part and no zip behind.
Exit status: 0 = ready, 1 = the download or a check failed.
"""
import hashlib
import http.client
import os
import sys
import urllib.request
import zipfile

VERSION = "1.23.2"
NAME = "onnxruntime-win-x64-" + VERSION
URL = "https://github.com/microsoft/onnxruntime/releases/download/v%s/%s.zip" % (VERSION, NAME)
# The pins were taken from the first fetch (2026-11-13) and checked against another source the same day: the
# extracted onnxruntime.dll carries a valid Authenticode signature (Get-AuthenticodeSignature: Status Valid, signer
# CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US, issuer CN=Microsoft Code Signing
# PCA 2011, certificate valid to 2026-06-18 and time-stamped by the Microsoft Time-Stamp Service, so it stays Valid
# after that), and its SHA-256 is DLL_SHA256. The GitHub release's published digest was not compared (the signer
# check was taken as enough). When the version goes up, check the new dll's signature the same way before pinning.
ZIP_BYTES = 78127794
ZIP_SHA256 = "0b38df9af21834e41e73d602d90db5cb06dbd1ca618948b8f1d66d607ac9f3cd"
DLL_BYTES = 14186016
DLL_SHA256 = "dec964ab1ee36cc9b0ae247d13b376627992fc57dec0454354017ab8fd84f1ea"
# The zip's ThirdPartyNotices.txt (shipped with the dll; the release check compares the file it ships with these).
NOTICES_BYTES = 333022
NOTICES_SHA256 = "e00f828e0a33de591a355ae6606d2625f5758da7d2c844db7821c9dd3e3647b6"
# What is taken from the zip (the rest: import libraries, pdbs, the shared-provider dll, is not needed).
MEMBERS = ["include/onnxruntime_c_api.h", "include/onnxruntime_cxx_api.h", "include/onnxruntime_cxx_inline.h",
           "include/onnxruntime_float16.h", "include/onnxruntime_ep_c_api.h",
           "include/onnxruntime_session_options_config_keys.h", "include/onnxruntime_run_options_config_keys.h",
           "lib/onnxruntime.dll", "LICENSE", "ThirdPartyNotices.txt", "VERSION_NUMBER", "GIT_COMMIT_ID"]

HERE = os.path.dirname(os.path.abspath(__file__))
THIRD = os.path.join(HERE, "third_party")
ZIP = os.path.join(THIRD, NAME + ".zip")
OUT = os.path.join(THIRD, NAME)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def zip_ok():
    return os.path.isfile(ZIP) and os.path.getsize(ZIP) == ZIP_BYTES and sha256_file(ZIP) == ZIP_SHA256


class HttpsOnly(urllib.request.HTTPRedirectHandler):
    """Follows a redirect only to another https URL (GitHub sends the file from its download host)."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not newurl.startswith("https://"):
            raise OSError("redirected away from https")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def download():
    """The zip into ZIP, or an OSError (a network failure, a timeout, a redirect away from https, the wrong
    size). Whatever happens, no .part is left behind."""
    part = ZIP + ".part"
    req = urllib.request.Request(URL, headers={"User-Agent": "build-script"})
    opener = urllib.request.build_opener(HttpsOnly)
    got = 0
    try:
        with opener.open(req, timeout=120) as r, open(part, "wb") as f:
            if not r.geturl().startswith("https://"):
                raise OSError("redirected away from https")
            while True:
                block = r.read(1 << 20)
                if not block:
                    break
                got += len(block)
                if got > ZIP_BYTES:
                    break
                f.write(block)
        if got != ZIP_BYTES:
            raise OSError("got %d bytes or more, expected %d" % (got, ZIP_BYTES))
        os.replace(part, ZIP)
    finally:
        if os.path.exists(part):
            os.remove(part)


def main():
    os.makedirs(THIRD, exist_ok=True)
    if not zip_ok():
        if os.path.exists(ZIP):
            os.remove(ZIP)
        print("fetching", URL)
        try:
            download()
        except (OSError, ValueError, http.client.HTTPException) as e:   # URLError and timeouts are OSErrors; a cut
                                                                         # or garbled reply is an HTTPException
            print("NG  the download failed: %s" % e)
            return 1
        if not zip_ok():
            os.remove(ZIP)
            print("NG  the zip is not the pinned one (size or SHA-256)")
            return 1
    with zipfile.ZipFile(ZIP) as z:
        names = set(z.namelist())
        for m in MEMBERS:
            src = NAME + "/" + m
            if src not in names:
                print("NG  the zip has no", src)
                return 1
        dll = z.read(NAME + "/lib/onnxruntime.dll")
        if len(dll) != DLL_BYTES or hashlib.sha256(dll).hexdigest() != DLL_SHA256:
            print("NG  onnxruntime.dll is not the pinned one")
            return 1
        notices = z.read(NAME + "/ThirdPartyNotices.txt")
        if len(notices) != NOTICES_BYTES or hashlib.sha256(notices).hexdigest() != NOTICES_SHA256:
            print("NG  ThirdPartyNotices.txt is not the pinned one")
            return 1
        for m in MEMBERS:
            dest = os.path.join(OUT, *m.split("/"))   # fixed names: nothing from the zip becomes a path
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as f:
                f.write(z.read(NAME + "/" + m))
    print("OK  %s in %s (onnxruntime.dll sha256 %s)" % (NAME, os.path.relpath(OUT, HERE), DLL_SHA256))
    return 0


if __name__ == "__main__":
    sys.exit(main())
