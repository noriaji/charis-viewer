/**
 * @file photodetail_bottom.cpp
 * @brief The bottom of the detailed photo screen: the strip of preset cards (research/221, parts 17 and 18).
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

// The strip as the target picture has it: the tabs with the lit line under the chosen one, the
// cards (a picture with rounded corners, the name and a second line under it), the "+" card at
// the right end, the panel's rounded ground. The cards that do not fit are reached by the wheel
// or by the arrows at the strip's ends, which show only while there are cards that way (with all
// the cards in view, as in the picture, there is nothing at the ends). The name to save under is
// asked in a small bubble that the "+" card opens (research/221 part 18).

#include "llviewerprecompiledheaders.h"

#include "photodetail_bottom.h"

#include "photodetail_layout.h"
#include "photodetail_rows.h"
#include "photodetail_state.h"
#include "plus/photomode/photomode.h"
#include "plus/photoscreen/psui/psstack.h"
#include "plus/photoscreen/psui/psstyle.h"
#include "plus/photoscreen/psui/pswidgets.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <set>

#include "lldir.h"
#include "llfile.h"
#include "llfloaterreg.h"
#include "llfontgl.h"
#include "llimage.h"
#include "llimagepng.h"
#include "lllineeditor.h"
#include "lllocalcliprect.h"
#include "llparcel.h"
#include "lltextbox.h"
#include "lltrans.h"
#include "llwindow.h"
#include "lluictrlfactory.h"
#include "lluiimage.h"
#include "llviewerparcelmgr.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llviewerwindow.h"

namespace plus::photoscreen::detail
{
    using namespace plus::photoscreen::sizes;
    namespace ps = plus::psui;

    namespace
    {
        // A card of the strip as collected for the tab chosen.
        struct StripCard
        {
            std::string label, subtitle, tip, thumb;    // thumb: the key of the small picture ("" none)
            const char* icon = "presets";               // drawn when there is no picture
            LLPointer<LLTexture> picture;               // instead of a saved small picture (the parcel's)
            bool        current   = false;
            bool        deletable = false;
            std::function<void()> click, remove;
        };
        std::vector<StripCard> sCards;

        // The small pictures of the cards by key; a key with no file is remembered as such (no disk each time).
        // (a pointer that is never freed: the textures are not to be released by a static destructor after the GL is gone)
        std::map<std::string, LLUIImagePtr>& thumbs()
        {
            static auto* map = new std::map<std::string, LLUIImagePtr>();
            return *map;
        }
        std::set<std::string> sNoThumb;

        // --- the small pictures --------------------------------------------------------------

        std::string thumbPath(const std::string& key)
        {
            return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "plus_photo_thumbs", gDirUtilp->getScrubbedFileName(key) + ".png");
        }

        LLUIImagePtr thumbOf(const std::string& key)
        {
            if (key.empty() || sNoThumb.count(key))
            {
                return nullptr;
            }
            auto found = thumbs().find(key);
            if (found != thumbs().end())
            {
                return found->second;
            }
            const std::string path = thumbPath(key);
            if (!LLFile::isfile(path))
            {
                sNoThumb.insert(key);
                return nullptr;
            }
            LLPointer<LLImageFormatted> png = LLImageFormatted::createFromType(IMG_CODEC_PNG);
            LLPointer<LLImageRaw> raw = new LLImageRaw;
            if (png && png->load(path) && png->decode(raw, 0.f) && raw->getWidth() > 0)
            {
                LLPointer<LLViewerTexture> texture = LLViewerTextureManager::getLocalTexture(raw.get(), false);
                if (texture)
                {
                    LLUIImagePtr image = new LLUIImage("", texture);
                    thumbs()[key] = image;
                    return image;
                }
            }
            sNoThumb.insert(key);
            return nullptr;
        }

        void forgetThumb(const std::string& key)
        {
            thumbs().erase(key);
            sNoThumb.erase(key);
        }

        void deleteThumb(const std::string& key)
        {
            forgetThumb(key);
            LLFile::remove(thumbPath(key));
        }

        // When the small picture of key was saved ("10/30 18:42"), "" without one. For the second line of my
        // presets (research/221 part 18: "マイ＝保存した日時"); read when the cards are collected, not drawn.
        std::string thumbDate(const std::string& key)
        {
            llstat status;
            if (key.empty() || LLFile::stat(thumbPath(key), &status) != 0)
            {
                return std::string();
            }
            const std::time_t when = (std::time_t)status.st_mtime;
            std::tm local {};
#if LL_WINDOWS
            if (localtime_s(&local, &when) != 0)
            {
                return std::string();
            }
#else
            if (!localtime_r(&when, &local))
            {
                return std::string();
            }
#endif
            char text[32];
            return std::strftime(text, sizeof(text), "%m/%d %H:%M", &local) ? std::string(text) : std::string();
        }

        // The picture of the parcel the avatar is on (About Land's snapshot), null without one. The camera
        // positions saved before small pictures existed show it (research/221 part 18; the task: "区画の写真").
        LLPointer<LLTexture> parcelPicture()
        {
            const LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
            if (!parcel || parcel->getSnapshotID().isNull())
            {
                return nullptr;
            }
            return LLViewerTextureManager::getFetchedTexture(parcel->getSnapshotID(), FTT_DEFAULT, true, LLGLTexture::BOOST_UI);
        }

        // A small picture of the world as it is now, for the card of a preset just saved. Once, from an idle
        // callback (as the one-press photo is taken), never from a draw.
        void captureThumb(const std::string& key)
        {
            const LLRect world = sS.world;
            if (world.getWidth() < 8 || world.getHeight() < 8 || !gViewerWindow)
            {
                return;
            }
            const S32 tw = THUMB_W;
            const S32 th = llmax(1, ll_round((F32)tw * world.getHeight() / world.getWidth()));
            LLPointer<LLImageRaw> raw = new LLImageRaw;
            if (!gViewerWindow->rawSnapshot(raw, tw, th, false, false, false, false, false, false, false))
            {
                return;
            }
            LLPointer<LLImageFormatted> png = LLImageFormatted::createFromType(IMG_CODEC_PNG);
            if (!png || !png->encode(raw, 0.f))
            {
                return;
            }
            LLFile::mkdir(gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "plus_photo_thumbs"));
            png->save(thumbPath(key));
            forgetThumb(key);   // read back at the next look at the strip
        }

        // --- the strip that scrolls sideways ---------------------------------------------------

        // Holds the cards side by side (STRIP_STEP apart) and shows the part of them that fits, from mOffset.
        // The wheel moves them; while there are cards out of view on a side, that side shows a soft fade and
        // an arrow (STRIP_EDGE wide) whose click moves a strip's width. Positions change only when the offset,
        // the width or the cards change; draw() only draws.
        class CardStrip : public LLView
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLView::Params>
            {
                Params() {}
            };

        protected:
            explicit CardStrip(const Params& p)
            :   LLView(p),
                mFont(ps::font(ps::style::FONT_HEAD)),
                mLeftArrow(1, (llwchar)0x2039),     // single angle quotation marks: < and >
                mRightArrow(1, (llwchar)0x203A)
            {
            }
            friend class LLUICtrlFactory;

        public:
            // Takes every child away (the cards and the empty note). The offset stays (the same tab made again
            // keeps its place); resetScroll() for another tab.
            void clearCards()
            {
                const LLView::child_list_t old_children = *getChildList();   // die() takes a child out of the list
                for (LLView* child : old_children)
                {
                    child->die();
                }
                mCards.clear();
            }

            void addCard(LLView* card)
            {
                mCards.push_back(card);
                addChild(card);
            }

            // After the cards were added: the offset kept within them, the cards put in their places.
            void finishCards()
            {
                mOffset = llclamp(mOffset, 0, maxOffset());
                place();
            }

            void resetScroll()
            {
                mOffset = 0;
            }

            // Shows the card at index (the keyboard went to it, or the one just saved).
            void scrollTo(size_t index)
            {
                const S32 left  = (S32)index * STRIP_STEP;
                const S32 right = left + CARD_W;
                if (left < mOffset)
                {
                    setOffset(left);
                }
                else if (right > mOffset + getRect().getWidth())
                {
                    setOffset(right - getRect().getWidth());
                }
            }

            void reshape(S32 width, S32 height, bool called_from_parent = true) override
            {
                LLView::reshape(width, height, called_from_parent);
                setOffset(mOffset);
                place();
            }

            bool handleScrollWheel(S32 x, S32 y, S32 clicks) override
            {
                setOffset(mOffset + clicks * STRIP_STEP);
                return true;    // not to the world's camera behind the strip
            }

            bool handleScrollHWheel(S32 x, S32 y, S32 clicks) override
            {
                setOffset(mOffset + clicks * STRIP_STEP);
                return true;
            }

            bool handleMouseDown(S32 x, S32 y, MASK mask) override
            {
                const S32 side = arrowAt(x);
                if (side != 0)
                {
                    setOffset(mOffset + side * pageStep());
                    return true;
                }
                return LLView::handleMouseDown(x, y, mask);
            }

            bool handleDoubleClick(S32 x, S32 y, MASK mask) override
            {
                if (arrowAt(x) != 0)
                {
                    return handleMouseDown(x, y, mask);     // a quick second click moves again
                }
                return LLView::handleDoubleClick(x, y, mask);
            }

            bool handleHover(S32 x, S32 y, MASK mask) override
            {
                mHoverSide = arrowAt(x);
                if (mHoverSide != 0)
                {
                    getWindow()->setCursor(UI_CURSOR_HAND);
                    return true;
                }
                return LLView::handleHover(x, y, mask);
            }

            void onMouseLeave(S32 x, S32 y, MASK mask) override
            {
                mHoverSide = 0;
                LLView::onMouseLeave(x, y, mask);
            }

            void draw() override
            {
                {
                    LLLocalClipRect clip(getLocalRect());
                    LLView::draw();
                }
                const F32 a = getDrawContext().mAlpha;
                if (mOffset > 0)
                {
                    drawEdge(-1, a);
                }
                if (mOffset < maxOffset())
                {
                    drawEdge(1, a);
                }
            }

        private:
            S32 contentWidth() const
            {
                return mCards.empty() ? 0 : (S32)mCards.size() * STRIP_STEP - CARD_GAP;
            }

            S32 maxOffset() const
            {
                return llmax(0, contentWidth() - getRect().getWidth());
            }

            // A strip's width of whole cards (at least one).
            S32 pageStep() const
            {
                return llmax(1, (getRect().getWidth() - 2 * STRIP_EDGE) / STRIP_STEP) * STRIP_STEP;
            }

            // -1 on the left arrow, 1 on the right one, 0 elsewhere (an arrow is there only while cards are out
            // of view on its side).
            S32 arrowAt(S32 x) const
            {
                if (x < STRIP_EDGE && mOffset > 0)
                {
                    return -1;
                }
                if (x >= getRect().getWidth() - STRIP_EDGE && mOffset < maxOffset())
                {
                    return 1;
                }
                return 0;
            }

            void setOffset(S32 offset)
            {
                offset = llclamp(offset, 0, maxOffset());
                if (offset != mOffset)
                {
                    mOffset = offset;
                    place();
                }
            }

            void place()
            {
                const S32 h = getRect().getHeight();
                for (size_t i = 0; i < mCards.size(); ++i)
                {
                    const S32 left = (S32)i * STRIP_STEP - mOffset;
                    const LLRect rect(left, h, left + CARD_W, h - CARD_H);
                    if (mCards[i]->getRect() != rect)
                    {
                        mCards[i]->setRect(rect);
                    }
                }
            }

            // The fade from the panel's colour into the cards and the round arrow on it.
            void drawEdge(S32 side, F32 alpha)
            {
                const F32 w = F32(getRect().getWidth());
                const F32 top = F32(getRect().getHeight());
                const F32 bottom = top - F32(CARD_H);
                const F32 edge = F32(STRIP_EDGE);
                const LLColor4 solid = ps::faded(ps::style::PANEL_MID, alpha);
                const LLColor4 clear(solid.mV[VRED], solid.mV[VGREEN], solid.mV[VBLUE], 0.f);
                const LLRectf band = side < 0 ? LLRectf(0.f, top, edge, bottom) : LLRectf(w - edge, top, w, bottom);
                ps::fillRect(band, side < 0 ? ps::Fill::horizontal(solid, clear) : ps::Fill::horizontal(clear, solid));

                // on the middle of the pictures
                const F32 cx = side < 0 ? edge * 0.5f + 1.f : w - edge * 0.5f - 1.f;
                const F32 cy = top - F32(ps::style::PRESET_PICTURE_HEIGHT) * 0.5f;
                const F32 radius = F32(STRIP_ARROW) * 0.5f;
                const LLColor4 fill = mHoverSide == side ? ps::hovered(ps::style::CHIP_FILL) : ps::style::CHIP_FILL;
                ps::drawShadow(cx, cy, radius, 0.35f * alpha);
                ps::drawCircle(cx, cy, radius, ps::Fill::solid(ps::faded(fill, alpha)));
                ps::drawText(mFont, side < 0 ? mLeftArrow : mRightArrow, cx, cy + 1.f,
                             ps::faded(mHoverSide == side ? ps::style::TEXT_WHITE : ps::style::ICON_LIGHT, alpha),
                             LLFontGL::HCENTER, LLFontGL::VCENTER);
            }

            const LLFontGL*      mFont;
            const LLWString      mLeftArrow, mRightArrow;
            std::vector<LLView*> mCards;        // children, in order (die() in clearCards() before they go)
            S32 mOffset    = 0;
            S32 mHoverSide = 0;
        };

        LLHandle<LLView> sStrip;

        CardStrip* strip()
        {
            return static_cast<CardStrip*>(sStrip.get());
        }

        // --- the cards -----------------------------------------------------------------

        // A word of the strip's panel (its <string>s), "" when it has none.
        std::string word(const std::string& name)
        {
            LLPanel* bottom = sS.bottom.get();
            return bottom && bottom->hasString(name) ? bottom->getString(name) : std::string();
        }

        // The poses folder's last change (a pose saved or removed by the poser while the tab is open), looked at
        // every POSES_CHECK_SECONDS, not every tick: a folder's time changes when a file is added to it or taken
        // from it.
        constexpr F32 POSES_CHECK_SECONDS = 1.5f;

        const std::string& posesFolderStamp()
        {
            static LLFrameTimer since;
            static std::string  stamp;
            if (stamp.empty() || since.getElapsedTimeF32() >= POSES_CHECK_SECONDS)
            {
                since.reset();
                llstat status;
                const std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "poses");
                stamp = LLFile::stat(dir, &status) == 0 ? std::to_string((long long)status.st_mtime) : std::string("-");
            }
            return stamp;
        }

        std::string cardSignature()
        {
            std::string sig = std::to_string((int)sS.ptab) + '|';
            switch (sS.ptab)
            {
                case PtCamera:
                    for (const pm::CameraView& view : pm::namedViews())
                    {
                        sig += view.name + '|';
                    }
                    sig += pm::regionName();
                    break;
                case PtScene:
                    sig += pm::currentPreset();
                    break;
                case PtMy:
                    for (const std::string& name : pm::myPresetNames())
                    {
                        sig += name + '|';
                    }
                    break;
                case PtPose:
                    sig += posesFolderStamp();
                    break;
                default:
                    break;
            }
            return sig;
        }

        // "50 mm  2.3 m": the lens and the distance to what it looks at.
        std::string viewSubtitle(const pm::CameraView& view)
        {
            std::string text;
            if (view.fov > 0.f)
            {
                text = std::to_string((int)std::lround(angleToMm(view.fov))) + " mm";
            }
            const F64 distance = (view.focus - view.position).magVec();
            if (distance > 0.05)
            {
                char metres[24];
                snprintf(metres, sizeof(metres), distance < 10.0 ? "%.1f m" : "%.0f m", distance);
                text += text.empty() ? metres : std::string("  ") + metres;
            }
            return text;
        }

        void collectCards()
        {
            sCards.clear();
            switch (sS.ptab)
            {
                case PtCamera:
                {
                    const std::string region = pm::regionName();
                    const std::vector<pm::CameraView> views = pm::namedViews();
                    LLPointer<LLTexture> parcel;
                    bool parcel_read = false;
                    for (size_t i = 0; i < views.size(); ++i)
                    {
                        StripCard card;
                        card.label     = views[i].name;
                        card.subtitle  = viewSubtitle(views[i]);
                        card.thumb     = "cam_" + region + "_" + views[i].name;
                        card.icon      = "camera";
                        card.deletable = true;
                        if (!thumbOf(card.thumb))
                        {
                            if (!parcel_read)
                            {
                                parcel = parcelPicture();
                                parcel_read = true;
                            }
                            card.picture = parcel;
                        }
                        card.click     = [i]() { pm::goToNamedView(i, true); };
                        const std::string key = card.thumb;
                        card.remove    = [i, key]() {
                            if (pm::deleteNamedView(i))
                            {
                                deleteThumb(key);
                            }
                            relayout();
                        };
                        sCards.push_back(std::move(card));
                    }
                    break;
                }
                case PtPose:
                {
                    const std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "poses");
                    const std::string sub = word("sub_pose");
                    for (const std::string& file : gDirUtilp->getFilesInDir(dir))
                    {
                        if (file.size() > 4 && file.compare(file.size() - 4, 4, ".xml") == 0)
                        {
                            StripCard card;
                            card.label    = file.substr(0, file.size() - 4);
                            card.subtitle = sub;
                            card.tip      = str("OpenPoser");
                            card.icon     = "pose";
                            card.click    = []() { LLFloaterReg::showInstance("fs_poser"); };
                            sCards.push_back(std::move(card));
                        }
                    }
                    break;
                }
                case PtScene:
                {
                    const std::string now = pm::currentPreset();
                    for (const pm::Preset& preset : pm::presets())
                    {
                        StripCard card;
                        // the name: the strip's own word, else the shared one of strings_plus.xml (the first six)
                        std::string name = word("name_" + preset.id);
                        if (name.empty() && !LLTrans::findString(name, "PhotoScreenDetail_Preset_" + preset.id))
                        {
                            name = preset.id;
                        }
                        card.label    = name;
                        card.subtitle = word("sub_" + preset.id);
                        card.tip      = word("tip_" + preset.id); // "" shows the name
                        card.icon     = "scene";
                        card.current  = preset.id == now;
                        const std::string id = preset.id;
                        card.click    = [id]() { pm::applyPreset(id); };
                        sCards.push_back(std::move(card));
                    }
                    break;
                }
                default:
                    for (const std::string& name : pm::myPresetNames())
                    {
                        StripCard card;
                        card.label     = name;
                        card.thumb     = "my_" + name;
                        card.subtitle  = thumbDate(card.thumb);
                        card.icon      = "presets";
                        card.deletable = true;
                        card.click     = [name]() { pm::applyMyPreset(name); };
                        const std::string key = card.thumb;
                        card.remove    = [name, key]() {
                            if (pm::deleteMyPreset(name))
                            {
                                deleteThumb(key);
                            }
                            relayout();
                        };
                        sCards.push_back(std::move(card));
                    }
                    break;
            }
        }

        // The "+" card and the name bubble for the tab chosen.
        void showAddCard(LLPanel* bottom)
        {
            ps::PsAddCard* add = bottom->getChild<ps::PsAddCard>("btn_save");
            const bool pose = sS.ptab == PtPose;
            add->setLabelText(word(pose ? "add_pose" : "add_save"));
            const std::string tip = word(pose ? "add_tip_pose" : sS.ptab == PtCamera ? "add_tip_camera" : "add_tip_my");
            if (add->getToolTip() != tip)
            {
                add->setToolTip(tip);
            }
            if (pose)
            {
                bottom->getChildView("name_bubble")->setVisible(false);
            }
        }

        // The cards of the tab chosen, made again when the tab or what they show changes.
        void showCards()
        {
            LLPanel* bottom = sS.bottom.get();
            CardStrip* cards = strip();
            if (!bottom || !cards)
            {
                return;
            }
            for (int i = 0; i < PtCount; ++i)
            {
                bottom->getChild<LLUICtrl>(std::string("ptab_") + PTAB_IDS[i])->setValue(i == sS.ptab);   // same value: nothing
            }
            showAddCard(bottom);
            cards->clearCards();

            const LLRect room = cards->getLocalRect();
            if (sCards.empty())
            {
                // say what this tab is for (the "+" card at the right does it)
                const std::string text = sS.ptab == PtCamera ? str("EmptyCamera") : sS.ptab == PtPose ? str("EmptyPose") : str("EmptyMy");
                if (LLTextBox* note = ps::makeNote("empty",
                                                   LLRect(STRIP_EDGE, room.mTop - 24, room.getWidth() - STRIP_EDGE, room.mTop - 24 - 60),
                                                   text, ps::font(ps::style::FONT_BODY), ps::style::TEXT_DIM,
                                                   FOLLOWS_LEFT | FOLLOWS_RIGHT | FOLLOWS_TOP))   // the strip's width, as it is resized
                {
                    cards->addChild(note);
                }
                cards->finishCards();
                return;
            }
            for (const StripCard& card : sCards)
            {
                ps::PsPresetCard::Params params;
                params.name          = "card";
                params.rect          = LLRect(0, room.mTop, CARD_W, room.mTop - CARD_H);
                params.label         = card.label;
                params.subtitle      = card.subtitle;
                params.icon          = card.icon;
                params.deletable     = card.deletable && (bool)card.remove;
                params.initial_value = card.current;
                if (ps::PsPresetCard* view = LLUICtrlFactory::create<ps::PsPresetCard>(params))
                {
                    view->setToolTip(card.tip.empty() ? card.label : card.tip);
                    if (LLUIImagePtr thumb = thumbOf(card.thumb))
                    {
                        view->setTexture(thumb->getImage());
                    }
                    else if (card.picture)
                    {
                        view->setTexture(card.picture);
                    }
                    if (card.click)
                    {
                        const auto click = card.click;
                        view->setCommitCallback([click](LLUICtrl*, const LLSD&) { later(click); });
                    }
                    if (card.remove)
                    {
                        const auto remove = card.remove;
                        view->setDeleteCallback([remove](ps::PsPresetCard*) { later(remove); });
                    }
                    cards->addCard(view);
                }
            }
            cards->finishCards();
        }

        std::string nextMyName()
        {
            const std::vector<std::string> names = pm::myPresetNames();
            for (int n = (int)names.size() + 1;; ++n)
            {
                LLStringUtil::format_map_t args;
                args["[N]"] = std::to_string(n);
                const std::string name = str("NameMy", args);
                if (std::find(names.begin(), names.end(), name) == names.end())
                {
                    return name;
                }
            }
        }

        void saveCurrent()
        {
            LLPanel* bottom = sS.bottom.get();
            if (!bottom)
            {
                return;
            }
            LLLineEditor* editor = bottom->getChild<LLLineEditor>("preset_name");
            std::string name = editor->getText();
            LLStringUtil::trim(name);
            if (sS.ptab == PtPose)
            {
                LLFloaterReg::showInstance("fs_poser");
                return;
            }
            std::string key;
            if (sS.ptab == PtCamera)
            {
                if (!pm::saveNamedView(name))
                {
                    return;
                }
                const std::vector<pm::CameraView> views = pm::namedViews();
                if (!views.empty())
                {
                    key = "cam_" + pm::regionName() + "_" + views.back().name;
                }
            }
            else
            {
                if (name.empty())
                {
                    name = nextMyName();
                }
                if (!pm::saveMyPreset(name))
                {
                    return;
                }
                key = "my_" + name;
                sS.ptab = PtMy;
            }
            editor->setText(std::string());
            bottom->getChildView("name_bubble")->setVisible(false);
            rebuildCards();
            if (CardStrip* cards = strip(); cards && !sCards.empty())
            {
                cards->scrollTo(sCards.size() - 1);     // the new card is the last one
            }
            if (!key.empty())
            {
                later([key]() {
                    captureThumb(key);
                    rebuildCards();
                    if (CardStrip* cards = strip(); cards && !sCards.empty())
                    {
                        cards->scrollTo(sCards.size() - 1);
                    }
                });
            }
        }

        // The "+" card: the poser for the poses, the name bubble (opened, or closed again) for the others.
        void onAddCard()
        {
            LLPanel* bottom = sS.bottom.get();
            if (!bottom)
            {
                return;
            }
            if (sS.ptab == PtPose)
            {
                LLFloaterReg::showInstance("fs_poser");
                return;
            }
            LLView* bubble = bottom->getChildView("name_bubble");
            const bool open = !bubble->getVisible();
            bubble->setVisible(open);
            if (open)
            {
                LLLineEditor* editor = bottom->getChild<LLLineEditor>("preset_name");
                editor->setText(std::string());
                editor->setFocus(true);
            }
        }

        void selectPresetTab(PresetTab tab)
        {
            if (LLPanel* bottom = sS.bottom.get(); bottom && tab != sS.ptab)
            {
                bottom->getChildView("name_bubble")->setVisible(false);
            }
            if (CardStrip* cards = strip(); cards && tab != sS.ptab)
            {
                cards->resetScroll();
            }
            sS.ptab = tab;
            rebuildCards();
        }
    }

    void rebuildCards()
    {
        sS.cardsSig = cardSignature();
        collectCards();
        showCards();
    }

    void bindBottomPanel()
    {
        LLPanel* bottom = sS.bottom.get();
        for (int i = 0; i < PtCount; ++i)
        {
            const PresetTab tab = (PresetTab)i;
            bottom->getChild<LLUICtrl>(std::string("ptab_") + PTAB_IDS[i])->setCommitCallback([tab](LLUICtrl*, const LLSD&) { selectPresetTab(tab); });
        }
        bottom->getChild<LLUICtrl>("btn_save")->setCommitCallback([](LLUICtrl*, const LLSD&) { onAddCard(); });
        bottom->getChild<LLUICtrl>("btn_save_ok")->setCommitCallback([](LLUICtrl*, const LLSD&) { saveCurrent(); });
        LLLineEditor* editor = bottom->getChild<LLLineEditor>("preset_name");
        editor->setCommitCallback([](LLUICtrl*, const LLSD&) { saveCurrent(); });   // Enter (the bubble's panel commits it)
        for (LLView* child : *editor->getChildList())
        {
            if (dynamic_cast<LLViewBorder*>(child))
            {
                child->setVisible(false);   // the box is the dark value box of the style (research/221 part 5), no bevel
            }
        }

        // the strip of cards fills the place the XML keeps for it
        LLView* area = bottom->getChildView("cards");
        CardStrip::Params params;
        params.name         = "strip";
        params.rect         = area->getLocalRect();
        params.follows.flags(FOLLOWS_ALL);
        params.mouse_opaque = true;
        if (CardStrip* cards = LLUICtrlFactory::create<CardStrip>(params))
        {
            area->addChild(cards);
            sStrip = cards->getHandle();
        }
    }

    void layoutBottom(const Margins& m)
    {
        // Between the side panels, under the world, with the panels' gap above and below (research/221: the
        // strip is as wide as the room the world has).
        if (LLPanel* bottom = sS.bottom.get())
        {
            const S32 gap   = ps::style::PANEL_GAP;
            const S32 left  = m.rail + m.left_panel;
            const S32 right = llmax(left + 1, sS.w - m.right_panel);
            const S32 top   = llmax(gap + 1, m.bottom - gap);
            bottom->setShape(LLRect(left, top, right, gap));
        }
    }

    void tickBottom()
    {
        const std::string sig = cardSignature();
        if (sig != sS.cardsSig)
        {
            rebuildCards();
        }
    }
}
