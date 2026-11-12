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

// research/220, research/318 7. The detail pane (panel_plus_inv_detail.xml) is made here the first time a
// window is wide enough for it (ensurePane, asked by invlayout.cpp), as a child of the inventory view
// (LLPanelMainInventory), and invlayout.cpp places it on the right of the tab's contents, between the tab row
// and the received items. This file has the pane and the two hooks of LLPanelMainInventory: onSelection() tells
// the pane what is selected, tick() lets invlayout show it and the three columns of a wider window (invcolumns.cpp
// has what they mean). The pane is a read-only view: it changes nothing in the inventory.

#include "llviewerprecompiledheaders.h"

#include "invexplorer.h"
#include "invcolumns.h"
#include "invlayout.h"

#include <unordered_map>

#include "llagent.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llbutton.h"
#include "llclipboard.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfolderview.h"
#include "llfolderviewitem.h"
#include "llfolderviewmodelinventory.h"
#include "llgltexture.h"
#include "llinventorymodel.h"
#include "llinventoryobserver.h"
#include "llinventorypanel.h"
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
    constexpr char PANE_FILE[] = "panel_plus_inv_detail.xml";

    // The pane's layout (panel_plus_inv_detail.xml): the title above PANE_TOP_PX, the buttons below PANE_BOTTOM_PX;
    // between them the picture preview of a texture (left 8; as wide as the pane allows, 244 at most) and the text.
    // A low pane drops the picture first, then the text (research/318 7.3); plus::invexplorer::PANE_MIN_HEIGHT_PX
    // is the title and the buttons only.
    constexpr S32 PREVIEW_BOX_W  = 244;
    constexpr S32 PREVIEW_BOX_H  = 150;
    constexpr S32 PREVIEW_LEFT   = 8;
    constexpr S32 PANE_TOP_PX    = 30;
    constexpr S32 PANE_BOTTOM_PX = 44;
    constexpr S32 PREVIEW_GAP    = 4;
    constexpr S32 PICTURE_MIN_PX = 40;
    constexpr S32 TEXT_MIN_PX    = 40;
    constexpr S32 BUTTONS_PX     = 26;   // the buttons' row, from the bottom (4 + 22)
    static_assert(plus::invexplorer::PANE_MIN_HEIGHT_PX >= PANE_TOP_PX + BUTTONS_PX,
                  "the lowest pane shown has room for its title and its buttons");

    class PlusInvDetailPanel : public LLPanel
    {
    public:
        PlusInvDetailPanel() : mThumbObserver(*this)
        {
            gInventory.addObserver(&mThumbObserver);
        }

        ~PlusInvDetailPanel() override
        {
            // The views are destroyed before cleanupInventory (shutdownViews comes first). The observer is a member:
            // left in the model, cleanupInventory would delete it.
            gInventory.removeObserver(&mThumbObserver);
        }

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
            return true;   // (built hidden: showing it builds its text, onVisibilityChange)
        }

        // The first selected item (null: nothing) and how many more are selected. A hidden pane only remembers it:
        // its text is built when it shows (onVisibilityChange).
        void setSelection(const LLUUID& id, S32 extra)
        {
            if (id == mId && extra == mExtra)
            {
                return;
            }
            mId    = id;
            mExtra = extra;
            refreshIfShown();
        }

        void draw() override
        {
            // draw() runs only while the pane is on screen and not minimized: the picture is fetched here,
            // refresh() only remembers which one is wanted (and lets go when the window is closed or minimized).
            if (mWantId != mPreviewId)
            {
                setPreview(mWantId);
            }
            if (mTexture.notNull())
            {
                // A texture that is not there (missing asset): no picture box, not a box that never fills.
                mShowPreview = !mTexture->isMissingAsset();
                if (mPreviewVisible)   // (not in a pane too low for the picture, updateLayout)
                {
                    mTexture->addTextureStats(static_cast<F32>(PREVIEW_BOX_W * PREVIEW_BOX_H));
                }
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

        // The pane was shown or hidden (by the setting, its button or the window's width, invlayout.cpp). Shown: its
        // text is built for the selection it was given just before (plus::invexplorer::syncPane). Hidden: it only
        // lets go of its picture; its text is not built again while it stays hidden (research/318 8.3).
        void onVisibilityChange(bool new_visibility) override
        {
            LLPanel::onVisibilityChange(new_visibility);
            if (new_visibility)
            {
                refresh();
            }
            else if (mPreview)   // (null only before postBuild)
            {
                setPreview(LLUUID::null);
            }
        }

        // The window is watched: the pane is a child of the inventory view, which does not pass the window's
        // visibility on to its children (LLPanelMainInventory::onVisibilityChange does not call its base), and a
        // minimized window keeps its children "visible" (it only does not draw them). The picture is let go while
        // the window is closed (reuse_instance: hidden) or minimized, and fetched again by draw() afterwards. A
        // hidden pane holds no picture already: it only notes the minimized state (it shows with refresh()).
        void watchWindow(LLFloater* floater)
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
                    self->refreshIfShown();
                }
            });
            // (after the window's own flag changed: refresh() sees the pane out of the visible chain)
            mVisibleConnection = floater->setVisibleCallback([handle](LLUICtrl*, const LLSD&)
            {
                if (PlusInvDetailPanel* self = dynamic_cast<PlusInvDetailPanel*>(handle.get()))
                {
                    self->refreshIfShown();
                }
            });
        }

    private:
        // The inventory changed: a shown folder whose picture was replaced or removed is built again. The model tells
        // when something changed (no question every frame); while the pane is hidden nothing is looked at (refresh()
        // runs when it shows). One lookup of the selected id per change.
        class ThumbObserver : public LLInventoryObserver
        {
        public:
            explicit ThumbObserver(PlusInvDetailPanel& owner) : mOwner(owner) {}
            void changed(U32) override
            {
                if (mOwner.getVisible() && mOwner.mId.notNull())
                {
                    const LLViewerInventoryCategory* category = gInventory.getCategory(mOwner.mId);
                    if (category && category->getThumbnailUUID() != mOwner.mWantId)
                    {
                        mOwner.refresh();
                    }
                }
            }
        private:
            PlusInvDetailPanel& mOwner;
        };

        // Fit the preview into its box (keeping the aspect) and give the text what is left. A low pane (the window's
        // height, the received items opened) drops the picture first, then the text: the title and the buttons stay.
        // Writes only when something changed.
        void updateLayout()
        {
            if (!mText || !mPreview)
            {
                return;
            }
            const S32 width  = getRect().getWidth();
            const S32 height = getRect().getHeight();
            const bool text_shown    = height >= PANE_TOP_PX + TEXT_MIN_PX + PANE_BOTTOM_PX;
            const S32  box_h         = llmin(PREVIEW_BOX_H, height - (PANE_TOP_PX + PREVIEW_GAP + TEXT_MIN_PX + PANE_BOTTOM_PX));
            const bool picture_shown = mShowPreview && box_h >= PICTURE_MIN_PX;
            const S32  box_w         = llclamp(width - 2 * PREVIEW_LEFT, 1, PREVIEW_BOX_W);
            S32 pw = 0;
            S32 ph = 0;
            if (picture_shown)
            {
                pw = box_w;
                ph = box_h;
                // The picture is a thumbnail (shrunk), so the aspect comes from the full size.
                const S32 iw = mTexture.notNull() ? mTexture->getFullWidth() : 0;
                const S32 ih = mTexture.notNull() ? mTexture->getFullHeight() : 0;
                if (iw > 0 && ih > 0)
                {
                    ph = llclamp(box_w * ih / iw, 1, box_h);
                    pw = llclamp(ph * iw / ih, 1, box_w);
                }
            }
            if (pw == mPreviewW && ph == mPreviewH && width == mPaneWidth && height == mPaneHeight
                && picture_shown == mPreviewVisible && text_shown == mTextVisible)
            {
                return;
            }
            mPreviewW       = pw;
            mPreviewH       = ph;
            mPaneWidth      = width;
            mPaneHeight     = height;
            mPreviewVisible = picture_shown;
            mTextVisible    = text_shown;
            mPreview->setVisible(picture_shown);
            mText->setVisible(text_shown);
            S32 text_top = PANE_TOP_PX;
            if (picture_shown)
            {
                const S32 left = PREVIEW_LEFT + (box_w - pw) / 2;
                mPreview->setShape(LLRect(left, height - PANE_TOP_PX, left + pw, height - PANE_TOP_PX - ph));
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
                PlusInvDetailPanel* self = dynamic_cast<PlusInvDetailPanel*>(handle.get());
                if (self && self->mId == shown_id)
                {
                    self->refreshIfShown();
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

        // The picture is a thumbnail (BOOST_THUMBNAIL: leaves the texture list about 30 seconds after
        // the last reference), never a UI image, which stays in VRAM until the viewer ends. It is not
        // shrunk to the thumbnail size: the draw size given below is MAX_IMAGE_SIZE (as LLThumbnailCtrl
        // gives), so the image of a texture item is not scaled down as it comes in. It is a texture of
        // its own (TEX_LIST_SCALE in the texture list), apart from the one the world draws with the
        // same id (TEX_LIST_STANDARD): only the fetch is shared, and while it is shown there is one
        // more image at the item's own size.
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
                // Not shrunk to the box: a draw size that is not a power of two (244x150) makes preCreateTexture
                // fail ("Non power of two" -> missing) and drops the fetch shared with the preview floater. Same as LLThumbnailCtrl.
                mTexture->setKnownDrawSize(MAX_IMAGE_SIZE, MAX_IMAGE_SIZE);
            }
            mPreviewId   = mTexture.notNull() ? texture_id : LLUUID::null;
            mShowPreview = mTexture.notNull();
            mPreview->setTexture(LLPointer<LLTexture>());   // set again by draw() once it has a GL image
        }

        // A hidden pane is not built: it is built when it shows (onVisibilityChange), for the selection it is given
        // then (plus::invexplorer::syncPane).
        void refreshIfShown()
        {
            if (getVisible())
            {
                refresh();
            }
        }

        // Builds the title, the text and the buttons for the selection: while the pane is shown (refreshIfShown)
        // and when it shows (onVisibilityChange).
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
                // A folder with a picture registered (the inventory thumbnail) shows it like a texture's preview.
                texture_id = category->getThumbnailUUID();
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
            // go when nothing is wanted or the window is closed or minimized (a hidden pane is not built:
            // it let go when it hid). The name callbacks build it again for the same item.
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
        LLUUID mWantId;      // the picture the selected item or folder has (a texture's asset, a folder's thumbnail)
        ThumbObserver mThumbObserver;
        boost::signals2::scoped_connection mMinimizeConnection;
        boost::signals2::scoped_connection mVisibleConnection;
        bool mWatching       = false;
        bool mMinimized      = false;
        bool mPictureReady   = false;
        bool mShowPreview    = false;
        bool mPreviewVisible = false;  // the picture is shown (the first layout runs: mPaneHeight is -1)
        bool mTextVisible    = true;
        S32  mPreviewW       = -1;
        S32  mPreviewH       = -1;
        S32  mPaneWidth      = -1;
        S32  mPaneHeight     = -1;
    };

    // The pane of each inventory view (ensurePane), by handle.
    std::unordered_map<const LLPanelMainInventory*, LLHandle<LLView>> sPanes;

    // The pane of this inventory view, or null while it has none (not made yet, or it could not be).
    PlusInvDetailPanel* paneFor(const LLPanelMainInventory* owner)
    {
        auto it = sPanes.find(owner);
        return it == sPanes.end() ? nullptr : dynamic_cast<PlusInvDetailPanel*>(it->second.get());
    }

    // The pane is given the first of these and how many more there are.
    void giveSelection(PlusInvDetailPanel& pane, const std::deque<LLFolderViewItem*>& items)
    {
        const LLUUID id = items.empty() ? LLUUID::null : plus::invexplorer::idOf(items.front());
        pane.setSelection(id, id.isNull() ? 0 : static_cast<S32>(items.size()) - 1);
    }
}

