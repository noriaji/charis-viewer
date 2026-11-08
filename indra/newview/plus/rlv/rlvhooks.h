/**
 * @file rlvhooks.h
 * @brief The calls the official viewer files make into RLVa (plus/rlv), research/248.
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

#ifndef PLUS_RLV_RLVHOOKS_H
#define PLUS_RLV_RLVHOOKS_H

#include <string>
#include <vector>

#include "llchat.h" // EChatType and LLChat (small header of llui)

class LLAvatarName;
class LLInventoryItem;
class LLOfferInfo;
class LLSD;
class LLUIString;
class LLUUID;
class LLVector3d;
class LLViewerInventoryCategory;
class LLViewerInventoryItem;
class LLViewerJointAttachment;
class LLViewerObject;
template <class Type> class LLPointer;

// The only header of plus/rlv that official files include. Forward declarations only: no heavy headers here.
// Every hook is called as  "if (plus::rlv::enabled() && plus::rlv::xxx(...))"  so that with RLVa off (the
// default) it costs one bool. Hooks of one branch live in its own section and its own rlvhooks_<branch>.cpp.
// Main thread only.
namespace plus::rlv
{
    // True when RLVa is on (RestrainedLove was set when the login began; changing it later needs a restart).
    bool enabled();

    // ================================================================ core (rlvhooks_core.cpp)

    // process_chat_from_simulator() (llviewermessage.cpp)
    // llOwnerSay lines of an RLVa user are decided once their text is read: the speech particles of
    // such a line are not made with the others.
    bool holdsChatParticles(int chat_type);
    // @recvchat / @recvemote text, @shownames speaker and text names, @showloc places, the "objectim"
    // link. May change chat and mesg. True: the line is dropped (llviewermessage.cpp returns).
    bool filterChat(LLChat& chat, std::string& mesg, const LLUUID& from_id, const LLUUID& owner_id, LLViewerObject* chatter);
    // llOwnerSay "@cmd,cmd" lines of the agent's own objects are RLV commands. True: the line is
    // consumed (nothing to show). False: show it (with RestrainedLoveDebug chat.mText/mesg hold the result).
    bool handleOwnerSay(LLChat& chat, std::string& mesg, const LLUUID& from_id, const LLUUID& owner_id, LLViewerObject* chatter);

    // LLStartUp::setStartupState() via plus::onStartupState() (plus.cpp): RLVa is switched on at
    // STATE_LOGIN_CLEANUP (the setting's value at that moment), and its login-time work starts at STATE_MISC.
    void onStartupState(int from, int to);

    // plus/grouptitles (Auto Region, the group titles window): the active group may be changed, i.e. no
    // @setgroup holds it (Firestorm's RlvActions::canChangeActiveGroup()). @setgroup is not in Charis yet
    // (stage 3): true until it comes, then the hold applies without a change there.
    bool canChangeActiveGroup();

    // idle_startup() (llstartup.cpp): RLVa on => always show the login screen (so it can be turned off).
    bool mustShowLoginScreen();
    // idle_startup(): after @tploc / @unsit ended a session, RLVaLoginLastLocation makes the next login
    // start at the last location.
    void forceLastLocationLogin();

    // LLAppViewer::cleanup(), first thing: drops the handler's observers, timers and signals.
    void cleanup();

    // For rlv-locks: called after a line's commands were all processed (RlvForceWear::done() there).
    typedef void (*command_batch_done_fn)();
    void setCommandBatchDone(command_batch_done_fn fn);

    // ================================================================ locks (rlvhooks_locks.cpp)

    // LLViewerObject::processUpdateMessage() (llviewerobject.cpp, W4): an object got our avatar as its parent, i.e. it is rezzing
    // as our attachment (llAttachToAvatar). On an "add locked" point the watchdog treats it as a user wear.
    void onRezzedAsAttachment(LLViewerObject* pObj);
    // LLVOAvatarSelf::attachObject() (llvoavatarself.cpp, W1): the object is on its attachment point now.
    // @detach locks, the lock watchdog (reattach on a forced detach), the locked HUD.
    void onAttachObject(LLViewerObject* pAttachObj, const LLViewerJointAttachment* pAttachPt);
    // LLVOAvatarSelf::detachObject() (W2): before LLVOAvatar::detachObject() takes the object off its point.
    void onDetachObject(LLViewerObject* pAttachObj);
    // LLVOAvatarSelf::detachObject() (W2): after it was taken off (a locked HUD may be gone).
    void onObjectDetached(LLViewerObject* pAttachObj);
    // LLVOAvatarSelf::detachAttachmentIntoInventory() (W3): false when the attachment item is locked.
    bool canDetachAttachment(const LLInventoryItem* pItem);
    // LLAttachmentsMgr::addAttachmentRequest() (rlv-wear, W5): true while the lock watchdog reattaches a forcefully detached
    // attachment - FS' fRlvForce argument. Such a request skips the lock checks.
    bool isForcedAttachRequest();
    // LLInventoryModel (rlv-wear, where FS calls RlvAttachmentLockWatchdog::onSavedAssetIntoInventory): a detached
    // attachment's asset was saved, so the watchdog can reattach it at once instead of after its 15 second timeout.
    void onSavedAssetIntoInventory(const LLUUID& idItem);

    // ================================================================ comm (rlvhooks_comm.cpp)
    // Chat, IM, teleport, script permissions, offers (research/248 B, C, D, E46-E48, W23, W24). Same places as in FS.

    // ---- B: sending chat
    // send_chat_from_viewer() (llfloaterimnearbychat.cpp, B1): @sendchat (text hidden), @redirchat / @rediremote (text
    // redirected), @sendchannel(_except), the chat volume, the debug channel. May change text and type. True: not sent.
    bool filterSentChat(std::string& utf8_out_text, EChatType& type, int channel);
    // LLFloaterIMNearbyChat::sendChatFromViewer() (B2): the volume of public chat and gestures (@chatwhisper, @chatnormal, @chatshout)
    EChatType checkChatVolume(EChatType type);
    // (B2) false while chat (an emote) is redirected: the talking animation is not played
    bool allowsChatAnimation(const std::string& utf8_text);
    // LLAgent::startTyping() (B3): false under @redirchat unless the user wants the indicator (RLVaShowRedirectChatTyping)
    bool canSendTypingStart();
    // callback_script_dialog() (B4) / LLScriptHandler::processNotification() (B5): a dialog that would answer on the nearby
    // chat channel (0) is blocked under @sendchat. The second one tells the user.
    bool blocksScriptDialogReply(int channel);
    bool blocksScriptDialog(int channel);

    // ---- C: IM
    // LLIMProcessing::processNewMessage() (llimprocessing.cpp, C1): an IM that is a command to RLVa (@version, @stopim, @list,
    // @except) is answered and not shown. True: eat it. fOnline false: the IM was stored (never answered).
    bool handleIMQuery(const LLUUID& idSender, const std::string& strSenderName, const std::string& strMessage, bool fOnline);
    // @recvim from this sender (C1, C10, the busy answer)
    bool canReceiveIM(const LLUUID& idSender);
    // The text that replaces what @recvim hides
    std::string receivedIMBlockedText();
    // processNewMessage() IM_NOTHING_SPECIAL (C1): @recvim - replaces strMessage, tells the sender. True: replaced.
    bool filterReceivedIM(const LLUUID& idSender, bool fOffline, bool fLinden, bool fMuted, const LLUUID& idSession, std::string& strMessage);
    // LLIMModel::sendMessage() (llimview.cpp, C4): @sendim / @sendimto - the text is replaced when the recipient (every
    // other member of a conference) may not be sent an IM.
    // nDialog is the IM dialog: IM_NOTHING_SPECIAL is always one resident (idOther), whatever the session says.
    void filterSentIM(int nDialog, const LLUUID& idSession, const LLUUID& idOther, std::string& strText);
    // Other texts that go out as an IM (friendship request, group notice, payment message): replaced when idRecipient may not be sent an IM
    void filterOutgoingIMText(const LLUUID& idRecipient, std::string& strText);
    // LLViewerChatterBoxInvitation::post() (C3): @recvim - false: a group invitation is not accepted. A conference one is censored.
    bool filterInvitationText(const LLUUID& idSession, const LLUUID& idFrom, std::string& strMessage);
    // @startim: true = refused (the user was told). LLAvatarActions::startIM()/startCall() (C5), startAdhocCall()/startConference() (C5),
    // LLGroupActions (C6), LLInventoryPanel::beginIMSession() (C7)
    bool refuseStartIM(const LLUUID& idRecipient);
    bool refuseStartConference(const std::vector<LLUUID>& idRecipients);
    bool refuseStartGroupIM(const LLUUID& idGroup);
    // LLAlertHandler / LLOfferHandler (C8): an IM session may be opened for a toast
    bool canStartIM(const LLUUID& idRecipient);
    // LLAvatarActions::teleport_request_callback() (C5)
    void filterTeleportRequestText(const LLUUID& idRecipient, std::string& strText);
    // LLFloaterPay::payDirectly() (C9) / LLFloaterPreference::refreshEnabledState() (C11)
    bool canSendIM(const LLUUID& idRecipient);
    bool canChangeDoNotDisturbResponse();

    // Stored history (LL's chat service, the group / conference history): @recvim keeps the text of someone it keeps out from
    // showing (C12). Main thread only. sdText is replaced by the "blocked" text.
    // idSession: the conversation (the resident for a one-on-one one, the group or conference id). For a group it is the group
    // that is looked at (as when the invitation came in, C3), otherwise the sender.
    void filterHistoryText(const LLUUID& idFrom, const LLUUID& idSession, LLSD& sdText);

    // ---- D: teleport and permissions
    // LLAgent::teleportViaLandmark() (llagent.cpp, D1): true = refused (the user was told)
    bool refuseTeleportToLandmark(bool fHome);
    // LLAgent::teleportViaLocation() / teleportViaLocationLookAt() (D2): true = refused.
    bool refuseTeleportToLocation(const LLVector3d& posGlobal);
    // The same, after startTeleportRequest(): a @tpto teleport that did begin is made uncancellable (one that did not
    // leaves the flag alone; teleport finished / failed sets it back).
    void noCancelForStartedTpto();
    // A teleport that is started again (LLTeleportRequestVia*::restartTeleport(), llagent.cpp) and the accepting of an offer
    // (lure_callback(), llviewermessage.cpp): the restrictions are looked at again. Teleports that cannot be cancelled
    // (@tpto, @accepttp) are not asked again.
    // A teleport RLVa itself makes (@tpto=force, the way back of @sittp / @standtp) is marked when its request is made and
    // is not asked again (LLAgent::startTeleportRequest()).
    bool isRlvMadeTeleport();
    bool refuseLureAccept(const LLUUID& idFrom);              // lure_callback(), mature_lure_callback()
    bool refuseTeleportRequestAccept(const LLUUID& idFrom);   // teleport_request_callback(): the answer yes (@tprequest)
    // process_teleport_start() / process_teleport_progress() (llviewermessage.cpp, D5): false = no cancel button
    bool canCancelTeleport();
    // processNewMessage() IM_LURE_USER / IM_TELEPORT_REQUEST (llimprocessing.cpp, D3)
    bool autoAcceptTeleport(int nDialog, const LLUUID& idFrom);                               // @accepttp / @accepttprequest
    bool blocksTeleportOffer(int nDialog, const LLUUID& idFrom);                              // @tplure / @tprequest: declined, the sender is told
    void censorTeleportOfferText(int nDialog, const LLUUID& idFrom, std::string& strMessage); // @recvim / @showloc
    void noCancelForTeleport();                                                               // an accepted offer cannot be cancelled
    // LLAvatarActions::canOfferTeleport() (llavataractions.cpp, D8): under @showloc only to those who are @tplure exceptions or can map this agent
    bool restrictsTeleportOffers();
    bool mayOfferTeleportRestricted(const LLUUID& idAgent);
    // handle_lure() / send_lures() (llviewermessage.cpp, D4). prepareTeleportOffer(): false = nothing is offered.
    bool prepareTeleportOffer(const std::vector<LLUUID>& idRecipients, LLSD& sdEditArgs, LLSD& sdPayload);
    void filterLureText(const LLSD& sdRecipients, std::string& strText);
    std::string anonymizedName(const LLUUID& idAgent);
    // process_script_teleport_request() (D6): @showworldmap
    bool canShowWorldMap();
    // process_script_question() (D7): permissions withheld (attach, teleport) and @acceptpermission; sets sdPayload["questions"]
    void filterScriptQuestions(const LLUUID& idTask, S32& nQuestions, LLSD& sdPayload);
    bool hideNoticeLocation(LLUIString& notice);                                              // notify_cautioned_script_question(): @showloc
    void tellGrantedPermission(const std::string& strNotice);                                 // what @acceptpermission granted
    void notifyBlockedPermission(const std::string& strKey, const std::string& strObjectName);

    // ---- E: hiding names and places in the communication code (E46-E48)
    // true = the sender may be shown by name (a nearby stranger under @shownames may not, unless an IM or the profile is open)
    bool canAddToRecentPeople(const LLUUID& idAgent);
    std::string offerOwnerName(const LLUUID& idOwner, const LLAvatarName& avName);            // LLOfferInfo::inventory_task_offer_callback()
    void filterSystemText(std::string& strText);                                              // process_alert_core(): @showloc and @shownames
    std::string meanCollisionName(const LLAvatarName& avName);                                // mean_name_callback()
    void anonymizeObjectOfferOwner(const LLOfferInfo& offer, LLSD& sdArgs, LLSD& sdPayload);  // inventory_offer_handler()
    void anonymizeAgentOfferSender(const LLOfferInfo& offer, LLSD& sdArgs, LLSD& sdPayload);
    void anonymizeNearbyName(const LLUUID& idAgent, const std::string& strOriginalName, LLSD& sdArgs); // IM_INVENTORY_ACCEPTED / DECLINED
    void filterObjectIM(bool fFromGroup, const LLUUID& idOwner, std::string& strName, std::string& strLocation, LLSD& sdQuery, LLChat& chat); // IM_FROM_TASK
    void anonymizeTeleportRequestName(const LLUUID& idAgent, const LLAvatarName& avName, LLSD& sdNotification); // LLAvatarActions::teleportRequest()
    // The same, while the name is not known yet: true (NAME set to the generic hidden text) when it is hidden, so the prompt is not left to wait
    // for the name (the TeleportNamesScope that hides it would be gone by then)
    bool anonymizeUnknownTeleportRequestName(const LLUUID& idAgent, LLSD& sdNotification);

    // ---- W23, W24: "#RLV/~" folders given to the user
    bool isGiveToRLVOffer(const LLOfferInfo& offer);                                          // inventory_offer_handler(): never auto-accepted
    void onAgentFolderOffer(LLOfferInfo& offer);                                              // an agent's offer is shown/accepted
    void onTaskFolderOfferAccepted(LLOfferInfo& offer);                                       // an object's offer is accepted: the folder goes to #RLV
    void onTaskFolderOfferDeclined(const LLOfferInfo& offer);                                 // ... or declined

    // ================================================================ show (rlvhooks_show.cpp)
    // (rlv-show) The hooks are declared in rlvhooks_show_<group>.h, one file per group of official files.
    #include "rlvhooks_show_gate.h"
    #include "rlvhooks_show_list.h"
    #include "rlvhooks_show_floater.h"
    #include "rlvhooks_show_panel1.h"
    #include "rlvhooks_show_panel2.h"
    #include "rlvhooks_show_misc.h"
    #include "rlvhooks_show_plusfx.h"

    // ================================================================ wear (rlvhooks_wear.cpp)
    // Attachment / outfit / inventory locks and the wear menus (research/248 W5-W22, W25, W26, E49). With RLVa
    // off none of these is called (every caller checks enabled() first). The invguard questions (folder and item
    // locks) are in plus/invguard; the RLV guard that answers them is in rlvplus.cpp.

    // LLAttachmentsMgr::addAttachmentRequest() (W5): false when the @addattach / @detach locks do not allow the request
    // (nothing is requested). May set the attachment point (an item with no point gets the one its lock names) and
    // turns a replace into an add; tells the lock watchdog. The watchdog's own requests (isForcedAttachRequest()) and
    // anything before the initial attachments were requested pass.
    bool allowAttachmentRequest(const LLUUID& idItem, bool fAdd, unsigned char& nAttachPt, bool& fAddOut);
    // LLAgentWearables::userAttachMultipleAttachments() (W5): the initial attachments were requested; from now on the
    // locks apply to wear requests (FS: LLAgentWearables::areInitialAttachmentsRequested()).
    void onInitialAttachmentsRequested();
    // LLAgentWearables::userRemoveMultipleAttachments() (W7): drops the locked attachments from the list; one that is
    // no longer in the COF is linked again.
    void removeLockedAttachments(std::vector<LLViewerObject*>& objects);

    // LLSelectMgr::sendDetach() / sendDropAttachment() / enable_detach() / is_editable_selected() (W6, W12): the selection
    // holds a locked attachment (FS: RlvSelectHasLockedAttach on the root nodes, or on every node with fRootsOnly false).
    bool selectionHasLockedAttachment(bool fRootsOnly = true);
    // The object (a root, or what the caller found) is a locked attachment / is in one (its root is). Task inventory,
    // script editor, drops (W12, W19-W22).
    bool isLockedAttachment(const LLViewerObject* pObj);
    bool isInLockedAttachment(const LLViewerObject* pObj);
    // W19: a script / notecard of a locked attachment is not opened; tells the user (blocked_viewxxx). Type: LLAssetType::EType.
    bool refusesViewLockedAttachment(const LLViewerObject* pObj, int eAssetType);
    // W20: a script of a locked attachment is not started, stopped, reset or saved; tells the user (blocked_generic).
    bool refusesChangeLockedAttachment(const LLViewerObject* pObj);

    // LLAppearanceMgr (W8, W9, W13, W16, W21)
    // The item can be worn under the locks (FS: rlvPredCanWearItem with replace / add).
    bool canWearItem(const LLInventoryItem* pItem, bool fReplace);
    // The linked item may leave the avatar (FS: rlvPredCanRemoveItem).
    bool canRemoveItem(const LLUUID& idItem);
    // The folder has a remove (fRemove) or add lock (outfit "take off" / "add" / "replace").
    bool isFolderLocked(const LLUUID& idFolder, bool fRemove);
    // updateCOF(): drops the items from index nFirstNew on that cannot be worn (the new ones of the outfit). Type: body part
    // or clothing or object (LLAssetType::EType).
    void dropUnwearableItems(std::vector<LLPointer<LLViewerInventoryItem> >& items, size_t nFirstNew, int eAssetType);
    // updateCOF() when replacing: adds the locked clothing / attachments the COF holds to items.
    void keepLockedCofItems(std::vector<LLPointer<LLViewerInventoryItem> >& items, int eAssetType);
    // updateAgentWearables(): @notify of the wearables that go and come (removals first). items: what is worn after.
    void notifyWearableChanges(const std::vector<LLPointer<LLInventoryItem> >& items);

    // plus/outfitbackup: before the saved outfit is put back (the COF is replaced as it is, not through updateCOF()):
    // contents is the array of COF link entries (name, desc, linked_id, type). Drops the new items the locks do not let us
    // wear (nDropped) and keeps the locked clothing / attachments the copy lacks (nKept), as updateCOF() does for a replaced outfit.
    void filterOutfitRestore(LLSD& sdContents, int& nKept, int& nDropped);

    // "Delete" of worn items (tree: llinventoryfunctions.cpp, gallery: llinventorygallery.cpp): takes the items a lock keeps
    // out of the lists (worn, items to delete after the take-off, folders to delete after it; a folder that holds one stays).
    // Says so once.
    void dropLockedFromDeletion(std::vector<LLUUID>& worn, std::vector<LLUUID>& itemsToDelete, std::vector<LLUUID>& foldersToDelete);

    // ungroup_folder_items(): an RLV folder lock refuses moving some of the folder's content to the parent (tells once); then nothing moves.
    bool refusesUngroup(const LLUUID& idFolder, const LLUUID& idDest);

    // Menus and buttons (W10-W13, W18, W26)
    bool canViewWireframe();                   // false (and says so) with a locked HUD
    bool hasLockedHUD();
    float hudZoomMin();                        // 0.1, or 0.85 with a locked HUD
    bool hasRemoveLockedAttachmentPoint();     // some attachment point cannot be removed from
    bool hasWearLocks();                       // a locked attachment point or layer exists
    bool canDetachFromPoint(const LLViewerJointAttachment* pAttachPt);
    bool canAddToAttachmentPoint(const LLViewerJointAttachment* pAttachPt);   // false for an "add locked" point
    bool isAttachmentPointAddLocked(int idxAttachPt);
    bool canReplaceAttachmentOn(const LLViewerJointAttachment* pAttachPt);
    // The selected object can be worn on the point (nIndex 0: the default point; nIndex != 0: it is "add attached").
    bool canAttachSelectionTo(int nIndex, const LLViewerJointAttachment* pAttachPt);
    // rez_attachment() (W13): a missing point is taken from the item name; false if the item cannot be worn.
    bool allowRezAttachment(const LLViewerInventoryItem* pItem, LLViewerJointAttachment*& pAttachPt, bool fReplace);
    // Item menus: what the locks leave of "Wear" (replace) and "Add" for the item.
    void wearMenuState(const LLInventoryItem* pItem, bool& fCanReplace, bool& fCanAdd);
    // The or-ing of wearMenuState and the removal lock over a selection (FS LLWearableItemsList::ContextMenu).
    void accumulateItemMenuState(const LLInventoryItem* pItem, bool fIsWorn, bool& fCanReplace, bool& fCanAdd, bool& fCanRemove);
    bool canRemoveWearable(const LLInventoryItem* pItem);
    bool canRemoveWearableType(int eWearableType);
    // LLEditTakeOff: the topmost wearable of the type that is not locked (nIndex in and out); false if there is none.
    bool topUnlockedWearable(int eWearableType, unsigned int& nIndex);
    // LLFolderBridge::isItemMovable(): the #RLV folder is not moved (FS: by its name).
    bool isSharedRootFolder(const std::string& strName);
    // LLInventoryModel::processBulkUpdateInventory(): a #RLV/~ folder keeps the name the viewer has.
    void keepGivenFolderName(LLViewerInventoryCategory* pNewFolder, const LLViewerInventoryCategory* pOldFolder);

    // plus/rlv internal (rlvplus.cpp): the RLV folder-lock guard of plus/invguard is added when RLVa is switched on
    // (onStartupState) and removed in cleanup().
    void enableInvGuard();
    void disableInvGuard();

    // llviewermenu.cpp (E49): @shownames hides the avatar's name / @showloc hides the location / no "Teleport Home"
    bool canShowMenuName(const LLUUID& idAgent);
    bool canShowMenuLocation();
    bool canTeleportHomeMenu();

    // ================================================================ sit (rlvhooks_sit.cpp)
    // (rlv-sit)
}

#endif // PLUS_RLV_RLVHOOKS_H
