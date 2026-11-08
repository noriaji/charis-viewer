/**
 * @file lltpfaillog.cpp
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

#include "llviewerprecompiledheaders.h"

#include "lltpfaillog.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llcircuit.h"
#include "llhost.h"
#include "message.h"
#include "llstartup.h"
#include "lltimer.h"
#include "lltrans.h"
#include "llviewerregion.h"
#include "llworld.h"

#include <sstream>

namespace
{
    // A disconnect this long after TeleportFinish still counts as teleport related.
    constexpr F64 TP_RELATED_WINDOW_SECS = 120.0;

    bool        sHaveFinish = false;
    F64         sFinishTime = 0.0;
    LLHost      sDestHost;
    U64         sDestHandle = 0;
    std::string sSourceName;
    LLHost      sSourceHost;
    LLVector3d  sSourcePosGlobal;

    void describeCircuit(std::ostream& out, const char* label, LLHost host)
    {
        out << " " << label << "=" << host;
        if (host.isInvalid())
        {
            return;
        }
        LLCircuitData* cdp = gMessageSystem ? gMessageSystem->mCircuitInfo.findCircuit(host) : nullptr;
        if (!cdp)
        {
            out << "(no circuit)";
            return;
        }
        F64 now = LLMessageSystem::getMessageTimeSeconds().value();
        out << "(alive=" << (cdp->isAlive() ? 1 : 0)
            << " last_in=" << (now - cdp->getLastPacketInTime().value()) << "s ago"
            << " ping=" << cdp->getPingDelay().value() << "ms"
            << " pings_in_transit=" << cdp->getPingsInTransit()
            << " lost=" << cdp->getPacketsLost()
            << " unacked=" << cdp->getUnackedPacketCount()
            << " age=" << cdp->getAgeInSeconds() << "s)";
    }
}

void LLTPFailLog::noteTeleportFinish(const LLHost& dest_host, U64 dest_handle)
{
    sHaveFinish = true;
    sFinishTime = LLTimer::getTotalSeconds();
    sDestHost = dest_host;
    sDestHandle = dest_handle;

    LLViewerRegion* src = gAgent.getRegion();
    sSourceName = src ? src->getName() : std::string("(none)");
    sSourceHost = src ? src->getHost() : LLHost();
    sSourcePosGlobal = gAgent.getPositionGlobal();
}

void LLTPFailLog::noteDisconnect(const std::string& mesg)
{
    if (gDoDisconnect || LLStartUp::getStartupState() < STATE_STARTED)
    {
        return; // only the first disconnect after login is interesting
    }

    S32 tp_state = gAgent.getTeleportState();
    F64 since_finish = sHaveFinish ? F64(LLTimer::getTotalSeconds()) - sFinishTime : -1.0;
    bool tp_related = tp_state != LLAgent::TELEPORT_NONE
        || (sHaveFinish && since_finish < TP_RELATED_WINDOW_SECS);

    LLViewerRegion* agent_region = gAgent.getRegion();
    bool region_alive = agent_region && agent_region->isAlive();

    // Best guess at who gave up. "viewer" means the server may still hold the session
    // (stage R1 can retry), "server" means a fresh login is needed.
    // Callers pass the translated text, so compare against the translations.
    std::string trigger;
    if (mesg == LLTrans::getString("AgentLostConnection"))
    {
        trigger = region_alive ? "eventpoll-stall (viewer)" : "circuit-timeout (viewer, no packets from sim)";
    }
    else if (mesg == LLTrans::getString("YouHaveBeenDisconnected"))
    {
        trigger = "region-disabled (server DisableSimulator or OS hibernation)";
    }
    else if (mesg == LLTrans::getString("SentToInvalidRegion"))
    {
        trigger = "invalid-region (viewer)";
    }
    else if (mesg == LLTrans::getString("TestingDisconnect"))
    {
        trigger = "test menu";
    }
    else
    {
        trigger = "server kick or bad network data";
    }

    std::ostringstream out;
    out << "disconnect tp_related=" << (tp_related ? 1 : 0)
        << " trigger=\"" << trigger << "\""
        << " mesg=\"" << mesg << "\""
        << " tp_state=" << LLAgent::teleportStateName(tp_state);
    if (sHaveFinish)
    {
        out << " since_tp_finish=" << since_finish << "s"
            << " source=\"" << sSourceName << "\" source_pos=" << sSourcePosGlobal
            << " dest_handle=" << sDestHandle;
        describeCircuit(out, "dest", sDestHost);
        describeCircuit(out, "source", sSourceHost);
    }
    if (agent_region)
    {
        out << " agent_region=\"" << agent_region->getName() << "\" alive=" << (region_alive ? 1 : 0);
        describeCircuit(out, "agent", agent_region->getHost());
    }
    else
    {
        out << " agent_region=(none)";
    }
    out << " regions=" << LLWorld::getInstance()->getRegionList().size();

    LL_WARNS("TPFail") << out.str() << LL_ENDL;
}
