# plus/rlv の出どころ

RLVa（RestrainedLove API）対応。設計は research/248-rlva-design.md。

- 移植元: Firestorm Viewer の RLVa 2.4.2（`indra/newview/rlv*.cpp/h`、FS の木 443cc2b4eb 2026-05-25）。
- 著作: Copyright (c) 2009-2016 Kitty Barnett。GNU LGPL 2.1 only。各ファイルの先頭の著作表示と、`// Checked:` などの上流のコメントは残してある。
- ファイル名・クラス名は RLVa のまま（上流の更新を `rlv*.cpp` の差分として取り込むため）。削った物・書き直した物は下の表。
- Charis で足したファイル: `rlvhooks.h`・`rlvhooks_core.cpp`（公式ファイルから呼ばれる口）、`rlvnames.cpp`、`rlvexperience.h/.cpp`、`rlvforcewear.h`（`RlvForceWear` の宣言だけ。実装は rlv-locks）、`rlvwearquery.cpp`、`rlvattachpt.h/.cpp`（`RlvAttachPtLookup`。FS では rlvlocks にあるが鍵に依らないので core へ）。
- 目印: 後続の枝が戻す箇所に `// RLV-LOCKS:`（rlv-locks）、`// RLV-SIT:`（rlv-sit）、`// RLV-COMM:`（rlv-comm）を置いてある（`grep -rn "RLV-" plus/rlv`）。段 3（カメラ・環境・デバッグ・描画効果・@fly ほか）は、設計 3.2 のとおり関数ごと削除してあり、目印は無い。
- 段 3 の列挙（`RLV_BHVR_*`）は上流との差分を小さくするため rlvdefines.h に残してある。辞書には登録しない。

## 削った物・書き直した物（担当ごとの表）

### rlvhelper / rlvdefines / rlvmodifiers

| ファイル | 関数 | 扱い | 理由 |
|---|---|---|---|
| rlvhelper.cpp | `RlvBehaviourDictionary()` の addEntry/addModifier | 書き直し | 段 1 全部＋段 2 の問い合わせ（@getattach 系・@getoutfit 系・@getinv/@getinvworn・@findfolder(s)・@getpath(new)）＋@getcommand・@tpto=force だけを登録。段 3（カメラ・@setenv・@setdebug・@setoverlay/@setsphere・@fly @jump @alwaysrun @temprun @allowidle @sendgesture・@touch* @fartouch @interact @edit* @rez・@showinv @view* @showhovertext* @shownearby @showself*・@share @pay @buy @setgroup @getgroup @getsitid・@adjustheight・@getcam_*）は削除。鍵・force は `// RLV-LOCKS:`、座る系（@sit @sittp @standtp @unsit @sitground と SITTPDIST）は `// RLV-SIT:` の目印 |
| rlvhelper.cpp | `RlvBehaviourDictionary()` の末尾（`RlvSettings::getNoSetEnv()` で "setenv" を BHVR_BLOCKED） | 書き直し | `refreshBlockedCommands()` に置き換え（RLVaBlockedCommands。@setenv は段 3 で登録しない） |
| rlvhelper.h/.cpp | `RlvBehaviourDictionary::isBlocked(ERlvBehaviour)`・`refreshBlockedCommands()`・`m_BlockedBhvrs`・`m_nBlockedCommandsVersion` | 追加 | RLVaBlockedCommands（設計 6.1）。名前の解析は RlvSettings（担当 C）の `isBlockedCommand()`・`getBlockedCommandsVersion()` を使い、版が変わった時だけ全項目の BHVR_BLOCKED を付け直す。RlvCommand の構築ごとに版を比べるだけ |
| rlvhelper.cpp | `RlvBehaviourDictionary::addEntry()` | 書き直し | 試験的コマンドで弾いた項目を delete（FS は漏れていた） |
| rlvhelper.cpp | `RlvBehaviourDictionary::addModifier()`（2 つ） | 書き直し | 範囲外の修飾を delete、上書き時に古い物を delete、項目が弾かれた時に解放後の参照をしない |
| rlvhelper.h/.cpp | `RlvBehaviourModifier::convertOptionValue()` | 書き直し | 設計 6.2・12 #8: float は nan/inf を拒否し [nMinFloat, nMaxFloat]（既定 0〜RLV_OPTION_DISTANCE_MAX=4096）に clamp。ベクトルの各成分も isfinite。UUID は `LLUUID::set(…, false)` |
| rlvhelper.h | `RlvCommand::parseCommand()` | 書き直し（protected → public） | selftest rlv-parse が抜き出して試すため。中身は FS のまま |
| rlvhelper.cpp | `RlvCommandOptionHelper::parseOption<float>` | 書き直し | `rlvParseFloat()` を通す（nan/inf を拒否） |
| rlvhelper.cpp | `parseOption<LLVector2/LLVector3/LLVector3d/LLColor3>` | 書き直し | sscanf("%f") が通す nan/inf を `rlvIsFinite()` で拒否 |
| rlvhelper.cpp | `parseOption<LLWearableType::EType>` | 書き直し | `rlvTypeNameToType()`（F4） |
| rlvhelper.cpp | `parseOption<LLViewerJointAttachment*>` | 書き直し（include のみ） | `RlvAttachPtLookup` は core の rlvattachpt.h（担当 I が rlvlocks から移した）を include |
| rlvhelper.h/.cpp | `rlvTypeNameToType()` | 追加 | F4: 公式の typeNameToType() は知らない名前で WT_NONE(-1) を返す。0 <= type < WT_COUNT 以外は WT_INVALID |
| rlvhelper.h/.cpp | `rlvParseFloat()`・`rlvIsFinite(float*/double*)` | 追加 | 設計 6.2 の数値の検証。std だけに依るので selftest で抜き出せる |
| rlvhelper.h | `operator<(LLVector3d, LLVector3d)`・`operator<(LLVector4, LLVector4)` | 追加 | F3: FS は llmath に足している。RlvBehaviourModifierCompMin/Max が RlvBehaviourModifierValue（boost::variant）を比べるのに要る。公式に入ったら消す |
| rlvhelper.cpp | `RlvCommandOptionGetPathCallback` | 書き直し | FS の `LLVOAvatarSelf::setAttachmentCallback()`（FS だけの信号）の代わりに、待ちがある間だけ idle で `gObjectList.findObject()` を引く（最長 30 秒）。自分の装着物だけ。コールバックの前に自分を外すので二重に呼ばない |
| rlvhelper.h/.cpp | `RlvCommandOptionAdjustHeight` | 削除 | @adjustheight（段 3） |
| rlvhelper.h | 共有ハンドラの typedef `RlvBehaviourCamEyeFocusOffsetHandler`・`RlvBehaviourSetCamFovHandler`・`RlvBehaviourShowSelfToggleHandler`・`RlvBehaviourCamZoomMinMaxHandler`・`RlvReplyCamMinMaxModifierHandler`・`RlvForceCamEyeFocusOffsetHandler` | 削除 | 段 3（カメラ・@showself）。`RlvBehaviourAddRemAttachHandler`・`RlvForceRemAttachHandler` は rlv-locks のために残す |
| rlvhelper.h/.cpp | `RlvBehaviourNotifyHandler::addNotify()`・`removeNotify()`・構築子 | 書き直し | FS は最後の @notify が消えると `delete this`（LLSingleton は次の取得で警告を出して作り直す）。インスタンスは残し、@notify がある間だけ命令の信号につなぐ |
| rlvhelper.h/.cpp | `Rlv::forceAtmosphericShadersIfAvailable()`・`Rlv::getObjectLinkNumber()`・`Rlv::getObjectRootId()` | 削除 | @setenv・@setsphere（段 3）だけが使っていた |
| rlvhelper.h/.cpp | `RlvForceWear`（宣言・実装） | 削除（メインが済） | rlvforcewear.h（宣言）と rlv-locks の rlvforcewear.cpp へ。core は `isWearingItem()` だけ（rlvwearquery.cpp） |
| rlvhelper.cpp | include の `fslslbridge.h`・`rlveffects.h`・`llattachmentsmgr.h`・`llfeaturemanager.h`・`llgesturemgr.h`・`llnotificationsutil.h`・`pipeline.h` | 削除 | FS だけの物と段 3・force の物。`llcallbacklist.h`・`llinventoryfunctions.h`・`llviewerjointattachment.h`・`rlvattachpt.h` を足した |
| rlvdefines.h | `RLV_OPTION_DISTANCE_MAX`（4096.f） | 追加 | 設計 6.2 の距離の clamp |
| rlvdefines.h | `RlvSettingNames::ExperimentalCommands`・`BlockedCommands` | 追加 | 設定名（RLVaExperimentalCommands・RLVaBlockedCommands） |
| rlvmodifiers.h | （コメント 1 行） | 追加 | operator< の置き場所（rlvhelper.h）の案内。コードは FS のまま |

### rlvhandler

| ファイル | 関数 | 扱い | 理由 |
|---|---|---|---|

