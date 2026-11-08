/**
 * @file invexplorer.cpp
 * @brief Inventory window like a file explorer: the detail pane on the right.
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

// research/220. floater_my_inventory.xml (an official file, <!-- plus --> block) holds the
// layout_panel "inv_detail_layout_panel" with <panel class="plus_inv_detail"> next to the
// inventory. This file has the panel and the two hooks of LLPanelMainInventory:
// onSelection() tells the panel what is selected, tick() shows the layout_panel while the
// window is wide enough. The pane is a read-only view: it changes nothing in the inventory.

#include "llviewerprecompiledheaders.h"

#include "invexplorer.h"

#include <unordered_map>

#include "llagent.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llbutton.h"
#include "llclipboard.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llfolderview.h"
#include "llfolderviewitem.h"
#include "llfolderviewmodelinventory.h"
#include "llgltexture.h"
#include "llframetimer.h"
#include "llinventorymodel.h"
#include "llpanel.h"
#include "llpanelmaininventory.h"
#include "llpermissions.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewertexture.h"

#include "plus/photoscreen/psui/pswidgets.h"

#include "plus/rlv/rlvhooks.h"

namespace
{
    constexpr char SETTING_ON[]        = "PlusInvExplorerDetail";
    constexpr char SETTING_MIN_WIDTH[] = "PlusInvExplorerMinWidth";
    constexpr char PANEL_NAME[]        = "plus_inv_detail";

    // How many frames to wait before looking again for a pane that was not found
    // (a floater without the pane; the search walks the floater's children).
    constexpr U32 RESOLVE_RETRY_FRAMES = 120;

    // The picture preview of a texture: the box it is fitted into (left 8, top 30 of the pane;
    // as wide as the pane allows, 244 at most).
    constexpr S32 PREVIEW_BOX_W = 244;
    constexpr S32 PREVIEW_BOX_H = 150;
    constexpr S32 PREVIEW_LEFT  = 8;
    constexpr S32 PREVIEW_TOP   = 30;
    constexpr S32 PREVIEW_GAP   = 4;

    class PlusInvDetailPanel : public LLPanel
    {
    public:
        bool postBuild() override
        {
            mTitle = getChild<LLTextBox>("detail_title");
            mText  = getChild<LLTextEditor>("detail_text");
            mPreview = getChild<plus::psui::PsPicture>("detail_preview");
            mPreview->setFit(plus::psui::PictureFit::Stretch);   // its rectangle already has the picture's aspect

            mPropertiesBtn = getChild<LLButton>("detail_properties_btn");
            mPropertiesBtn->setClickedCallback([this](LLUICtrl*, const LLSD&)
            {
                if (gInventory.getItem(mId))
                {
                    LLFloaterReg::showInstance("item_properties", LLSD().with("id", mId), true);
                }
            });
            mOpenBtn = getChild<LLButton>("detail_open_btn");
            mOpenBtn->setClickedCallback([this](LLUICtrl*, const LLSD&)
            {
                if (gInventory.getCategory(mId))
                {
                    LLPanelMainInventory::newFolderWindow(mId);
                }
            });
            mCopyBtn = getChild<LLButton>("detail_copy_uuid_btn");
            mCopyBtn->setClickedCallback([this](LLUICtrl*, const LLSD&)
            {
                if (mId.notNull())
                {
                    const std::string text = mId.asString();
                    LLClipboard::instance().copyToClipboard(utf8str_to_wstring(text), 0, static_cast<S32>(text.size()));
                }
            });
            refresh();
            return true;
        }

        // The first selected item (null: nothing) and how many more are selected.
        void setSelection(const LLUUID& id, S32 extra)
        {
            if (id == mId && extra == mExtra)
            {
                return;
            }
            mId    = id;
            mExtra = extra;
            refresh();
        }

        void draw() override
        {
            // draw() runs only while the pane is on screen and not minimized: the picture is fetched here,
            // refresh() only remembers which one is wanted (and lets go when the pane is hidden).
            if (mWantId != mPreviewId)
            {
                setPreview(mWantId);
            }
            if (mTexture.notNull())
            {
                // A texture that is not there (missing asset): no picture box, not a box that never fills.
                mShowPreview = !mTexture->isMissingAsset();
                mTexture->addTextureStats(static_cast<F32>(PREVIEW_BOX_W * PREVIEW_BOX_H));
                // Until the picture has a GL image, the picture control shows its placeholder.
                const bool ready = mTexture->hasGLTexture();
                if (ready != mPictureReady)
                {
                    mPictureReady = ready;
                    mPreview->setTexture(ready ? LLPointer<LLTexture>(mTexture.get()) : LLPointer<LLTexture>());
                }
            }
            updateLayout();   // the texture's size is known only after its header arrived
            LLPanel::draw();
        }

        // The pane, its layout panel or the window was shown or hidden (closed, hidden by the setting or
        // a narrow window): a hidden pane holds no picture (a minimized window: see watchMinimize).
        void onVisibilityChange(bool new_visibility) override
        {
            LLPanel::onVisibilityChange(new_visibility);
            refresh();
        }

        // A minimized window keeps its children "visible" (it only does not draw them), so it is
        // watched: the picture is let go while it is minimized, and fetched again by draw() afterwards.
        void watchMinimize(LLFloater* floater)
        {
            if (mWatching || !floater)
            {
                return;
            }
            mWatching = true;
            LLHandle<LLView> handle = getHandle();
            mMinimizeConnection = floater->setMinimizeCallback([handle](LLUICtrl*, const LLSD& minimized)
            {
                if (PlusInvDetailPanel* self = dynamic_cast<PlusInvDetailPanel*>(handle.get()))
                {
                    self->mMinimized = minimized.asBoolean();   // (the signal comes before the floater's own flag changes)
                    self->refresh();
                }
            });
        }

    private:
        // Fit the preview into its box (keeping the aspect) and give the text what is left.
        // Writes only when something changed.
        void updateLayout()
        {
            if (!mText || !mPreview)
            {
                return;
            }
            S32 pw = 0;
            S32 ph = 0;
            const S32 width  = getRect().getWidth();
            const S32 height = getRect().getHeight();
            const S32 box_w  = llclamp(width - 2 * PREVIEW_LEFT, 1, PREVIEW_BOX_W);
            if (mShowPreview)
            {
                pw = box_w;
                ph = PREVIEW_BOX_H;
                // The picture is a thumbnail (shrunk), so the aspect comes from the full size.
                const S32 iw = mTexture.notNull() ? mTexture->getFullWidth() : 0;
                const S32 ih = mTexture.notNull() ? mTexture->getFullHeight() : 0;
                if (iw > 0 && ih > 0)
                {
                    ph = llclamp(box_w * ih / iw, 1, PREVIEW_BOX_H);
                    pw = llclamp(ph * iw / ih, 1, box_w);
                }
            }
            if (pw == mPreviewW && ph == mPreviewH && width == mPaneWidth && height == mPaneHeight
                && mShowPreview == mPreviewVisible)
            {
                return;
            }
            mPreviewW       = pw;
            mPreviewH       = ph;
            mPaneWidth      = width;
            mPaneHeight     = height;
            mPreviewVisible = mShowPreview;
            mPreview->setVisible(mShowPreview);
            S32 text_top = PREVIEW_TOP;
            if (mShowPreview)
            {
                const S32 left = PREVIEW_LEFT + (box_w - pw) / 2;
                mPreview->setShape(LLRect(left, height - PREVIEW_TOP, left + pw, height - PREVIEW_TOP - ph));
                text_top += ph + PREVIEW_GAP;
            }
            const LLRect old = mText->getRect();
            mText->setShape(LLRect(old.mLeft, llmax(height - text_top, old.mBottom + 1), old.mRight, old.mBottom));
        }

        // A name that may still be on its way: the cache answers now, or calls back (the
        // pane is looked up again by its handle, so a closed window is no problem).
        std::string avatarName(const LLUUID& avatar_id)
        {
            if (avatar_id.isNull())
            {
                return getString("unknown");
            }
            LLAvatarName name;
            if (LLAvatarNameCache::get(avatar_id, &name))
            {
                return name.getCompleteName();
            }
            LLHandle<LLView> handle = getHandle();
            const LLUUID shown_id = mId;
            LLAvatarNameCache::get(avatar_id, [handle, shown_id](const LLUUID&, const LLAvatarName&)
            {
                if (PlusInvDetailPanel* self = dynamic_cast<PlusInvDetailPanel*>(handle.get()))
                {
                    if (self->mId == shown_id)
                    {
                        self->refresh();
                    }
                }
            });
            return getString("loading");
        }

        void addLine(std::string& out, const char* label_key, const std::string& value)
        {
            out += getString(label_key);
            out += ": ";
            out += value;
            out += "\n";
        }

        std::string permissionsText(PermissionMask mask)
        {
            const std::string yes = getString("yes");
            const std::string no  = getString("no");
            return getString("perm_modify") + " " + ((mask & PERM_MODIFY) ? yes : no) + "  "
                 + getString("perm_copy") + " " + ((mask & PERM_COPY) ? yes : no) + "  "
                 + getString("perm_transfer") + " " + ((mask & PERM_TRANSFER) ? yes : no);
        }

        std::string acquiredText(time_t time_utc)
        {
            if (time_utc == 0)
            {
                return getString("unknown");
            }
            static LLCachedControl<bool> use_24h(gSavedSettings, "Use24HourClock", true);
            std::string text = getString(use_24h ? "acquired_24h" : "acquired_12h");
            LLSD substitution;
            substitution["datetime"] = static_cast<S32>(time_utc);
            LLStringUtil::format(text, substitution);
            return text;
        }

        // The picture is a thumbnail (BOOST_THUMBNAIL: shrunk, leaves the texture list about 30 seconds
        // after the last reference), never a UI image, which stays in VRAM until the viewer ends.
        // Let go (null) when another item or nothing is shown.
        void setPreview(const LLUUID& texture_id)
        {
            if (texture_id == mPreviewId)
            {
                return;
            }
            mPictureReady = false;
            mTexture      = texture_id.notNull()
                ? LLViewerTextureManager::getFetchedTexture(texture_id, FTT_DEFAULT, true /*use_mipmaps*/, LLGLTexture::BOOST_THUMBNAIL)
                : nullptr;   // (null also before the texture list is up or while the viewer ends: tried again by draw())
            if (mTexture.notNull())
            {
                mTexture->setKnownDrawSize(PREVIEW_BOX_W, PREVIEW_BOX_H);
            }
            mPreviewId   = mTexture.notNull() ? texture_id : LLUUID::null;
            mShowPreview = mTexture.notNull();
            mPreview->setTexture(LLPointer<LLTexture>());   // set again by draw() once it has a GL image
        }

        void refresh()
        {
            if (!mText)
            {
                return;
            }
            std::string body;
            std::string title;
            bool is_item   = false;
            bool is_folder = false;
            LLUUID texture_id;

            if (LLViewerInventoryItem* item = gInventory.getItem(mId))
            {
                is_item = true;
                if (item->getType() == LLAssetType::AT_TEXTURE)
                {
                    texture_id = item->getAssetUUID();
                }
                title   = item->getName();
                const LLPermissions& perm = item->getPermissions();
                const char* type_name = LLAssetType::lookup(item->getType());
                addLine(body, "l_type", type_name ? std::string(type_name) : getString("unknown"));
                // @shownames: a creator the item properties would anonymize is anonymized here too (research/248 E36)
                if (plus::rlv::enabled() && !plus::rlv::canShowItemCreator(item))
                {
                    addLine(body, "l_creator", plus::rlv::anonymizedName(item->getCreatorUUID()));
                }
                else
                {
                    addLine(body, "l_creator", avatarName(item->getCreatorUUID()));
                }
                addLine(body, "l_owner", perm.isGroupOwned() ? getString("group")
                                       : (perm.getOwner() == gAgentID ? getString("you") : avatarName(perm.getOwner())));
                addLine(body, "l_acquired", acquiredText(item->getCreationDate()));
                addLine(body, "l_perm_you", permissionsText(perm.getMaskOwner()));
                addLine(body, "l_perm_next", permissionsText(perm.getMaskNextOwner()));
                const std::string& description = item->getDescription();
                addLine(body, "l_description", description.empty() ? getString("none") : description);
            }
            else if (LLViewerInventoryCategory* category = gInventory.getCategory(mId))
            {
                is_folder = true;
                title     = category->getName();
                addLine(body, "l_type", getString("folder"));
                const S32 count = category->getDescendentCount();
                addLine(body, "l_contents", count == LLViewerInventoryCategory::DESCENDENT_COUNT_UNKNOWN ? getString("unknown") : llformat("%d", count));
            }
            else
            {
                body = getString("nothing");
            }
            if (mId.notNull())
            {
                addLine(body, "l_uuid", mId.asString());
            }
            if (mExtra > 0)
            {
                LLStringUtil::format_map_t args;
                args["[N]"] = llformat("%d", mExtra);
                body += "\n" + getString("more_selected", args);
            }

            // The picture itself is fetched by draw() (on screen only). Here: remember the wish, and let
            // go when nothing is wanted or the pane is hidden or minimized (the setting off, a narrow
            // window, a closed window). The name callbacks call refresh() again for the same item.
            mWantId = texture_id;
            if (texture_id.isNull() || mMinimized || !isInVisibleChain())
            {
                setPreview(LLUUID::null);
            }
            updateLayout();
            mTitle->setText(title);
            mText->setText(body);
            mPropertiesBtn->setEnabled(is_item);
            mOpenBtn->setEnabled(is_folder);
            mCopyBtn->setEnabled(mId.notNull());
        }

        LLTextBox*    mTitle         = nullptr;
        LLTextEditor* mText          = nullptr;
        plus::psui::PsPicture* mPreview = nullptr;
        LLButton*     mPropertiesBtn = nullptr;
        LLButton*     mOpenBtn       = nullptr;
        LLButton*     mCopyBtn       = nullptr;

        LLUUID mId;
        S32    mExtra = 0;

        LLPointer<LLViewerFetchedTexture> mTexture;
        LLUUID mPreviewId;   // the picture held now
        LLUUID mWantId;      // the picture the selected item has
        boost::signals2::scoped_connection mMinimizeConnection;
        bool mWatching       = false;
        bool mMinimized      = false;
        bool mPictureReady   = false;
        bool mShowPreview    = false;
        bool mPreviewVisible = true;   // != mShowPreview at first, so the first layout runs
        S32  mPreviewW       = -1;
        S32  mPreviewH       = -1;
        S32  mPaneWidth      = -1;
        S32  mPaneHeight     = -1;
    };

    LLPanelInjector<PlusInvDetailPanel> sPanelRegistration("plus_inv_detail");

    // The pane of one inventory window, found once and remembered by handle.
    struct Pane
    {
        LLHandle<LLView> panel;
        U32              lastSearch = 0;
        bool             searched   = false;
    };

    std::unordered_map<const LLPanelMainInventory*, Pane> sPanes;

    // The pane next to this inventory view, or null when its window has none.
    PlusInvDetailPanel* paneFor(LLPanelMainInventory* owner)
    {
        Pane& pane = sPanes[owner];
        if (PlusInvDetailPanel* found = dynamic_cast<PlusInvDetailPanel*>(pane.panel.get()))
        {
            return found;
        }
        const U32 frame = LLFrameTimer::getFrameCount();
        if (pane.searched && frame - pane.lastSearch < RESOLVE_RETRY_FRAMES)
        {
            return nullptr;
        }
        pane.searched   = true;
        pane.lastSearch = frame;
        pane.panel      = LLHandle<LLView>();
        if (LLFloater* floater = owner->getParentByType<LLFloater>())
        {
            if (PlusInvDetailPanel* found = floater->findChild<PlusInvDetailPanel>(PANEL_NAME, true))
            {
                pane.panel = found->getHandle();
                found->watchMinimize(floater);
                return found;
            }
        }
        return nullptr;
    }
}