namespace plus
{
namespace invexplorer
{
    void onSelection(LLPanelMainInventory* owner, LLInventoryPanel* panel, const std::deque<LLFolderViewItem*>& items)
    {
        plus::invcolumns::onSelection(owner, panel, items);
        // In three columns, while the last choice is the center's, the detail shows the center's selection, not what
        // this notification brings: the left only gave the item the columns came with way to its folder (invcolumns
        // keeps the center's selection then), or it is the notification of another tab, one not shown. (A choice on
        // the left takes the center's down first, invcolumns::onSelection.)
        const std::deque<LLFolderViewItem*>* center_items = plus::invcolumns::centerSelection(owner);
        showInDetail(owner, center_items ? *center_items : items);
    }

    LLUUID idOf(const LLFolderViewItem* view)
    {
        const LLFolderViewModelItemInventory* model =
            view ? dynamic_cast<const LLFolderViewModelItemInventory*>(view->getViewModelItem()) : nullptr;
        return model ? model->getUUID() : LLUUID::null;
    }

    void showInDetail(LLPanelMainInventory* owner, const std::deque<LLFolderViewItem*>& items)
    {
        // A hidden pane (the setting or its button off, a narrow or low window) is not told: no text, no names
        // asked for. It reads the selection again when it shows (syncPane).
        PlusInvDetailPanel* pane = paneFor(owner);
        if (pane && pane->getVisible())
        {
            giveSelection(*pane, items);
        }
    }