|---|---|---|---|
| rlvhandler.h | `RlvHandler` の基底 `LLOldEvents::LLSimpleListener`・`LLParticularGroupObserver`、`changed()`・`handleEvent()`・`onActiveGroupChanged()`・`getAgentGroup()`、`m_idAgentGroup`・`m_PendingGroupChange`・`m_GroupChangeExpiration` | 削除 | @setgroup（段 3） |
| rlvhandler.cpp | `RlvHandler()`（`gAgent.addListener("new group")`）・`cleanup()`（グループの観察の解除） | 書き直し | 同上。cleanup() は信号の切断・上限の表の消去を足した（149 (E)） |
| rlvhandler.cpp | `checkActiveGroupThrottle()`・`setActiveGroup()`・`setActiveGroupRole()`・`RlvForceHandler<RLV_BHVR_SETGROUP>`・`@getgroup` の返事 | 削除 | 段 3 |
| rlvhandler.h/.cpp | `setCameraOverride()`・`m_strCameraPresetRestore`、@setcam* の切替・修飾・force・`@getcam_*`・`@camzoommin/max` | 削除 | 段 3（カメラ） |
| rlvhandler.cpp | @setenv・@setdebug の切替、`setEnabled()` の `RlvEnvironment`・`RlvExtGetSet` の登録、@setcam/@setdebug/@setenv の単独保持の確認 | 削除 | 段 3 |
| rlvhandler.cpp | @setoverlay・@setoverlay_touch・@setoverlay_tween=force・@setsphere | 削除 | 段 3（描画効果） |
| rlvhandler.cpp | @edit・@pay・@buy の切替、@fly=force、@adjustheight=force、`@getheightoffset`・`@getsitid` の返事 | 削除 | 段 3 |
| rlvhandler.cpp | @showhovertext・@showinv・@shownearby・@showself(head)・@viewtransparent・@viewwireframe | 削除 | 段 3 |
| rlvhandler.h/.cpp | `RLV_EXPERIMENTAL_COMPOSITEFOLDERS` の関数（`getCompositeInfo()` ほか） | 削除 | 定義が無く使われない |
| rlvhandler.cpp | `getAttachmentResourcesCoro()`・`onExperienceAttach()`・`onExperienceEvent()` | 削除（rlvexperience.cpp へ移動、担当 I。宣言は rlvhandler.h に残す） | 設計 3.1 |
| rlvhandler.h/.cpp | `onAddRemFolderLock()`・`onAddRemFolderLockException()`、`processAddRemCommand()` の @attachthis/@detachthis[_except]・@addoutfit/@remoutfit・@(un)sharedwear/unwear の case | 削除（目印 RLV-LOCKS） | 鍵（rlv-locks が戻す） |
| rlvhandler.cpp | `RlvBehaviourAddRemAttachHandler`（@addattach/@remattach）・`RlvBehaviourHandler<RLV_BHVR_DETACH>` | 削除（目印 RLV-LOCKS） | 鍵 |
| rlvhandler.h/.cpp | `RlvForceHandler<RLV_BHVR_DETACHME>`・`RlvForceRemAttachHandler`・`RlvForceHandler<RLV_BHVR_REMOUTFIT>`・`onForceWear()`・`onForceWearCallback()`、`processForceCommand()` の `RLV_CMD_FORCEWEAR` | 削除（目印 RLV-LOCKS） | force（rlv-locks） |
| rlvhandler.cpp | `onAttach()`・`onDetach()`・`onGC()` の `gRlvAttachmentLocks` の呼び出し（`verifyAttachmentLocks()` を含む） | 削除（目印 RLV-LOCKS） | 鍵 |
| rlvhandler.cpp | `RlvForceHandler<RLV_BHVR_SITGROUND>`・`RlvForceHandler<RLV_BHVR_SIT>`、`processForceCommand()` の @unsit=force、`onSitOrStand()` の @standtp・@sitground の続き | 削除（目印 RLV-SIT） | 座る系（rlv-sit） |
| rlvhandler.cpp | `onGetAttach()`・`onGetAttachNames()`・`onGetOutfit()`・`onGetOutfitNames()` | 書き直し（鍵の項を RLV-LOCKS で外し、鍵が無い時と同じ答え。FS の LSL ブリッジの差し引きを削除） | 鍵の型を使わない。カリスに LSL ブリッジは無い |
| rlvhandler.cpp | `onGetOutfit()` の層の名前 | 書き直し（`rlvTypeNameToType()`） | F4（知らない名前で -1 が返る） |
| rlvhandler.cpp | `onGetInvWorn()` | 書き直し（`RlvInventory::collectDescendentsIf()`、親をたどる所で null を見る） | 公式に follow_folder_links が無い／壊れた木で落ちない |
| rlvhandler.cpp | `RlvBehaviourToggleHandler<RLV_BHVR_SHOWNAMES>`・`RlvBehaviourHandler<RLV_BHVR_SHOWNAMES>`・`RlvBehaviourModifierHandler<RLV_MODIFIER_SHOWNAMETAGSDIST>::onValueChange()`・`RlvBehaviourHandler<RLV_BHVR_SHOWNAMETAGS>` | 書き直し（`setShowName()` と例外の表だけ残し、UI の更新は rlvui_show.cpp へ） | 設計 3.1（FSRadar・fs_voice_controls・setForceDisplayNames・refreshAllObjectText は FS だけ／UI） |
| rlvhandler.h/.cpp | `setExceptionCallback()`・`m_OnException`（`RlvBehaviourGenericHandler<RLV_OPTION_EXCEPTION>` が鳴らす） | 追加 | 例外の追加・削除で UI を更新する口（FS は各ハンドラーの中で直接更新していた） |
| rlvhandler.cpp | `RlvBehaviourToggleHandler<RLV_BHVR_SENDIM>` | 書き直し（`getControl()` の null を見る） | 安全 |
| rlvhandler.cpp | `RlvBehaviourRecvSendStartIMHandler` | 書き直し（`std::isfinite`、距離を 4096 m で clamp、修飾の null を見る） | 設計 6.2・12 #8 |
| rlvhandler.cpp | `RlvForceHandler<RLV_BHVR_TPTO>`・`onTeleportCallback()` | 書き直し（角度・座標の有限の確認、選択肢は 2 つまで、地域名 64 字まで。look-at 付きの TP は目印 RLV-COMM で `teleportViaLocation()` に） | 設計 6.2。`teleportViaLocationLookAt(pos, look_at)` は rlv-comm が足す（設計 1.4） |
| rlvhandler.cpp | `RlvCommandHandlerBaseImpl<RLV_TYPE_REPLY>::processCommand()`・`processReplyCommand()` | 書き直し（返信は `isValidReplyChannel(ch)` だけ。ch 0 と RLV コンソールへの返事を削除） | 設計 6.2（ch 0 には返さない）。コンソールは段 3 |
| rlvhandler.cpp | `processCommand(reference_wrapper)` | 書き直し（`RLVaBlockedCommands` で止めた行動は =y/=rem/@clear 以外 `RLV_RET_FAILED_BLOCKED`、上限の確認を追加） | 設計 6.1・12 |
| rlvhandler.cpp | `processCommand(idObj, str)`・`processRetainedCommands()` | 書き直し（オフなら `RLV_RET_FAILED_DISABLED`、ログイン中の保留 4000／うち追加・force・返信 2000） | 設計 12 #5 |
| rlvhandler.h/.cpp | `checkCommandLimits()`・`logLimit()`・`pruneThrottles()`・`m_CommandThrottle`・`m_RetainedLimited`、先頭の上限の定数 | 追加 | 設計 12 #2〜#5（1 物体 1000、物体 256、1 物体 200 命令／秒、ログは物体ごとに 1 分 1 回） |
| rlvhandler.h/.cpp | `checkAutoResponseThrottle()`・`m_AutoResponseTimes` | 追加 | 設計 12 #6（自動応答は相手ごとに 60 秒に 1 回。rlv-comm も使う） |
| rlvhandler.cpp | `processIMQuery()` | 書き直し（オフなら false、自動応答を上の間引きに通す、@list/@except の確認は相手ごとに 60 秒に 1 回） | 設計 12 #6 |
| rlvhandler.cpp | `onIMQueryListResponse()` | 書き直し（オフ・送り主が空なら何もしない、`sendIMQueryList()` で送る） | ログアウト後の応答 |
| rlvhandler.h/.cpp | `sendIMQueryList()` | 追加 | 設計 12 #7（IM 8 通で切り `(truncated)`） |
| rlvhandler.h/.cpp | `getFormattedBehaviourString()`・`ERlvBehaviourFilter`、cpp の `rlvGetItemNameFromObjID()`・`rlvGetShowException()` | 追加（FS rlvfloaters.cpp/.h から移動） | RLV の窓は段 3。@list/@except の整形だけ使う（設計 3.2） |
| rlvhandler.cpp | `filterChat()` | 書き直し（`RlvSettings::getShowEllipsis()`） | 毎行の設定の名前引きをやめる（決まり 10） |
| rlvhandler.cpp | `utf8str_strlen()`・`utf8str_chtruncate()` | 書き直し（static） | 名前の衝突を避ける |
| rlvhandler.cpp | `onAttach()`・`onDetach()`・`onSitOrStand()`・`onLoginComplete()`・`onIdleStartup()` | 書き直し（オフなら何もしない） | 決まり 10 (c) |
| rlvhandler.cpp | `setEnabled()` | 書き直し（`ShowAssertionFail` の名前を std::string に） | カリスの `LLControlGroup::set()` は string_view を取らない |

### rlvcommon / rlvnames / rlvactions / rlvui

| ファイル | 関数 | 扱い | 理由 |
|---|---|---|---|

