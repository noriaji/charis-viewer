/**
 *
 * Copyright (c) 2009-2020, Kitty Barnett
 *
 * The source code in this file is provided to you under the terms of the
 * GNU Lesser General Public License, version 2.1, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. Terms of the LGPL can be found in doc/LGPL-licence.txt
 * in this distribution, or online at http://www.gnu.org/licenses/lgpl-2.1.txt
 *
 * By copying, modifying or distributing this software, you acknowledge that
 * you have read and understood your obligations described above, and agree to
 * abide by those obligations.
 *
 */

#ifndef RLV_EXPERIENCE_H
#define RLV_EXPERIENCE_H

// Charis: experience (temporary attachment) handling moved out of FS rlvhandler.cpp into rlvexperience.cpp. The RlvHandler
//         members (onExperienceEvent, onExperienceAttach, getAttachmentResourcesCoro) stay declared in rlvhandler.h.
//         This header holds only the response check, which depends on LLSD/LLUUID alone so the selftest can build it.

#include "llsd.h"
#include "lluuid.h"

#include <string>
#include <vector>

// Limits on the region's "AttachmentResources" capability response (research/248 6.2, 12 #9)
constexpr size_t RLV_ATTACHRES_MAX_ATTACHMENTS = 64;    // entries of "attachments" that are looked at
constexpr size_t RLV_ATTACHRES_MAX_OBJECTS     = 256;   // entries of each "objects" that are looked at
constexpr size_t RLV_ATTACHRES_MAX_NAME        = 255;   // bytes of an object name

struct RlvAttachResObject
{
    LLUUID      idObject;
    std::string strName;
};

// Returns the well-formed objects of an "AttachmentResources" response, in response order: a map holding an "attachments" array,
// each a map holding an "objects" array, each a map with a non-null UUID "id" (UUID or UUID string) and a string "name" of at most
// RLV_ATTACHRES_MAX_NAME bytes. Anything else, and entries past the count limits, is skipped. Names are compared exactly by the caller.
std::vector<RlvAttachResObject> rlvValidateAttachmentResources(const LLSD& sdResult);

#endif // RLV_EXPERIENCE_H