namespace plus
{
namespace invexplorer
{
    void onSelection(LLPanelMainInventory* owner, const std::deque<LLFolderViewItem*>& items)
    {
        PlusInvDetailPanel* pane = paneFor(owner);
        if (!pane)
        {
            return;
        }
        LLUUID id;
        if (!items.empty() && items.front())
        {
            if (const LLFolderViewModelItemInventory* model = dynamic_cast<const LLFolderViewModelItemInventory*>(items.front()->getViewModelItem()))
            {
                id = model->getUUID();
            }
        }
        pane->setSelection(id, id.isNull() ? 0 : static_cast<S32>(items.size()) - 1);
    }

    void tick(LLPanelMainInventory* owner)
    {
        PlusInvDetailPanel* pane = paneFor(owner);
        if (!pane || !pane->getParent())
        {
            return;
        }
        static LLCachedControl<bool> on(gSavedSettings, SETTING_ON, true);
        static LLCachedControl<S32>  min_width(gSavedSettings, SETTING_MIN_WIDTH, 640);
        LLView* layout_panel = pane->getParent();
        LLFloater* floater   = owner->getParentByType<LLFloater>();
        const bool wanted    = on && floater && floater->getRect().getWidth() >= min_width;
        if (layout_panel->getVisible() != wanted)
        {
            layout_panel->setVisible(wanted);
        }
    }
}
}