|---|---|---|---|
| rlvcommon.h/.cpp | `utf8str_split()` | 追加（FS の llcommon/llstring.cpp から。`utf8str_substr()` は static の `rlv_utf8str_substr()` に） | F2: 公式の llcommon に無い。途中が壊れた UTF-8 で長さ 0 の切り出しになり無限ループになる所を、必ず進むように直した |
| rlvcommon.h/.cpp | `RlvSettings`（全 getter、`initClass()`） | 書き直し（設定を static に持ち、変更の信号で更新。`bindSetting()`） | 設計 5.2: 毎行・毎フレームの設定の名前引きをしない。CanOOC・NoSetEnv・Experience の 2 つは FS どおり起動時に 1 回だけ読む |
| rlvcommon.h/.cpp | `RlvSettings::getShowEllipsis()`・`getShowRedirectChatTyping()`・`getShowAssertionFailures()`・`getExperimentalCommands()`・`getTopLevelMenu()`・`getWearReplaceUnlocked()`・`getMaturityThreshold()` | 追加 | FS では各所で `gSavedSettings` を直接引いていた物を static に |
| rlvcommon.h/.cpp | `RlvSettings::getBlockedCommands()`・`isBlockedCommand()`・`getBlockedCommandsVersion()`・`onChangedBlockedCommands()` | 追加 | 設計 6.1: カリスの新設定 `RLVaBlockedCommands`（カンマ区切り、`@` と大文字小文字を無視） |
| rlvcommon.h/.cpp | `RlvSettings::cleanupClass()`・`initMainListener()` | 追加 | 設計 6.5: 終了時に信号を外す。`RestrainedLove` の変更通知は FS では llviewercontrol.cpp（公式）で繋いでいた |
| rlvcommon.cpp | `RlvSettings::initClass()` の `RLVaExperienceMaturityThreshold` | 書き直し（0〜3 に clamp） | 設計 7.1 |
| rlvcommon.cpp | `RlvSettings::initCompatibilityMode()`・`isCompatibilityModeObject()` | 書き直し（`name:` の正規表現を読む時に 1 回だけ組み立て、壊れた式は捨てる） | FS は @version のたびに組み立て、壊れた式で boost が例外を投げる |
| rlvcommon.cpp | `RlvSettings::updateLoginLastLocation()` | 書き直し（`PerAccountSettingsFile` が空なら保存しない） | ログイン前に空のパスへ書かない |
| rlvcommon.cpp | `RlvStrings::getString()` | 書き直し（値の一覧が空の時は「Missing」） | 空の list の `back()` を避ける |
| rlvcommon.h | `RlvSettings::getWearAddPrefix()`・`getWearReplacePrefix()` | 書き直し（`const std::string&` を返す） | 毎回の複写をやめた |
| rlvcommon.h/.cpp | `RlvUtil::filterNames()`・`filterLocation()`・`isNearbyAgent()`・`isNearbyRegion()` | 書き直し（rlvnames.cpp へ移した） | 設計 3.1・5.1 |
| rlvnames.cpp | `RlvNearbyNames`（`invalidate()`・`cleanupClass()`）と名前表・地名表 | 追加 | 近くの名前の表を 1 秒ごとに作り直して使い回し、正規表現を毎回組み立てない。結果は「表」と「隠す相手の組」が同じ間だけ使い回す。置換は `format_literal`（名前を書式として読まない）。空の地名・区画名で全部の語の境目に「(Hidden ...)」が入る FS の不具合を直した。FS の `fFilterLegacy &=` がループをまたいで残る不具合も直した |
| rlvcommon.cpp | `RlvUtil::filterMentions()` | 書き直し（`/mention` が無ければすぐ戻る。UUID は `LLUUID::set(…, false)`） | 速さと設計 6.2 |
| rlvcommon.cpp | `RlvUtil::filterScriptQuestions()` の装着の権限 | 削除（`// RLV-LOCKS:`） | `gRlvAttachmentLocks`。rlv-locks が戻す |
| rlvcommon.h | `RLV_SCRIPT_PERMISSION_TAKE_CONTROLS`・`_ATTACH`・`_TELEPORT` | 追加 | FS が llscriptruntimeperms.h に足した定数。公式に無い |
| rlvcommon.cpp | `RlvUtil::notifyBlocked()` | 書き直し（引数は自分で作った agent/group の SLURL 以外を `plus::safeText()` に通す） | 設計 6.2: 他人の名前などに入ったリンクを実行させない |
| rlvcommon.cpp | `RlvUtil::notifyFailedAssertion()` | 書き直し（`RlvSettings::getShowAssertionFailures()`） | static 化 |
| rlvcommon.h/.cpp | `RlvUtil::sendBusyMessage()` | 書き直し（相手ごとに 60 秒に 1 回、bool を返す、本文は 1023 バイトで切る） | 設計 12 節 6 |
| rlvcommon.h/.cpp | `RlvUtil::sendIMMessage()` | 書き直し（第 4 引数 `nMaxMessages` 既定 8、超えたら最後に `(truncated)`） | 設計 12 節 7 |
| rlvcommon.cpp | `escape_for_regex()` | 書き直し（rlvnames.cpp の無名名前空間へ。正規表現は 1 回だけ組み立て） | 全体の名前空間に出さない |
| rlvcommon.cpp | `rlvMenuToggleVisible()` | 書き直し（`gMenuBarView`・「Advanced」が無い時に落ちない。設定は static） | FS は null を確かめない。メニュー名は rlv-ui が合わせる |
| rlvcommon.h/.cpp | `rlvCanDeleteOrReturn()`（2 つ）・`RlvSelectHasLockedAttach` | 削除（`// RLV-LOCKS:`） | 鍵。@edit・@rez の部分は段 3 |
| rlvcommon.h/.cpp | `RlvSelectIsEditable` | 削除 | 段 3（@edit） |
| rlvcommon.cpp | `RlvSelectIsSittingOn::apply()` | 書き直し（`isChild()` へ `const_cast`） | 公式の `isChild()` は const でないポインタを取る（FS が変えた） |
| rlvcommon.h/.cpp | `rlvPredCanWearItem()`・`rlvPredCanNotWearItem()`・`rlvPredCanRemoveItem()`（2 つ）・`rlvPredCanNotRemoveItem()`（2 つ）・`RlvPredCanWearItem`・`RlvPredCanNotWearItem`・`RlvPredCanRemoveItem`・`RlvPredCanNotRemoveItem` | 削除（`// RLV-LOCKS:`） | 鍵 |
| rlvcommon.cpp | `RlvNotifications`（コメントアウト済み） | 削除 | FS でも使っていない |
| rlvcommon.h | `rlvGetAnonym()` | 追加（宣言のみ。定義は FS どおり） | 宣言が無かった |
| rlvactions.h/.cpp | カメラの全部（`canChangeCameraFOV`・`canChangeCameraPreset`・`canChangeToMouselook`・`isCamera*`・`getCamera*Limits`） | 削除 | 段 3 |
| rlvactions.h/.cpp | `canChangeActiveGroup`・`canGiveInventory`（2 つ）・`canPlayGestures`・`canShowNearbyAgents`・`canPreviewTextures`・`canFly`（2 つ）・`canJump`・`canChangeEnvironment`・`hasPostProcess`・`canBuild`・`canBuyObject`・`canEdit`（2 つ）・`canInteract`・`canPayAvatar`・`canPayObject`・`canRez`・`canShowHoverText`・`canTouch`・`canHighlightTransparent`・`canViewWireframe`、`enum class ERlvCheckType` | 削除 | 段 3（@setgroup・@share・@sendgesture・@shownearby・@viewtexture・@fly・@jump・@setenv・@setsphere・@edit・@rez・@buy・@pay・@showhovertext・@touch・@interact・@viewtransparent・@viewwireframe） |
| rlvactions.h/.cpp | `canPasteInventory()`（2 つ） | 削除（`// RLV-LOCKS:`） | フォルダの鍵 |
| rlvactions.cpp | `canStand()`（2 つ）・`canGroundSit()`（2 つ） | 書き直し（`true` を返す仮の実装、`// RLV-SIT:`） | 座る系は rlv-sit |
| rlvactions.cpp | `canSit()` | 書き直し（制限の無い時の FS の結果＝体積のある物体なら真、`// RLV-SIT:`） | 同上。@fartouch の部分は段 3 |
| rlvactions.cpp | `canTeleportToLocal()` の @sittp | 削除（`// RLV-SIT:`） | 同上 |
| rlvactions.cpp | `canSendTypingStart()` | 書き直し（`RlvSettings::getShowRedirectChatTyping()`） | 設計 5.2 |
| rlvactions.cpp | `canShowName()`・`canShowNameTag()`・`rlvCheckAvatarIMDistance()` | 書き直し（文脈の範囲・null・修飾が無い時を確かめる） | 範囲外の添字と null の参照を避ける |
| rlvactions.cpp | `rlvGetAvatarPosition()` | 追加 | FS の `LLWorld::getAvatar()` は公式に無い。見えるアバターは物体から、見えなければ `LLWorld::getAvatars()` の位置から |
| rlvui.h/.cpp | `RlvUIEnabler` の構築子 | 書き直し（`m_Handlers.insert` を全部やめ、登録の口だけ。核が持つのは `RlvNearbyNames::invalidate()` と `RLVaLoginLastLocation` の更新だけ） | 設計 3.1: 中身は rlvui_show.cpp・rlvui_sit.cpp |
| rlvui.h/.cpp | `registerToggle()`・`registerExceptionChange()`・`registerModifierChange()`・`onExceptionChange()`・`onModifierChange()`・デストラクタ | 追加 | 同上。例外の信号と修飾の信号には骨組みが自分で繋ぐ |
| rlvui.h/.cpp | `addFloaterFilter()`・`removeFloaterFilter()`・`addPanelFilter()`・`removePanelFilter()`・`canShowFloater()`・`canShowPanel()`・`setFloaterGateHook()`・`setPanelGateHook()`・`updateFloaterGate()`・`updatePanelGate()` | 追加 | 設計 2.2 E1・E2: FS の `LLFloaterReg::setValidateCallback`（boost の信号）の代わり。門は要る間だけ差す |
| rlvui.cpp | `addGenericFloaterFilter()`・`removeGenericFloaterFilter()` | 書き直し（信号でなく門の付け外し） | 同上 |
| rlvui.cpp | `filterFloaterGeneric()` | 書き直し（`canShowFloater()` の中へ） | 同上 |
| rlvui.h/.cpp | `onRefreshHoverText()`・`onToggleShowLoc()`・`onToggleShowNames()`・`onToggleShowMinimap()`・`onToggleShowWorldMap()`・`onToggleTp()`・`filterFloaterShowLoc()`・`filterPanelShowLoc()` | 削除 | rlv-show が rlvui_show.cpp に書く |
| rlvui.h/.cpp | `onToggleUnsit()` | 削除 | rlv-sit が書く |
| rlvui.h/.cpp | `onToggleMovement()`・`onToggleViewXXX()`・`filterFloaterViewXXX()` | 削除 | 段 3 |
| rlvui.cpp | `hasOpenIM()` | 書き直し（`fs_impanel` → 公式の `impanel`） | FS だけの窓 |
| rlvui.cpp | `canViewParcelProperties()` | 書き直し（選択が null の時） | null の参照を避ける |

### rlvinventory / rlvexperience / rlvattachpt / rlvwearquery

| ファイル | 関数 | 扱い | 理由 |
|---|---|---|---|

