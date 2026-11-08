/**
 * @file lltpfaillog.h
 * @brief Records why the viewer disconnected during or right after a teleport.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Charis Viewer Source Code
 * Copyright (C) 2026, Charis Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef LL_LLTPFAILLOG_H
#define LL_LLTPFAILLOG_H

#include "stdtypes.h"

#include <string>

class LLHost;

// Event-only logging (research/43 stage R0). Nothing runs per frame.
// Look for "TPFail" in SecondLife.log after a teleport ends in a logout.
namespace LLTPFailLog
{
    // Called from process_teleport_finish, before the viewer contacts the destination.
    void noteTeleportFinish(const LLHost& dest_host, U64 dest_handle);

    // Called at the top of LLAppViewer::forceDisconnect.
    void noteDisconnect(const std::string& mesg);
}

#endif // LL_LLTPFAILLOG_H