    LLPanel* ensurePane(LLPanelMainInventory* owner, LLFloater* floater)
    {
        if (PlusInvDetailPanel* found = paneFor(owner))
        {
            return found;
        }
        PlusInvDetailPanel* pane = new PlusInvDetailPanel();
        if (!pane->buildFromFile(PANE_FILE))   // (hidden: the root's visible="false")
        {
            delete pane;
            return nullptr;
        }
        owner->addChild(pane);   // in front of the contents
        sPanes[owner] = pane->getHandle();
        pane->watchWindow(floater);
        return pane;   // (it gets the selection when it shows, syncPane)
    }

    void syncPane(LLPanelMainInventory* owner)
    {
        PlusInvDetailPanel* pane = paneFor(owner);
        if (!pane)
        {
            return;
        }
        // What is selected now, read without choosing anything (chosenCenter would note a choice and clear the
        // received items): the center's selection when it was chosen last, else the active view's (in the official
        // single-folder view, its list).
        static const std::deque<LLFolderViewItem*> nothing;
        const std::deque<LLFolderViewItem*>* items = plus::invcolumns::centerSelection(owner);
        if (!items)
        {
            LLInventoryPanel* active = owner->getActivePanel();
            LLFolderView* root = active ? active->getRootFolder() : nullptr;
            items = root ? &root->getSelectedItems() : &nothing;
        }
        giveSelection(*pane, *items);
    }

    bool paneHasFocus(const LLPanelMainInventory* owner)
    {
        const PlusInvDetailPanel* pane = paneFor(owner);
        return pane && gFocusMgr.childHasKeyboardFocus(pane);
    }

    void tick(LLPanelMainInventory* owner)
    {
        plus::invlayout::tick(owner);
    }

    void forget(const LLPanelMainInventory* owner)
    {
        sPanes.erase(owner);
        plus::invlayout::forget(owner);
        plus::invcolumns::forget(owner);
    }
}
}