|---|---|---|---|
| rlvinventory.h | `#include "rlvlocks.h"` | 書き直し（`rlvattachpt.h`・`rlvforcewear.h` に） | core は鍵の型を持たない |
| rlvinventory.h | `RlvInventory::isFoldedFolder()` | 書き直し（空の名前で `at(0)` が投げるのを直した） | FS の不具合 |
| rlvinventory.h | `RlvInventory::getFoldedParent()` | 書き直し（上へたどる回数を `cMaxFolderDepth` で打ち切り） | 壊れた木で止まらないのを防ぐ（設計 6.2） |
| rlvinventory.h/.cpp | `RlvInventory::collectDescendentsIf()`・`collectDescendentsIfDepth()`・`cMaxFolderDepth`(64) | 追加 | FS は公式の `LLInventoryModel::collectDescendentsIf()` に follow_folder_links を足している。同じ順で辿る版を plus に置き、深さで打ち切る（設計 6.2「検索は木の深さで打ち切る」） |
| rlvinventory.cpp | `RlvInventory::fetchSharedLinks()`・`findSharedFolders()` | 書き直し（上の `collectDescendentsIf()` を使う） | 公式に 6 番目の引数が無い／深さの打ち切り |
| rlvinventory.cpp | `RlvInventory::getSharedFolder(path)`・`getSharedPath()` | 書き直し（段数を `cMaxFolderDepth` で打ち切り） | 設計 6.2 |
| rlvinventory.cpp | `RlvRenameOnWearObserver::doneIdle()` | 書き直し（`RlvHandler::isEnabled()` を確認。FS の `LLVOAvatarSelf::getWornAttachmentPoint()` を `getWornAttachment()`＋`RlvAttachPtLookup` に） | 公式に `getWornAttachmentPoint()` が無い |
| rlvinventory.h/.cpp | `RlvGiveToRLVOffer::isValidDestPath()` | 追加 | `#RLV/~` の受け取りフォルダ名を検証（各段が空でない・63 バイト以下、`correctInventoryName` で補正）。設計 6.2 |
| rlvinventory.cpp | `RlvGiveToRLVOffer::createDestinationFolder()`・`onCategoryCreateCallback()` | 書き直し（`isValidDestPath()` を通す。補正は先に済ませる） | 同上 |
| rlvinventory.cpp | `rlv_rename_category()`（static） | 追加 | 公式の `rename_category()` に完了の callback が無い（FS が足した）。同じ判定で `update_inventory_category()` を呼ぶ |
| rlvinventory.h/.cpp | `RlvGiveToRLVOffer::moveAndRename()` | 書き直し（引数 `fRenameTried`。名前の付け直しは 1 回だけ。付け直せない時は null で終える） | FS は名前が一致しないと付け直しを繰り返し、付け直せないと完了しない |
| rlvinventory.h/.cpp | `RlvGiveToRLVTaskOffer` の構築子・解体子・`changed()` | 書き直し（取引 ID ではなく「新しく足された `#RLV/~` で始まるフォルダ」で照合、1 フォルダは 1 つの申し出だけが取る、120 秒で見張りをやめる） | 公式の `LLInventoryModel` に `getTransactionId()`（FS の追加）が無い |
| rlvinventory.cpp | `RlvGiveToRLVTaskOffer::doneIdle()`・`RlvGiveToRLVAgentOffer::doneIdle()` | 書き直し（`RlvHandler::isEnabled()` を確認） | オフなら何もしない |
| rlvinventory.cpp | `RlvWearableItemCollector::onCollectItem()` | 書き直し（`case` の落ち込みに `[[fallthrough]]`） | 中身は FS と同じ |
| rlvinventory.cpp | `RlvFindAttachmentsOnPoint::operator()` | そのまま | — |
| rlvexperience.h | `rlvValidateAttachmentResources()`・`RlvAttachResObject`・上限の定数 | 追加 | cap `AttachmentResources` の応答の検証（attachments 64・objects 256・UUID の形・name 255 バイト・型違いは飛ばす）。LLSD/LLUUID だけに依る（selftest 用） |
| rlvexperience.cpp | `RlvHandler::getAttachmentResourcesCoro()` | 移動＋書き直し（FS rlvhandler.cpp から。検証を通す、戻った時に有効・ログアウト中でないかを確認、既に止めた物体は先に除く、ログは DEBUGS） | 設計 3.2・6.2・6.5 |
| rlvexperience.cpp | `RlvHandler::onExperienceAttach()` | 移動＋書き直し（有効・ログアウトの確認、成熟度を 0〜255 に clamp してから U8 へ） | 非同期の戻り／値の折り返し |
| rlvexperience.cpp | `RlvHandler::onExperienceEvent()` | 移動（オフなら早期 return を足した） | 設計 3.2 |
| rlvattachpt.h / .cpp（新） | `RlvAttachPtLookup`（宣言・inline・`initLookupTable()`・`getAttachPointIndex()` 各版・`getAttachPointIndexLegacy()`・`m_AttachPtLookupMap`） | 移動（FS rlvlocks.h/.cpp から。中身は同じ） | 鍵の状態を持たないので core に置く。rlvlocks.h はこれを include する |
| rlvattachpt.h | `RlvAttachPtLookup::getAttachPointIndex(std::string)` | 書き直し（表が空なら `initLookupTable()` を呼ぶ） | FS は `LLVOAvatarSelf::initInstance()` から作る。公式の印を増やさない |
| rlvattachpt.h | `RlvAttachPtLookup::getAttachPointIndex(const LLViewerObject*)` | 書き直し（`const_cast` で `getAttachmentState()`） | 公式の `getAttachmentState()` は const でない（FS が const にした） |
| rlvwearquery.cpp | `RlvForceWear::isStrippable(const LLInventoryItem*)` | 移動（FS rlvhelper.cpp から。折り畳みの親をたどる回数を `cMaxFolderDepth` で打ち切り） | 鍵に依らない。@getinvworn と収集器が使う |
| rlvwearquery.cpp | include | 追加（`rlvcommon.h`・`rlvinventory.h`） | rlvforcewear.h が `RlvPredIsEqualOrLinkedItem` を include なしで使う／`isFoldedFolder` |

### レビュー対応（2026-10-31）で足した・変えた物

| ファイル | 関数 | 扱い | 理由 |
|---|---|---|---|

|---|---|---|---|
| rlvcommon.h | `RLV_AUTORESPONSE_INTERVAL`・`RLV_MAX_IMQUERY_LIST_MESSAGES`・`RLV_MAX_REPLIES_PER_SECOND` | 追加（全体の名前空間。rlvhandler.cpp と共有） | 設計 12 節の上限を 1 か所に |
| rlvcommon.h/.cpp | `RlvUtil::sendBusyMessage()` の第 4 引数 `fThrottle` | 追加 | 頼まれた返事（@version・@list の拒否）は間引かない |
| rlvcommon.h/.cpp | `RlvUtil::checkReplyBudget()`、`sendChatReply()` の第 3 引数 `fBudget` | 追加 | 返信は全体で毎秒 100 通まで |
| rlvcommon.h/.cpp | `RlvSettings::getDebugDeprecateExplicitPoint()` | 追加 | FS の `RLVaDebugDeprecateExplicitPoint`（既定 0）を static に |
| rlvnames.cpp | 名前表の作り直しの条件 | 書き直し（呼ぶたびに `getAvatars()` の ID 列を前回と比べ、違えば作り直す。1 秒の間隔は名前の変化だけ） | 着いたばかりの人の名前が遅れて伏せられるのを防ぐ |
| rlvactions.cpp | `rlvGetAvatarPosition()` | 書き直し（孤立・UI 用・アニメッシュのアバターを除く） | FS の `mIsDummy`・`isOrphaned()` の除外に当たる |
| rlvdefines.h | `RLV_ERRS`・`RLV_ASSERT`・`RLV_ASSERT_DBG`（LL_RELEASE_WITH_DEBUG_INFO の節） | 書き直し | FS は RelWithDebInfo（カリスの配布ビルド）でも LL_ERRS で止まる。外から届く命令で到達できる assert があるので、止めるのは LL_DEBUG だけ。RelWithDebInfo は呼び出し箇所ごとに 1 回 LL_WARNS_ONCE を出して続行 |
| rlvhelper.cpp | `RlvCommandOptionGetPathCallback`（`canAdd()`・同時に待つ数 64 まで） | 追加 | 待ち 1 件ごとに idle で 1 回引くので、force の連打で増え続けないよう上限 |
| rlvinventory.h/.cpp | `RlvGiveToRLVTaskOffer` 構築子（`strOfferName` 引数）・`getOfferedFolderName()`・`changed()` | 追加／書き直し（申し出のフォルダ名と完全一致し、受諾から 120 秒以内に足された物だけ取る） | 取引 ID が無い代わりの照合を狭める（レビュー S-L7） |
| rlvinventory.cpp | `RlvWearableItemCollector::getWearAction()`・`getFoldedParent()` | 書き直し（たどる回数を `cMaxFolderDepth` で打ち切り） | 壊れた木・循環で止まらないのを防ぐ（レビュー S-L6） |
| rlvhandler.cpp | `checkCommandOrigin`・`deferCommandLine`・`onDeferredLinesTimer`・`removeRetainedCommands`・`countBehaviour`・`scheduleLoginLastLocationUpdate`・`onCommandBatchDone` | 追加 | 一時装着の判定の保留（S-M1）、@notify の上限、設定の書き込みのまとめ |
| rlvhandler.cpp | `cleanup`・`onGC`・`processIMQuery`・`onIMQueryListResponse` | 書き直し | 配布ビルドで落ちない・通知の確認・相手の検証 |
| rlvhandler.cpp | `sendIMQueryList`・`checkAutoResponseThrottle` | 削除 | `RlvUtil::sendIMMessage` に一本化。@list の 60 秒間引きは FS に無いので廃止 |

## FS との意図した差（設計 12 節の表に加える）

| # | 何 | FS | Charis |
|---|---|---|---|
| 10 | `=y`・`@clear` | 使わない設定の命令でも通る/数える | 上限・保留・使わない設定の判定に関わらず必ず通す（数えない）。ログイン中も保留せず、保留中の追加を消すだけ |
| 11 | 一時装着の命令 | 物体の届く順に依存 | 自分の物と確定するまで 10 秒・1 物体 32 行・全体 128 行まで保留。拒否待ちの Experience と名前が一致する一時装着は止める |
| 12 | `@notify` | 上限なし | 1 物体 16・全体 64 登録 |
| 13 | 返信の送信 | 上限なし | 全体で毎秒 100 通（`RlvUtil::checkReplyBudget`） |
| 14 | `RLV_ASSERT`（配布ビルド） | 落ちる（LL_ERRS） | 警告して続行（LL_DEBUG だけ落ちる） |
| 15 | `@list/@except` の確認 | 毎回 | 毎回（FS と同じ）。通知テンプレートが無ければ断る |
| 16 | `@tpto` 座標 | 範囲なし | 地域内・高さ 4096 に clamp。3 つ目以降の選択肢は FS どおり無視 |
| 17 | `#RLV/~` の受け渡し | 取引 ID | 申し出のフォルダ名＋受諾から 120 秒の窓 |
| 18 | 自動応答 | 毎回 | 相手ごとに 60 秒に 1 回（@version の返事と imquery_list_deny は間引かない） |
| 19 | `RestrainedLove` | 設定ファイルで変わる | 設定の取り込み・プリセット XML では変わらない |

### 再レビュー対応で足した物

| ファイル | 関数 | 扱い | 理由 |
|---|---|---|---|
| rlvcommon.h/.cpp | `RlvUtil::checkReplyBudget(idObject)`、`sendChatReply`・`sendChatReplySplit` の `idObject` 引数、`RLV_MAX_OBJECT_REPLIES_PER_SECOND` | 追加 | 返信は 1 物体 20 通/秒（全体 100 通/秒とは別。表は 64 件で終わった枠を掃除） |
| rlvhandler.h/.cpp | `isBlockedObjectOrRoot()` | 追加 | 止めた物体の子プリムも止める |
| rlvhandler.h/.cpp | `clearLinkset()` | 追加 | 拒否した Experience のリンクセット全部の制限を外す（名前一致の時と cap の時） |
| rlvhandler.h/.cpp | `m_PendingExperienceLookups`（30 秒）・`onExperienceEventTracked()`・`hasPendingExperienceLookup()` | 追加 | Experience の照会が返るまで一時装着の行を保留する |
| rlvhandler.h/.cpp | `m_PendingListQueries`（900 秒で忘れる） | 追加 | @list・@except の確認は相手ごとに待ち 1 件まで |
| rlvhandler.h/.cpp | `m_NotifyCounts`・`m_nNotifyTotal` | 追加 | @notify は追加・削除の時に増減して数える（1 物体 16・全体 64）。`countBehaviour()` は削除 |
| rlvhandler.h/.cpp | `runTakeBackCommands()`・`rlvTakesBack()` | 追加 | =y・=rem・@clear は常に即実行し、保留中・ログイン中に溜めた追加を取り消す |
| rlvhooks_core.cpp | `handleOwnerSay()` | 書き直し | Blocked・NotCommand の行でも =y・=rem・@clear を通す |
| rlvhelper.cpp | `RlvBehaviourNotifyHandler::sendNotification()` | 書き直し | 物体ごとの返信の枠に数える |

