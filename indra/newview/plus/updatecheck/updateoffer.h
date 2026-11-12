/**
 * @file updateoffer.h
 * @brief Update check: the update file read into its fields, and the copy of
 *        them kept in PlusUpdateCheckOffer (boost::json only, no viewer
 *        dependencies).
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

// The fields of the update file are in research/123-update-check.md, section 1.
// The copy kept between starts (research/325 4.2) has the same form as the
// file, with only the fields below: it comes from the server, so every start
// reads it with readOfferFile() again (the size, the version, the addresses and
// their hosts checked as for an answer) before it is used.

#ifndef PLUS_UPDATECHECK_UPDATEOFFER_H
#define PLUS_UPDATECHECK_UPDATEOFFER_H

#include "updateversion.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace plus::updatecheck
{
    constexpr size_t MAX_OFFER_BYTES   = 16 * 1024;   // the file, and the copy kept of it
    constexpr size_t MAX_MESSAGE_BYTES = 1000;        // message_ja, message_en each

    // What an update file says, checked.
    struct OfferFile
    {
        Version     version;
        bool        has_min_version = false;    // min_version given (and of the form 0.1.0)
        int         min_major = 0, min_minor = 0, min_patch = 0;
        std::string download_url;               // a web URL on an allowed host, or ""
        std::string notes_url;                  // the same
        std::string message_ja;                 // at most MAX_MESSAGE_BYTES, cut at a whole character
        std::string message_en;
    };

    // Reads an update file (an answer, or the copy kept in PlusUpdateCheckOffer).
    // false, with why, when it is not one: more than MAX_OFFER_BYTES, not a JSON
    // object, no "version" of the form 0.1.0, an unknown "stage", a bad
    // "stage_number" or "build". A field that is only left out (an address that
    // may not be opened, or on a host not in allowed_hosts; a min_version of
    // another form) adds a line to notes and the rest is read.
    bool readOfferFile(std::string_view body, std::string_view allowed_hosts, OfferFile& out, std::string& why,
                       std::vector<std::string>& notes);

    // The copy kept in PlusUpdateCheckOffer: the fields of offer in the form
    // of the update file, so readOfferFile() reads it back.
    std::string writeOfferFile(const OfferFile& offer);

    // text cut to at most max_bytes, at the start of a UTF-8 character.
    std::string truncateUtf8(std::string_view text, size_t max_bytes);
}

#endif // PLUS_UPDATECHECK_UPDATEOFFER_H