## rlv-locks（装着・衣装・フォルダの鍵と force の着脱）

`RLV-LOCKS:` の目印は全部回収した。FS からの移植で変えた所:

| ファイル | 関数 | 扱い | 理由 |
|---|---|---|---|
| rlvlocks.h/.cpp | `RlvAttachPtLookup` | 移植元から外した（rlv-core の rlvattachpt.*） | 鍵に依らない。二重定義になるため |
| rlvlocks.h/.cpp | `RlvAttachmentLockWatchdog::onDetach()` の `FSLSLBridge::canDetach()` | 削除 | Charis に LSL ブリッジは無い |
| rlvlocks.cpp | `RlvAttachmentLocks::updateLockedHUD()` の `set_use_wireframe(false)` | 書き直し（`gUseWireframe = false`） | FS の関数は llviewermenu.cpp（@viewwireframe=段 3）にあり、フラグを代入するだけ |
| rlvlocks.cpp | `RlvFolderLocks::addFolderLock()` の `gAgentAvatarp->setAttachmentCallback()` | 書き直し（`gRlvAttachmentChanged` の信号） | FS が足した公式の信号。W1・W2 のフック（rlvhooks_locks.cpp）が鳴らす |
| rlvlocks.cpp | `RlvAttachmentLockWatchdog` の `addAttachmentRequest(item, pt, true, true)` 2 か所 | 書き直し（`RlvForcedAttachScope` ＋ 3 引数） | 公式の `LLAttachmentsMgr::addAttachmentRequest()` に第 4 引数 `fRlvForce` が無い。rlv-wear の W5 が `plus::rlv::isForcedAttachRequest()` を見て鍵の判定を飛ばす |
| rlvlocks.cpp | `RlvFolderLocks::hasLockedFolderDescendent()` の `collectDescendentsIf(…, f, false)` | 書き直し（`RlvInventory::collectDescendentsIf()`） | 公式に第 6 引数が無い。core の深さ上限つきを使う |
| rlvlocks.h | `RlvForcedAttachScope`・`gRlvAttachmentChanged` | 追加 | 上の 2 件 |
| rlvforcewear.cpp | `RlvForceWear`（rlvhelper.cpp 1248〜1870 から） | 移植（`isWearingItem()`・`isStrippable()` は rlv-core の rlvwearquery.cpp） | |
| rlvforcewear.cpp | `RlvForceWear::forceFolder()` の `collectDescendentsIf(…, f, true)` | 書き直し（`RlvInventory::collectDescendentsIf()`） | 同上（深さ上限） |
| rlvforcewear.cpp | `RlvForceWear::isForceDetachable()` の LSL ブリッジの除外（FS FIRE-4453） | 削除 | ブリッジが無い |
| rlvforcewear.cpp | `RlvForceWear::done()` | 書き直し | 設計 1.4: 公式の `LLAppearanceMgr` に `updateCOF(体, 服, 装着物, ジェスチャー, …)` と `removeItemsFromAvatar(ids, nop, cb, immediate)` が無い。体・服は `wearItemsOnAvatar()`、削除は 1 本のコールバックを共有する `removeCOFItemLinks()`（#25）、ジェスチャーは COF にリンクして `LLGestureMgr::activateGestures()`。装着物は FS と同じく `updatePendingAttachments()` が COF 更新の後に要求する |
| rlvhelper.cpp | `RlvBehaviourDictionary()` の段 2 の登録 | 戻した（`RLV-LOCKS:` の目印） | addattach/remattach/detach/addoutfit/remoutfit/sharedwear 系/*this(_except)/attach* と同義語/detachme と force の attach*/detach* |
| rlvhandler.cpp | `processAddRemCommand()` の @attachthis/@detachthis(_except)・@addoutfit/@remoutfit・@sharedwear 系・@unsharedwear 系、`RlvBehaviourAddRemAttachHandler`・`RlvBehaviourHandler<RLV_BHVR_DETACH>`・`onAddRemFolderLock(Exception)()` | 戻した | FS のまま |
| rlvhandler.cpp | `processForceCommand()` の `RLV_CMD_FORCEWEAR`、`RlvForceHandler<RLV_BHVR_DETACHME>`・`RlvForceRemAttachHandler`・`RlvForceHandler<RLV_BHVR_REMOUTFIT>`・`onForceWear()`・`onForceWearCallback()` | 戻した | FS のまま |
| rlvhandler.cpp | `onAttach()`・`onDetach()`・`onGC()` の `gRlvAttachmentLocks` の呼び出し（`verifyAttachmentLocks()` を含む） | 戻した | FS のまま |
| rlvhandler.cpp | `onGetAttach()` ほか @getattach 系・@getoutfit 系の鍵の項（`RLVaHideLockedAttachments`・`RLVaHideLockedLayers`、`canAttach`・`canWear`・`isForceDetachable`・`isForceRemovable`） | 戻した | FS のまま |
| rlvhandler.cpp | `RlvHandler::cleanup()` | 追加（見張りの削除） | 全部の制限を外した後で見張りとそのタイマーを持ち越さない（149 (E)） |
| rlvcommon.h/.cpp | `rlvCanDeleteOrReturn()`（2 つ）・`RlvSelectHasLockedAttach`・`rlvPredCan(Not)WearItem()`・`rlvPredCan(Not)RemoveItem()`（`const LLViewerInventoryItem*` と `const LLUUID&` の `CanRemove`、`CanNotRemove` は `const LLViewerInventoryItem*` だけ。宣言だけで定義の無かった `rlvPredCanNotRemoveItem(const LLUUID&)` は消した。関数オブジェクト 4 つ） | 戻した（`rlvCanDeleteOrReturn` は @unsit の項だけ。@edit・@rez・`RlvActions::canEdit()` は段 3） | |
| rlvcommon.cpp | `RlvUtil::filterScriptQuestions()` の `SCRIPT_PERMISSION_ATTACH` | 戻した（`RLV_SCRIPT_PERMISSION_ATTACH`） | 装着物を付けられない時は権限の「装着」を断る |
| rlvactions.h/.cpp | `RlvActions::canPasteInventory()`（2 つ） | 戻した | フォルダの鍵の貼り付け判定（W16・W17 の invguard から rlv-wear が呼ぶ） |
| rlvhooks.h・rlvhooks_locks.cpp | `onAttachObject()`・`onDetachObject()`・`onObjectDetached()`・`canDetachAttachment()`・`isForcedAttachRequest()`・`onSavedAssetIntoInventory()` | 追加 | 公式の口（W1〜W3）。後ろ 2 つは rlv-wear の W5・`llinventorymodel.cpp` が呼ぶ。`RlvForceWear::done()` を `setCommandBatchDone()` に登録する |

設計 2.3 の W4（`llviewerobject.cpp` の `processUpdateMessage()`）は FS の `llviewerobject.cpp`（親が自分のアバターで付く点が `RLV_LOCK_ADD` なら `AttachItemID` を読んで `onWearAttachment(idItem, RLV_WEAR_ADD)`）と同じ所に `plus::rlv::onRezzedAsAttachment()` として置いた（本体は rlvhooks_locks.cpp）。

### FS との意図した差（rlv-locks。設計 12 節の表に加える）

| # | 何 | FS | Charis |
|---|---|---|---|
| 23 | ジェスチャーを force で着る | `updateCOF()` で COF のリンクと有効化 | `addCOFItemLink()` ＋ `LLGestureMgr::activateGestures()`（結果は同じ） |
| 24 | 見張りの再装着が鍵を飛ばす | `addAttachmentRequest()` の第 4 引数 | `RlvForcedAttachScope`（rlv-wear の W5 が見る） |
| 25 | `RlvForceWear::done()` の削除 | 1 本の `LLUpdateAppearanceOnDestroy` を削除と追加で共有し COF のリンクを即時に消す | `removeCOFItemLinks(id, cb)` ＋ `clearPendingAttachmentLink()` を同じ 1 本のコールバックで。公式の `removeCOFItemLinks()` が即時に消すのは AT_OBJECT のリンクだけで、服・体の部品のリンクは遅れて消え、全部消した後に外見を 1 回更新する（結果は同じ）。`addDoomedTempAttachment()` は公式で private なので呼ばない（装着物は `userRemoveMultipleAttachments()` で先に外している） |
| 26 | `m_remAttachments` | 生ポインタ | `LLPointer`。`done()` で `isDead()`・`isAttachment()` を確かめてから使う |
| 27 | `@detachthis`/`@attachthis` の `=y` | 在庫でフォルダを解き直す | `=y` はオプションの文字だけで鍵の元を決める（フォルダが消えていても鍵が残らない）。`RlvObject` が消える所で、その物体の鍵を全部外す |
| 28 | 層の範囲 | 添字をそのまま使う | `forceFolder()`・収集器とも `WT_COUNT` 以上の層は飛ばす |
| 29 | 見張りの再装着 | 回数・時間の上限なし | 10 回で諦める。鍵が無くなれば待ちを捨てる。`m_PendingDetach` は 60 秒で捨てる |
| 30 | 保留中に溜めた行の順（`=y` と force） | `=y` は即実行（core の 12 節 #10） | 同じ。force を含む行で `=y` が force より先に走る（FS は行の順）。安全側（解除は止めない）の意図した差 |
| 31 | 遅れて走る `@attachthis=force` 系 | 走る時に確認しない | 走る時に RLVa が有効か・物体が止められていないかを確かめる |
| 32 | `CanNotChangeAppearanceUntilLoaded`（force で、外見の読み込み前） | 毎回 modal | 1 分に 1 回 |
| 33 | 子プリムの鍵 | 呼ぶ側が根を渡す | `isLockedAttachment()` 系は根に直してから引く。`onGC` の鍵も根の ID |
| 34 | `isLockedFolder()` の親たどり、`getLockedItems()` の巡回 | 深さ上限なし | `cMaxFolderDepth`（64）。`RlvInventory::collectDescendentsIf()` |
| 35 | `RlvWearInfo::dumpInstance()` の品名ログ | INFO | `LL_DEBUGS("RLV")`（タグを入れた時だけ） |
| 36 | 体の部品・服の force の着方 | `updateCOF()` | `wearItemsOnAvatar()` を通る。同種の体の部品が 2 つあると両方リンクし得る／ゴミ箱の品と COF にある品は飛ばす／服の層が上限なら追い出す（公式の `wearItemsOnAvatar()` の規則） |
| 37 | `processRetainedCommands()` の最後 | 呼ばない | `onCommandBatchDone()` を呼び、溜めた force を流す（安全側の差） |

### rlv-comm（チャット・IM・テレポート・権限・申し出の公式側フック。`rlvhooks_comm.cpp`）

各フックの中身は FS の同じ関数の `[RLVa:KB]` ブロックのまま。呼ぶ場所は FS と同じ（C4 だけ FS の窓 `FSFloaterIM::sendMsg()` でなく全部の IM が通る `LLIMModel::sendMessage()`）。

| ファイル | 関数 | 扱い | 理由 |
|---|---|---|---|
| rlvhooks_comm.cpp | `filterSentChat()`・`checkChatVolume()`・`allowsChatAnimation()`（B1・B2） | 戻した | `send_chat_from_viewer()` は Charis では const 参照なので、公式側で局所コピーを作ってから渡す |
| rlvhooks_comm.cpp | `filterSentIM()`・`filterOutgoingIMText()`・`filterHistoryText()`（C4・C12） | 戻した（置き場所だけ違う） | FS は窓ごと。Charis は `LLIMModel::sendMessage()` の頭。セッションが無い時は FS と同じく文を差し替える |
| rlvhooks_comm.cpp | `handleIMQuery()`・`filterReceivedIM()`・`filterInvitationText()`（C1・C3） | 戻した | 問い合わせの条件（オンライン・ミュート・友だちのみ・4 字以上）は `RlvHandler::processIMQuery()`（core） |
| rlvhooks_comm.cpp | `autoAcceptTeleport()` ほか D3 の 4 つ、`refuseTeleportTo*()`・`canCancelTeleport()`（D1・D2・D5） | 戻した | |
| rlvhooks_comm.cpp | `filterScriptQuestions()`（D7） | 戻した（`known_questions` に対して掛ける） | 差 #45 |
| rlvhooks_comm.cpp | `anonymizedName()` と E46〜E48 の名前の差し替え | 書き直し（`rlvanonym` の SLURL でなく匿名の文字そのもの） | 差 #46 |
| rlvhooks_comm.cpp | `onAgentFolderOffer()`・`onTaskFolderOfferAccepted()`・`onTaskFolderOfferDeclined()`（W23） | 戻した | FS の `inventory_offer_callback()` / `inventory_task_offer_callback()` |
| rlvhandler.cpp | `RlvForceHandler<RLV_BHVR_TPTO>::onCommand()`・`onTeleportCallback()` | 戻した（`RLV-COMM:` の目印） | `teleportViaLocationLookAt(pos, look_at)` が llagent にできた（設計 1.4） |
| rlvhandler.cpp | `addBlockedObject()`・`hasUnresolvedBlockedObject()` | 変更 | 照合待ちの拒否 Experience は 300 秒で捨てる（core の申し送り ③） |
| rlvhandler.cpp | `rlvTakesBack()`（`@clear:<filter>`） | 変更 | 保留中の命令ごとに文字列を作らず、使い回しの置き場で比べる（core の申し送り ⑤） |
| rlvhelper.cpp | `RlvBehaviourNotifyHandler::sendNotification()` | 変更 | `@notify` の返信の枠を、同じ物体の問い合わせへの返事の枠と分ける（core の申し送り ④） |

### FS との意図した差（rlv-comm。設計 12 節の表に加える）

| # | 何 | FS | Charis |
|---|---|---|---|
| 44 | グループ・会議の発言の `@recvim`（設計 2.2 の C2 `LLIMMgr::addMessage()`） | FS にも無い（招待の C3 だけ） | FS と同じ。招待（C3）だけで、始まったセッションの発言は @recvim で伏せない（設計の C2 は FS に無いので外した） |
| 45 | `process_script_question()` の権限の絞り込み | 聞かれた全ビット（`questions`）を `filterScriptQuestions()` に渡し、残りが 0 なら自動で答える | 知っている権限（`known_questions`）だけを渡す。知らないビットは許可に含めない（安全側） |
| 46 | 匿名にした名前の出し方（申し出・TP の誘い・TP の依頼・記録） | `secondlife:///app/agent/<id>/rlvanonym` のリンク | 匿名の文字そのもの（リンクにしない）。リンクの登録（E5）が無くても漏れず、押して相手のプロフィールが開くこともない |
| 47 | `@tpto` の向きが無い時 | カメラの向き（`at` 軸）を向きとして送り、着いた後の向きの保持（`mbTeleportKeepsLookAt`）は常に切る | 向きが zero の時は今までどおり `mbTeleportKeepsLookAt` を立ててカメラの向きを保つ（向きがある時は FS と同じ） |
| 48 | 取消ボタン | `FSAlwaysShowTPCancel`（FS の設定）で取消ボタンを出せる | その設定は無い。`@tpto`・`@accepttp` の TP は取消ボタンを出さない |
| 49 | C4 の入口 | 窓（`FSFloaterIM::sendMsg()`）で差し替える | `LLIMModel::sendMessage()` で差し替える（会話の窓から出る文）。`groupchatlistener.cpp` が直接送る LEAP の文も `LLIMModel::sendMessage()` を通るので判定される（FS より厳しい側）。溜めた文は送る時に判定される |
| 50 | D7 の注意の権限の知らせ | 注意の権限は全部近くのチャットへ | `rlv_notify`（@acceptpermission が自動で許した時）だけ近くのチャットへ。注意の権限の記録は Charis の今までの動き（`PermissionsCautionEnabled`）のまま |
| 51 | 保存された履歴（LL のチャットサービス、グループ・会議のサーバー履歴）の @recvim | FS に無い（原文が見える） | 表示する時（主スレッド）に、送り主が @recvim で止められていれば本文を差し替える（C12） |
| 52 | 友だち申請・グループ通知・支払いの文・DND の自動応答（送る側）、DND の自動応答（受ける側） | FS に支払いの文だけ | @sendim で止められた相手へは本文を差し替える／DND の自動応答は送らない。支払いの文は送る時にも確かめる。受けた `IM_DO_NOT_DISTURB_AUTO_RESPONSE` の本文は @recvim で差し替える（`llimprocessing.cpp`） |
| 53 | `@list`/`@except` を「常に送る」にした後 | 毎回すぐ答える | 相手ごとに 10 秒に 1 回・全体で 1 分に 30 回まで（超えた分は隠して答えない）。`ignorable_dialogs.xml` は PC の全アカウントに効く |
| 54 | 再開した TP・受諾した誘い・TP 依頼への「はい」 | 確かめない | 再開（`LLAgent::startTeleportRequest()` の `kRestartPending`）で D1・D2 を見直し、断ったら TP の状態を `TELEPORT_NONE` に戻して要求を捨てる。RLVa が出した TP（`@tpto`、`@sittp`・`@standtp` の戻り）は、要求の印（`setRlvMade()`）で見分けて見直さない。`lure_callback()`・`mature_lure_callback()` は `canAcceptTpOffer`、`teleport_request_callback()` の「はい」は `canAcceptTpRequest` を見直す |
| 55 | `IM_FROM_TASK` の `objectim` のリンク | 名前・場所・`rlv_shownames` を入れる | RLVa オンの時だけ FS と同じ。オフは今までのまま。リンクを読む側は rlv-show（E5・E10〜E12）が使う |
| 56 | `process_script_question()` の連投制限 | 自動回答を先に済ませ、制限は自動回答しない時だけ | 同じ（FS に合わせた順序） |
| 57 | 権限の自動回答の知らせ（`rlv_blocked`・`rlv_notify`） | 制限なし | 表示するものは全体で 1 分に 10 件まで（超えた分は答えるだけで表示しない）。連投制限は自動回答しない要求にだけ掛かる（FS と同じ順序） |
| 58 | 履歴の @recvim の判定先 | — | グループの会話は `canReceiveIM(グループ)`（C3 と同じ）、それ以外は送り主 |
## rlv-wear（装着・衣装・在庫のフック、research/248 2.3 の W5〜W22・W25・W26・E49）

公式ファイルの FS の `[RLVa:KB]` の塊を `plus::rlv::` の呼び出し（`rlvhooks_wear.cpp`）に置いた。FS の関数がカリスに無い所、形が違う所は下の表。在庫の鍵（`RlvFolderLocks`）の判定は `plus/invguard` の守りとして `rlvplus.cpp` の `RlvInvGuard` が答える（RLVa を有効にした時に登録、`cleanup()` で外す）。

| ファイル | 関数 | 扱い | 理由 |
|---|---|---|---|
| rlvhooks_wear.cpp | `allowAttachmentRequest()` | 書き直し | FS の `addAttachmentRequest(…, fRlvForce)` の塊。第 4 引数の代わりに `isForcedAttachRequest()`（rlv-locks の `RlvForcedAttachScope`）。`LLAgentWearables::areInitialAttachmentsRequested()` は公式に無いので、`onInitialAttachmentsRequested()`（`userAttachMultipleAttachments()` の最後。FS と同じ所）が立てる plus 側の旗 |
| rlvhooks_wear.cpp | `notifyWearableChanges()` | 書き直し | FS の `updateAgentWearables()` の塊（@notify の onTakeOff/onWear）。公式に `getWearableItemIDs(uuid_vec_t&)` と `areInitalWearablesLoaded()` が無いので、型ごとに `getWearableItemID()` で集め、「初期の外見の読み込み後」は `areWearablesLoaded()` で見る |
| rlvhooks_wear.cpp | `keepLockedCofItems()`・`dropUnwearableItems()`（`updateCOF()`） | 書き直し | FS は `updateCOF(体, 服, 装着物, ジェスチャー, append, idOutfit)` の重ね定義が新しい品を別の配列で受けるので、そこで `RlvPredCanNotWearItem` を掛ける。カリスの `updateCOF(category, append)` は COF の分とフォルダの分を同じ配列に入れるので、フォルダの分を入れる前の件数を覚え、その後ろだけを選ぶ。置き換えの時に COF の鍵付きを残す塊は `else if` として同じ位置に置いた。体の部品を `append` の時に並べ替える `reverse()`（公式）は残した（FS は外した。FS の force は `updateCOF()` を通るが、カリスの force は `wearItemsOnAvatar()` を通る＝#36 ので影響しない） |
| llappearancemgr.cpp | `removeCOFItemLinks()`・`removeCOFLinksOfType()`・`updateAppearanceFromCOF()` の `RLV_ASSERT`・`ForceMissingType` の試験用の塊 | 入れない | 動きの無い確認（assert・デバッグ試験）だけ。鍵付きの除外は `removeItemsFromAvatar()`（W8）と `userRemoveMultipleAttachments()`（W7）の保険で足りる |
| llviewermenu.cpp | `handle_selected_script_action()`（FS の関数） | 同じ所 | カリスでは `LLToolsSelectedScriptAction::handleEvent()` の先頭に置いた（FS はこの関数を作って分けた。中身の判定は同じ） |
| llviewermenu.cpp | `enable_show_HUD_attachments()`・`Object.EnableShowOriginal`・`derenderObject`（FS だけ） | 入れない | カリスに無い。HUD を隠す側（`LLViewShowHUDAttachments`）は止めてあり、メニュー項目の灰色化だけ無い |
| llviewermenu.cpp | `object_selected_and_point_valid()` | 同じ所 | FS は引数 `const LLSD&` を足す（`Object.EnableWear` は `object_is_wearable` に結ばれていて、実質いつも引数 0＝既定の点）。カリスは引数を足さず、既定の点の判定（`canAttachSelectionTo(0, nullptr)`）だけ置いた。点ごとの判定は `LLObjectAttachToAvatar::handleEvent()` が行う（FS と同じ） |
| llinventorybridge.cpp | `getPreferredType()`・`getSortGroup()`（#RLV の `FT_RLV` の印と並び） | 入れない | カリスの `LLFolderType` に `FT_RLV` と専用のアイコンが無い（見た目だけ。#RLV は普通のフォルダとして並ぶ＝FS で「FS のフォルダを上に並べない」にした時と同じ）。`isItemMovable()` の #RLV を動かさない所は FS と同じに名前で判定（`isSharedRootFolder()`） |
| llinventorybridge.cpp | `addMoveToDefaultFolderMenuOption()`・`LLInventoryAction::doToSelected()` の「既定のフォルダへ」 | 入れない | カリスに無い（FS だけ） |
| llinventorybridge.cpp | `LLTaskInvFVBridge::buildContextMenu()`（llpanelobjectinventory.cpp） | 書き換えず | カリスは名前の変更・削除が出来ない時は項目を隠す（FS は灰色）。`isItemRenameable()`・`isItemRemovable()` が止めるので同じ結果。「開く」だけは FS と同じに灰色にした |
| llpanelcontents.cpp | `onClickNewScript()` の予備の判定 | 同じ役の所に置いた | カリスは `onNewScriptFlyoutCommit()` が新しいスクリプトを作る。そこの先頭で鍵付きの装着物なら戻る |
| llpreviewscript.cpp | `onRunningCheckboxClicked()` | 差 | FS は止めた時にチェックボックスを戻さない（見た目と実際がずれる）。カリスは戻す |
| llselectmgr.cpp | `sendDropAttachment()` | 足した（FS に無い） | 設計 2.3 W6 のとおり。鍵付きの装着物は落とさない（`ObjectDrop` は装着物を外すため） |
| llselectmgr.cpp | `selectDelete()`・`selectDuplicate()`・`canDoDelete()`・`canDuplicate()` の `rlvCanDeleteOrReturn()` | 入れない | 段 2 では @unsit の項だけなので座る系（rlv-sit の S10）が入れる |
| llinventorymodel.cpp | `changeItemParent()` | 足した（FS に無い） | 設計 W15。鍵の掛かり方が違うフォルダの間へ動かさない（`invguard::refusesItemMove`）。木の移動・貼り付け（切り取り）・ギャラリーの全部の移動がここを通る |
| llinventorygallery.cpp | `dragItemIntoFolder()`・`paste(dest, …)` | 足した（FS のギャラリーには無い） | 設計 W17。木と同じに止める |
| plus/invguard | `blocksItemRemoval()` ほか品目・名前・貼り付けの問い合わせ、`Guard::asksUser()`、`refusesRemoval()`・`refusesMove()` の 2 段 | 足した | rlv-invguard の申し送り①: 確認の窓を出す守り（保護フォルダ）の答えは、窓を出さない守り（RLV の鍵）が「ダメ」と言わない時だけ聞く |
| plus/ao | `aoengine.cpp`・`aostore.cpp` の `protectedfolders::` の直接呼び出し 5 か所 | `invguard::` に替えた | 申し送り②: AO の削除でも RLV の鍵が効く |
| plus/outfitbackup | `restoreOutfit()` | 足した | 保存した外見を戻すのは COF を `updateCOF()` を通さず置き換えるので、`filterOutfitRestore()` で同じ規則（着られない新しい品を除く・鍵付きの今の品を残す）を掛け、結果を「RLV の制限のため…」で知らせる |
| plus/photoscreen | `photodetail_left.cpp`・`photoscreen.cpp` の HUD 切替 | 足した | 鍵付き HUD は隠さない |
| plus/lostattach | `refreshAttachments()` | 変更なし | `addAttachmentRequest(…, true)` を通る（W5）。@detach の鍵の物は付け直せる（追加は許される）。@addattach の鍵の点の物は付け直せない（鍵の意図どおり） |

### FS との意図した差（rlv-wear。設計 12 節の表に加える）

| # | 何 | FS | Charis |
|---|---|---|---|
| 38 | 外見の置き換えの時の新しい品の除外 | `updateCOF()` の重ね定義の中 | `updateCOF(category, append)` の中、フォルダの分を入れた直後に `dropUnwearableItems()`（結果は同じ） |
| 39 | 初期の装着の要求済みの旗・初期の外見の読み込み済みの旗 | 公式の `areInitialAttachmentsRequested()`・`areInitalWearablesLoaded()` | plus 側の旗／`areWearablesLoaded()` |
| 40 | `sendDropAttachment()`・`changeItemParent()`・ギャラリーの移動と貼り付け | 鍵を見ない | 鍵を見る（鍵の抜け道を塞ぐ追加） |
| 41 | 保存した外見の復元 | 無い機能 | 鍵に従う（`filterOutfitRestore()`） |
| 42 | 鍵付き HUD の撮影画面での切替 | 無い機能 | 隠さない |
| 43 | スクリプトの実行チェックの切替を止めた時 | チェックボックスがずれる | 戻す |
| 59 | 外部スクリプトエディタの WebSocket（llscripteditorws.cpp。FS に無い） | 無い機能 | 鍵付きの装着物は公開せず、公開済みでも `isObjectPublished()` が偽を返す（W19・W20・W22 の抜け道塞ぎ） |
| 60 | ギャラリーの名前変更・「着ている物を削除」、`ungroup_folder_items()`（RLV の鍵の時だけ全部確かめる）、`consolidateForType()` | 鍵を見ない | 名前変更は `rename_category()` 経由で灰色、削除は鍵付きの着ている物とそれを含むフォルダを除く（`dropLockedFromDeletion()`）、動かせない物があれば元のフォルダを消さない |
| 61 | 保存した外見の復元で体の部品 | — | 絞り込みで型が空いた体の部品は今の COF のリンクで補う（体の部品が消えない） |
| 62 | 名刺の「IM を始める」 | `LLAvatarActions::startIM()` | RLVa 有効時は同じ（@startim。公式は `addSession()` 直呼びなので置き換えた） |
| 63 | `Avatar.EnableCall` | `enable_avatar_call()` | 同じ関数を足して登録を差し替え（@shownames で名前が隠れた相手へは通話できない） |
| 64 | 公式の体の部品の `reverse()`（append 時） | FS は外した | 残した（#38）。`changeCategoryParent()`・AO のフォルダ削除にも RLV の鍵が効く（invguard 経由。FS は効かない） |
| 65 | `removeItemsFromAvatar()` の更新コールバック | 1 件でも外す時だけ作る | 同じ（RLVa 有効の時だけ。オフは公式のまま先に作る） |

### rlv-show の差（#66〜#105）

| # | 何 | FS | Charis |
|---|---|---|---|
| 66 | 窓の門（E1） | `LLFloaterReg::setValidateCallback`（boost の信号） | llui の関数ポインタ `gPlusFloaterGate`。フィルターがある間だけ差す。`toggleInstanceOrBringToFront()`・`showInstanceOrBringToFront()` は開いていない窓だけ確かめる（開いている窓は閉じる・前に出すを止めない） |
| 67 | 横パネルの門（E2） | `LLFloaterSidePanelContainer::setValidateCallback` | llfloatersidepanelcontainer.cpp の中の関数ポインタ（ヘッダーは変えない） |
| 68 | 名前が伏せられた近くのアバターのプロフィール・`inspect_avatar`（E1） | 入口を一つずつ塞ぐ（窓自体は止めない） | 同じ（窓の門では止めず、入口ごとに塞ぐ。2026-11-01 の決定で FS と同一に戻した） |
| 69 | `aboutland`・`map`・`minimap` の有効判定（E3） | `Floater.CanShow`・`RLV.EnableIfNot` | `aboutland` は `Plus.Rlv.CanShowFloater`、`map`・`minimap` は FS と同じ `RLV.EnableIfNot`（rlvhooks_ui.cpp にあるものを使う）。オフでも登録し、オフなら常に有効 |
| 70 | `rlvanonym` の名前（E5） | `extern rlvGetAnonym` | 関数ポインタ `gPlusUrlAnonymName`。オフ・ポインタ空なら普通の名前。`LLUrlEntryAgentUserName::getName()` には足していない（FS にも無い） |
| 71 | 表示名の強制（F1） | `LLAvatarNameCache::setForceDisplayNames`（llmessage を変更） | `setUseDisplayNames(true)`。設定 `UseDisplayNames` が戻された時は落ち着いた後に掛け直す |
| 72 | 浮き文字の引き直し（F5） | `LLHUDText::refreshAllObjectText()`・`mObjText` | `gObjectList` を一巡し `mHudText` が空でない物だけ `restoreHudText()`。写しは作らない |
| 73 | ナビバー・情報バーの更新、People の小地図、近くの一覧の名前の更新 | FS の関数（`refreshLocationCtrl()`・`updateAvatarNames()` など） | 公開されている関数で同じ結果（`handleLoginComplete()`・`handleDisplayNamesOptionChanged()`、名前の無い layout_panel）。ボイスの話者一覧に当たる Conversations の参加者一覧は #109 |
| 74 | TP 履歴の「隠した項目」の通知 | `mHistoryChangedSignal(-1)`（FS は friend） | friend で対応済み（`PlusRlvTeleportHistory`。`llteleporthistory*.h` に印付きの friend 宣言） |
| 75 | 一覧への @shownames の伝え方（E8） | 一覧ごとに behaviour の信号へ接続 | `showNamesGeneration()`（@shownames と例外が変わるたびに増える数）を `draw()` で比べる。初めて名前を伏せる一覧が現れた時に一度だけ接続 |
| 76 | `getNameForDisplay()` の引数（E8） | 表示名・ユーザー名の 2 つのフラグ | カリスの一覧の `show_complete_name`・`force_complete_name` |
| 77 | People の「近く」の一覧（E27） | 自前のレーダーなので `setRlvCheckShowNames(true)` を外している | 公式の一覧を使うので呼ぶ（元の RLVa と同じ） |
| 78 | 近くのチャットの履歴で伏せた発言者のアイコン（E10） | fschathistory 側 | `Unknown_Icon` にし、`/me` の名前も名前キャッシュから引き直さない |
| 79 | 物体の行のトースト・履歴の SLURL（E10・E12） | 地域名入りの `getSenderSLURL` | RLVa が作った objectim の `chat.mURL`（comm 差 #55）を使う |
| 80 | アバター選択の「近くの人」タブ（E13） | `onList()` の時だけ止める | `postBuild()`・`onTabChanged()` でも止める |
| 81 | About の RLV の行（E7） | AboutLibs の中 | AboutLibs の後に `AboutRLV` を 1 行（strings_plus.xml） |
| 82 | アバターリストの項目（E9） | `setShowInfoBtn()` で判定 | カリスはマウスを乗せた時だけボタンを出すので `onMouseEnter()`・`updateRlvRestrictions()` に置く。アイコンのツールチップは項目側で消す（`LLAvatarIconCtrl` は触らない） |
| 83 | ミニマップの「About Land」（E24） | 窓の門（`canViewParcelProperties()`）だけで止める。メニュー項目は有効のまま | 同じ（項目は有効のまま。押しても窓の門が止める） |
| 84 | ミニマップのツールチップの匿名の行（E24） | floater_map.xml に `[AGENT]` | XML は変えず、コードで先頭の行に挿入 |
| 85 | ミニマップの区画の持ち主・友だちの色（E24） | `rlvanonym` のリンク／FS 独自の色の関数 | 匿名の文字（#46 と同じ）／伏せる相手では公式の友だちの色を普通の色に戻す |
| 86 | 物体の権限の作成者・持ち主（E29） | 名前が全部同じ時だけ伏せる（違えば Multiple） | 公式は違っていても最初の 1 人を出すので、出す名前で判定する（より厳しい）。写真のアイコンも隠す。リンクは作らず匿名の文字 |
| 87 | `requestTeleport()`・`offerTeleport()` が近くの一覧のメニューか（E28） | メンバー `m_fRlvCheck` | ヘッダーは変えず `this == &gNearbyPeopleContextMenu`。名前の文脈は RAII の `TeleportNamesScope` |
| 88 | 場所の窓の地図ボタン（E31） | 場所の情報を開いている時も @showworldmap でも有効になる | 無効にする |
| 89 | プロフィールの「地図」「TP」（E32） | ボタンを更新 | Charis のプロフィールにはメニューだけ。`onEnableMenu()` で判定（TP の誘いは comm D8） |
| 90 | @showloc 切替時のピック・広告の更新（E33） | 各パネルが信号へ接続 | `updateProfilePlaceButtons()` が開いているプロフィールを探して更新（rlvui_show.cpp から呼ぶ） |
| 91 | 物体の情報の「前の所有者」（E36・E37） | 名前を出す | @shownames で匿名にする（自分は除く） |
| 92 | 通報の窓の地域名（E16） | コメントアウトされていて隠さない | @showloc の時は `postBuild()`・`getObjectInfo()` で隠す |
| 93 | 世界地図の Go Home・座標ラベル（E17） | RLV の判定で上書き／`getChild("events_label")` が別のラベルを隠す | 初心者島の判定と RLV の両方を満たす時だけ有効／座標と同じパネルの中から探す |
| 94 | 渡した物の記録（E18） | 相手の名前が引ければフォルダでも「Inventory item offered to [NAME]」の文（ITEM_NAME 無し） | 同じ（`inventory_item_offered_rlv`）。名前が引けない時は既存の文（品物名つき）。strings_plus.xml に足した |
| 95 | 浮き文字（E20） | 毎回文字を写す | 制限が無い時は写さずそのまま渡す。制限中は使い回しのバッファ |
| 96 | 外の物体の調べる窓（E22） | `rlv_shownames` が無い時は前回の値が残る | 開くたびに「隠さない」から始める |
| 97 | 物体の調べる窓のボタン（E15） | `getSelectedNode()` をヘッダーに足す | ヘッダーは変えず `onSelectObject` の印の中で選択の行を探す |
| 98 | `teleportToClickedLocation()` の @tplocal の距離（E42） | `mPick`（前に拾った点） | `mHoverPick`（クリックした点） |
| 99 | 窓の題「名前 @ 地域」・土地のツールチップ（E40・E42） | 地域名・区画名・持ち主を出す | @showloc の時は隠した地域名・出さない。@shownames の時は持ち主（個人）を匿名 |
| 100 | TP 履歴の隠した項目（E40・E41） | `mTitle` だけ | `mFullTitle` にも同じ文字（NavBarShowCoordinates の時に空欄にならない）。0 位置の判定と保存しない処理は RLVa 有効の時だけ |
| 101 | 状態バー（E39） | 区画の文字・情報ボタン・土地を買うボタン | Charis の状態バーにはどれも無いので入れない（同じ働きは E23・E35） |
| 102 | E25 `llpanelland.cpp` `onClickClaim()` | 判定がコメントアウトされていて何もしない | 入れない（FS と同じ） |
| 103 | E19 注視の名前 | `DebugLookAtShowNames` | Charis に該当機能が無いので入れない |
| 104 | plus の機能の名前（radar・areasearch・avatarattach・subtitle・camerashot・camerapath・focuslock・tts・chatconsole） | （FS にあるのは radar・areasearch のみ） | 名前を匿名にする。radar は例外のアバターは出す、メニューは選択ごとに判定。areasearch は作者が持ち主と同じ時も匿名、グループ名は伏せない。subtitle は近くの相手だけ（本文も名前・地名のフィルタを通す）。tts・chatconsole は A1 で匿名にした行を `chat.mRlvNamesFiltered` で受け取る（設計 3.4 の「変更なし」は誤りだった） エリアサーチは @shownames の例外の相手を名前で出す（FS の `RLVa_hideNameIfRestricted` は全員を隠す）。People の「近く」の友だち追加ボタンは相手ごとに判定（FS のレーダーは @shownames の間は全員で無効）。 |
| 105 | 撮影ツールの名前つきカメラ・tplag のログ・posermcp の status | （FS に無い） | @showloc の間は一覧に出さず保存もしない。地域名と座標を伏せる |
| 106 | About の「RestrainedLove API: （無効）」行（E7） | RLVa がオフでも出る | 同じ（公式との差）。`RLVaStatusDisabled` |
| 107 | `…/rlvanonym` の URL（E5） | オフでもリンクの無い文字になる | 同じ（公式との差。`rlvanonym` の URL を作るのは RLVa だけ） |
| 108 | エリアサーチの座標・区画名の列（設計 3.4 の L） | — | FS にも無いので入れない |
| 109 | 近くの参加者一覧（Conversations）・メンション候補（S-H1） | FS は自前のボイス一覧で `setRlvCheckShowNames`、メニューは出さない／メンション候補は切替時に `hideHelper` するだけ | Conversations の参加者を匿名にし（`mName` も）、メニュー・コマンドは近くの人のメニューと同じ項目だけ、「i」ボタンとアイコンのツールチップは出さない。近くのチャットで @shownames 中はメンション候補の窓を開かない（llui の関数ポインタ `gPlusChatMentionGate`）。設計 2.4 の「ボイスの話者一覧は公式に無い」は誤りで、この参加者一覧が相当。範囲は「親が近くのセッション（ID が空）の参加者の行」だけ。1 対 1 の IM・グループ・会議の行は FS と同じく止めない |
| 110 | トーストのアイコン（E11） | 本人の顔写真のまま（ツールチップだけ消す） | `Unknown_Icon`（チャット履歴に揃えた） |
| 111 | 近くのチャットの記録ファイル（S-H2） | `!hasBehaviour(SHOWNAMES)` の時だけ本名 | `chat.mRlvNamesFiltered` の行は匿名のまま記録（FS と同じ結果） |
| 112 | 物体チャットの見出し・objectim の slurl（S-M1） | 本当の地域名と座標 | @showloc 中は近くの地域なら隠し、所有者も匿名にする |
| 113 | キーワード通知の本文（S-M5） | FS に無い機能 | RLVa 有効時は名前と地名を伏せる |
| 114 | 名前の引き直し（S-M6） | 信号ごとにすぐ（全物体の走査を含む） | idle で 1 フレーム 1 回にまとめる。名札の uuid は 16 個まで貯め、超えたら全体。浮き文字は物体一覧を走査（`LLHUDText` の一覧が private のため） |
| 115 | 「場所」の現在地表示（S-M2） | 前の表示が残る | @showloc を入れたら一覧に戻す。`onOpen()`・`changedParcelSelection()` の門。`add_landmark` も止める。`filterFloaterShowLoc` で `places` は止めない（他人の区画へ移った後も開けるように） |
| 116 | 名前・地名の置き換え（S-M7・再確認） | 名前ごとに `\b名前\b` の boost::regex で順に置き換える（置き換えた結果を次の名前でまた走査する。Windows では OS の文字コード依存） | 文字列のまま照合し、元の文を 1 回だけ走査して最長一致で置き換える（結果は再走査しない＝膨張しない）。英字だけ大文字小文字を区別しない。境界は端が ASCII の英数字・_ の側だけ（日本語・記号の名前も伏せる）。出力は入力×8＋1KB まで（超えたら「...」で切る）。文中で置き換えるアバター名は 2 文字以上かつ 3 バイト以上（FS と同じ条件。1 文字の名前は文中を置き換えない）。地名は最短の制限なし（短い区画名も隠す）で、空の区画名・地域名は置き換えの対象にしない |
| 117 | インベ検索の作成者名（invexplorer・invfinder） | FS に該当機能なし | 自分の持ち物として E36 と同じ判定で匿名化。本名では検索に当たらない |
| 118 | 既知の未対応（低） | — | testscene.cpp:941 の `region_word` の組み立て行（出口は未確認）。ほかは 2026-11-06 に直した（research/275 L087・L088・L104〜L106: `gPlusPanelGate` は llfloatersidepanelcontainer.h で宣言、`isNearbyAgent` の前に null を見る、世界地図・E37 は下の 121〜123） |
| 119 | Performance の「近くのアバター」一覧（`llfloaterperformance.cpp`） | RLVa の処理なし（本名と「i」ボタンが出る） | @shownames で名前が隠れる相手は匿名にし、「i」ボタンを出さない（行は special id） |
| 120 | Pathfinding の所有者名（キャラクター・linkset 一覧） | RLVa の処理なし | @shownames で匿名（グループの所有は除く）。切替後は一覧を作り直した時に反映 |
| 121 | @tpto の TP を取り消せなくする時（S-L7、research/275 L086） | `refuseTeleportToLocation` の判定の中（TP が始まる前）で旗を立てる | `startTeleportRequest()` の後、TP が始まった時だけ（`noCancelForStartedTpto`）。始まらなかった @tpto で旗が残らない |
| 122 | 名前がまだ分からない相手への TP の要求（research/275 L105） | 名前を待ってから聞く（その時は `TeleportNamesScope` が終わっていて本名が出る） | @shownames で隠す相手なら待たずに、名前の代わりに「隠した」の文字で聞く（ID から作る匿名は名前から作る匿名と別の名前になるため）（`anonymizeUnknownTeleportRequestName`、llavataractions.cpp） |
| 123 | 開いている世界地図・物の情報（research/275 L104・L106） | 世界地図の座標・地域名は開き直すまで前のまま／E37 は毎回 setValue | @showloc の切替で `LLFloaterWorldMap::plusRefreshShowLoc()`／E37 は値が変わった時だけ書く |
