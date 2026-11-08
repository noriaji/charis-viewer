/**
 * @file llgraywire.h
 * @brief Draw a batch as a wireframe while its texture is not loaded yet, instead of the gray stand-in.
 *
 * An avatar is drawn as a wireframe as a whole while any texture of its
 * attachments is not loaded yet (no half dressed avatar for a long time).
 * Both give up (GIVE_UP_SECONDS when nothing fetches the texture,
 * FETCHED_GIVE_UP_SECONDS while it is fetched), so a texture that never comes
 * (no permission, no asset) is not shown as a wireframe forever.
 *
 * research/256 (the materialize band, plus/materialize): with PlusMaterialize
 * the wireframe takes a Tron look, and when the texture of a surface shown as
 * a wireframe comes, a band of light sweeps it from the bottom up for
 * PlusMaterializeSeconds: the wireframe above the band, the real surface
 * below. The batch is drawn twice meanwhile (drawBatch), and the shaders
 * clip each pass by the band (materialize_plane / materialize_params /
 * materialize_color). Not alpha, not in the shadow, reflection, cube
 * snapshot and HUD passes.
 *
 * research/259 (PlusMaterializeAvatar): an avatar drawn as a wireframe as a
 * whole (updateAvatar) is swept by one band from its feet to the top of its
 * head when it is not any more: its body (drawAvatar) and its attachments
 * (their batches, BAND_WORN) take the same plane, worked out every frame
 * from its joints, in the world's up. At most
 * plus::materialize::MAX_AVATAR_BANDS at once.
 *
 * research/277: such an avatar stays a wireframe while its attachments are
 * still coming (at most 15 s), so that its band takes its body and all of
 * them; a worn batch made during the band joins it (held on the GL path
 * at once), and an attached animesh is its wearer's.
 *
 * research/281: an avatar seen whole once (its wireframe as a whole over, or
 * whole without one) is never a wireframe as a whole again until it is made
 * again (a teleport, a relog, shown again): not for a change of outfit (its
 * body keeps its old bakes until the new ones load), a new attachment, a
 * texture put on or a level of detail. What is not loaded is a wireframe on
 * its own batch, and flashes in its own band when it comes. A face that is
 * a point (an empty submesh at a level of detail) does not count, and no
 * wireframe is taken into an impostor.
 *
 * research/278: the bands (a texture's, an avatar's) and their holds go by
 * their own clock (plus::materialize::bandClock: at most twice the recent
 * frame interval, or 50 ms, a frame, so that a hitch does not use one up
 * unseen), and so does whether a wait went stale or its wireframe was seen
 * lately (a second); the waits' own lengths (the grace, the give ups) go by
 * the frame time. The login curtain's fade out is out of the curtain.
 * PlusMaterializeLog counts what the bands did (plus/materialize), and
 * (research/289) the batches drawn as the gray stand-in for good (a wait
 * given up, an asset missing).
 */
#ifndef LL_LLGRAYWIRE_H
#define LL_LLGRAYWIRE_H

#include "llrhigl.h"

class LLDrawInfo;
class LLFace;
class LLGLSLShader;
class LLTexture;
class LLUUID;
class LLViewerFetchedTexture;
class LLViewerTexture;
class LLVOAvatar;

namespace GrayWire
{
    // Without PlusMaterialize, a texture without a GL image this long is shown
    // as a wireframe; it keeps a short grace time for a texture that is
    // recreated a frame later. research/276: with it, by the texture's state
    // instead: a wireframe from the first frame of its wait, unless its image
    // is about to come (looked up in the fast cache, being decoded, decoded
    // and waiting for its creation: the gray stand-in until it comes, for at
    // most 0.1 s, or 4 frames if they take longer, and never past
    // GRACE_SECONDS), and once drawn as one, one until its image comes.
    // research/243: it gives up after GIVE_UP_SECONDS only while nothing
    // fetches it (a busy region's textures wait longer than that in the
    // fetch queue); a fetched one gives up after FETCHED_GIVE_UP_SECONDS.
    constexpr F32 GRACE_SECONDS = 0.25f;
    constexpr F32 GIVE_UP_SECONDS = 120.f;   // was 30 s: a busy shopping event's fetch queue is longer than that
    constexpr F32 FETCHED_GIVE_UP_SECONDS = 300.f;
    // research/276: a texture whose image comes in under this, from the start
    // of its wait, takes no band (it was a wireframe for a moment, if at all:
    // the textures that take one are those of the time grace).
    constexpr F32 BAND_AFTER_SECONDS = GRACE_SECONDS;

    // The setting (RenderGrayTextureWireframe), without a name lookup per call.
    bool enabled();

    // research/289 (pre-merge review L3): whether a wireframe is drawn in the pass now (a Scope draws one
    // there and asks isGray): not the shadow, reflection, cube snapshot (water mirror, probes) and, with the
    // avatar band on, impostor passes, nor under the whole-scene wireframe. Main thread, inside the passes.
    bool wirePassNow();

    // Whether texture is gray for good reason: no GL image (past the grace,
    // above), not for longer than the give up times, and not known to be
    // missing. Updates the texture's timers: call it where the texture is drawn
    // as a wireframe if it is gray (research/289: a pass where wirePassNow; in
    // the others its wait would go on unseen, the Scope asks nothing there).
    // research/276: for one without a GL image it also notes whether its wait
    // was given up (mGrayWireGivenUp, main thread), which plus/materiallist
    // reads (such a texture's batches may be list draws again).
    bool isGray(LLViewerTexture* texture);

    // The avatars drawn as a wireframe as a whole (kept by LLVOAvatar::idleUpdate):
    // an attachment's texture is gray, or (PlusAvatarWireframeLoading,
    // research/243) a body bake is being fetched (up to
    // FETCHED_GIVE_UP_SECONDS), or the agent's own avatar is still loading
    // (drawn then by drawsLoading).
    // research/259: and when an avatar stops being one (it was drawn as a
    // wireframe, not behind the login curtain), its band starts (under
    // plus::materialize::MAX_AVATAR_BANDS); every frame of it, its heights.
    // research/277: one stays a wireframe while its attachments are still
    // coming (to attach, their meshes, shapes and PBR materials; another's
    // as many as the region lists, the agent's requests answered), at most
    // 15 s, so that its body and attachments come in one band; while its
    // band sweeps it, an attachment's gray texture does not take it back.
    // research/281 (with PlusMaterializeAvatar): once seen whole, never again
    // (above), until forgetAvatar.
    void updateAvatar(LLVOAvatar* avatar);
    void forgetAvatar(const LLVOAvatar* avatar);
    bool avatarIsGray(const LLVOAvatar* avatar);

    // research/259: whether the avatar's band sweeps it now.
    bool avatarInBand(const LLVOAvatar* avatar);

    // Whether the avatar's body and attachments are drawn apart from the fast
    // paths (the scene's worn groups stay uncut): a wireframe, or its band.
    bool avatarDrawnApart(const LLVOAvatar* avatar);

    // research/243: whether a loading avatar the official rules hide (the
    // agent's own, LLVOAvatar::isFullyLoaded ignores RenderUnloadedAvatar for
    // it) is drawn: as a wireframe (PlusAvatarWireframeLoading and the
    // setting on), where a Scope draws one (not the shadow, reflection and
    // cube snapshot passes: the water mirror, avatar reflections, probes).
    // Asked where isFullyLoaded() is false; any thread.
    bool drawsLoading(const LLVOAvatar* avatar);

    // research/281 review H1 (LLVOAvatar::getBakedTexture, the bake its
    // attachments' BOM faces take for the bake `index`; main thread): for a
    // materialized avatar (PlusMaterializeAvatar), while a new bake loads
    // (current, not the one its body uses yet: last_used, mLastTextureID), the
    // old one when it is loaded, as its system body keeps it until
    // LLVOAvatar::useBakedTexture (a new bake still coming: at most 60 s seen,
    // from the avatar's latest change of a bake; one that failed: for good,
    // as the body); else current. onBakeUsed
    // (useBakedTexture's end, `id` the bake used): when it is a new bake whose
    // old one was kept, the BOM faces take the bakes again.
    LLViewerFetchedTexture* bakeShown(LLVOAvatar* avatar, U8 index, LLViewerFetchedTexture* current, const LLUUID& last_used);
    void onBakeUsed(LLVOAvatar* avatar, const LLUUID& id);

    // The look of the wireframe: a cyan that pulses (a texture unit 0 binds in its place while a Scope is on);
    // research/256: PlusMaterialize's colour, in a Tron look.
    // init: its textures, made with the texture manager's own (LLViewerTextureManager::init).
    void init();
    void cleanup();
    // Once a frame where textures are made (LLViewerTexture::updateClass: no render pass is open there):
    // research/278: the bands' clock (plus::materialize::tick) and PlusMaterializeLog's line; research/256: the
    // look's textures again when its colour or PlusMaterialize changed (at most every few frames: review L6).
    void updateClass();

    // Whether a Scope drew a wireframe in the main view's scene (not the HUD)
    // within the last few frames: TAA's jitter waits meanwhile (plus/taa,
    // select-flicker), for lines thinner than a pixel moved by under one each
    // frame came and went with it. research/289 (PlusWireframeTaaNearOnly):
    // only a frame whose wireframes (a band's lines too) cover enough of the
    // view, near ones; off, any. Any thread.
    bool drawnLately();

    // research/256: the materialize band.
    // How a batch takes it (LLDrawInfo::mBandKind): not at all; as a flash of
    // the whole batch (no bounds known: research/282, the wireframe melts into
    // the surface in patches); as a band sweeping up its bounds (mExtents in
    // mModelMatrix's space): a static batch's (review L1, research/282: those
    // of its last full rebuild when vertices rewritten in place dropped
    // mHasExtents), a moving one's its own; a thin one's along its longer
    // horizontal side.
    // research/259: a worn batch (rigged or of an attachment) of a band's
    // pass takes its wearer's band (BAND_WORN), none while there is none.
    enum BandKind : U8 { BAND_NONE = 0, BAND_FLASH = 1, BAND_SWEEP = 2, BAND_SWEEP_MOVING = 3, BAND_WORN = 4 };

    // research/259: what of an avatar a drawAvatar draws: its body (the
    // skinned meshes, whose program takes the band), its eyeballs (a
    // program without vary_position: they stay a wireframe until the band's
    // front passes them), or its body's alpha (the system hair, eyelashes
    // and skirt after the deferred passes, a program without the band: a
    // wireframe until the band ends, as the alpha attachments).
    enum AvatarPart : U8 { AVATAR_BODY = 0, AVATAR_EYES = 1, AVATAR_ALPHA = 2 };

    // The avatar that wears a face's object (LLDrawInfo::mWireOwner; registerFace):
    // the one it is rigged to, or its attachment's. research/277 E: with
    // PlusMaterializeAvatar, an attached animesh's wearer, not its control
    // avatar (the wearer's wireframe and band take it too, its rigged faces
    // and its unrigged prims alike), and none for a HUD's face.
    const LLVOAvatar* wireOwner(const LLFace& face);

    // LLVolumeGeometryManager::registerFace, for each face it adds to info:
    // the batch's kind when it is new (pass: the face's render pass, not the
    // rigged one; worn: rigged or of an attachment), and for one that moves
    // (moving: the face, or null for a static or animated child's batch) its
    // bounds as the face extents grow them (registerFace keeps a static one's).
    // research/277 (with PlusMaterializeAvatar): a worn batch made while its
    // wearer is a wireframe as a whole is held on the GL path as one
    // (HOLD_WIRE, as its wearer's walks hold the others), and one made while
    // its wearer's band sweeps it is held to the band's end (C: it joins the
    // band at once).
    void noteBatch(LLDrawInfo& info, bool new_info, U32 pass, bool worn, bool region_space, const LLFace* moving);

    // LLViewerFetchedTexture::postCreateTexture (main thread): texture has its
    // image now. When it was shown as a wireframe until then (research/276: a
    // Scope drew it as one in this wait, within the last second of the bands'
    // clock, research/278; it waited
    // BAND_AFTER_SECONDS or more, not given up), after the login
    // curtain and under materialize::MAX_BANDS, its band starts:
    // mGrayWireDone, and its image held on the GL path while the band lasts
    // (LLImageGL::bandHeld, where drawBatch draws it). Once per wait: the next
    // resolution of the same image does not start another. Also where the
    // creation ends without postCreateTexture's work (re-review L-2:
    // addToCreateTexture's branches that need no creation, postCreateTexture's
    // early return): onImageScheduled's hold ends there too.
    void onImageCreated(LLViewerTexture* texture);

    // Review M1: LLViewerFetchedTexture::scheduleCreateTexture, when the image
    // is made on another thread (main thread): it gets its GL name a frame or
    // more before postCreateTexture. A texture drawn as a wireframe now (a
    // Scope drew it as one within the last second of the bands' clock) stays
    // held on the GL path and a wireframe (isGray) until onImageCreated, so
    // its band starts from the wireframe, not from a frame of the surface.
    void onImageScheduled(LLViewerTexture* texture);

    // Whether a band may be under way at all (while false, drawBatch does not look at the batches).
    bool bandsActive();

    // Wireframe for the draw calls of its scope, then the polygon mode it
    // found (research/256: not FILL whatever it was, for scopes inside one
    // another). Does nothing for the shadow, reflection, cube snapshot (water
    // mirror, probes) and, with the avatar band on, impostor passes, with the
    // whole-scene wireframe on (it is wireframe already) or with the setting
    // off; research/289: a batch's asks nothing there either (not isGray).
    // research/276: when it draws one for a texture of the batch, that texture
    // is noted as drawn as one (mGrayWireShown: its grace, its band);
    // research/278: every gray texture of the batch, not only the first.
    // research/259: blended (an alpha blended batch, which takes no band): a
    // worn one stays a wireframe while its wearer's band sweeps it too (its
    // hair does not show over the wireframe of its head); not into an
    // impostor. research/284: an alpha blended one stays a wireframe while its
    // own textures' band is under way (with PlusAlphaMaskRebuild); research/289:
    // a worn one only when its wearer's opaque batches flash in their own
    // textures' band (the avatar band on, its wearer materialized before).
    // An avatar's part: its eyes while the band's front is under them, its
    // body's alpha while its band lasts.
    class Scope
    {
    public:
        explicit Scope(const LLDrawInfo& params, bool blended = false);
        explicit Scope(const LLVOAvatar* avatar, AvatarPart part = AVATAR_BODY);
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        ~Scope();

        bool on() const { return mOn; }

    private:
        void begin();
        // research/256: with PlusMaterialize, the waiting wireframe's glow (materialize_params.x 3, the
        // colour times the pulsing strength added to the emissive: the lines show at night too) on a
        // program that takes the band's uniforms; back to 0 when the scope ends.
        void beginGlow();
        bool mOn = false;
        bool mPreviousLines = false;
        LLTexture* mPrevious = nullptr;
        LLGLSLShader* mGlowShader = nullptr;
        LLGLSLShader* mPreviousGlow = nullptr;  // the outer Scope's, given back after
    };

    // research/282: a sweep's line in agent space (llgraywire.cpp).
    struct Sweep;

    // research/256: one batch's band in this pass, for its two draws: lines()
    // before the draw of the wireframe above the band (materialize_params.x 2,
    // z the lines' glow as the waiting wireframe's), surface() before the draw
    // of the real surface below it (1, z the edge's glow). The
    // uniforms back to 0 on the shader they were set on, the polygon mode and
    // texture unit 0's stand-in back to what they were, when it ends (also
    // after one of the draws returned early). Off (on() false) where a band is
    // not drawn: no band for the batch, the shadow, reflection, cube snapshot
    // and HUD passes, no program bound or one that does not clip by the band
    // (it does not declare materialize_params), or inside another band.
    // research/259: a worn batch (BAND_WORN) and an avatar's body take the
    // avatar's band (one sweep for all of it).
    class Band
    {
    public:
        explicit Band(const LLDrawInfo& params);
        explicit Band(const LLVOAvatar* avatar);
        Band(const Band&) = delete;
        Band& operator=(const Band&) = delete;
        ~Band();

        bool on() const { return mShader != nullptr; }
        // research/282 main review L3: whether its line pass may keep anything (a flash's not once it has
        // crossed the ripple: no draw, nor TAA's jitter held for it).
        bool hasLines() const { return mHasLines; }
        void lines();
        void surface();

    private:
        // On the program bound now, when it takes the band: a band of that kind (bandNow's), its
        // line (a sweep's, agent space) and how far it is (0 to 1).
        void start(BandKind kind, const Sweep& sweep, F32 progress);
        void setMode(F32 mode);
        LLGLSLShader* mShader = nullptr;
        bool mHasLines = false;
        F32 mEdgeStrength = 0.f;    // materialize_params.z of the surface pass (the edge's glow)
        F32 mLineGlow = 0.f;        // and of the line pass (the lines' glow, as the waiting wireframe's)
        // research/289: what it is drawn for (a batch, or an avatar's body), whose share of the view its lines
        // tell TAA's jitter (they outlive it: drawBatch, drawAvatar)
        const LLDrawInfo* mBatch = nullptr;
        const LLVOAvatar* mAvatar = nullptr;
        LLTexture* mPrevious = nullptr;
        bool mPreviousLines = false;
        F32 mPlane[4] = {};
        F32 mParams[4] = {};
    };

    // While one lives, no Band is on (a draw that must stay one draw, nested
    // drawBatch calls included).
    class NoBand
    {
    public:
        NoBand();
        NoBand(const NoBand&) = delete;
        NoBand& operator=(const NoBand&) = delete;
        ~NoBand();

    private:
        bool mPrevious = false;
    };

    // The draws of a band that is on: the wireframe above it (research/282: a flash's too, while
    // it has any), then the surface below it.
    template <typename Draw>
    void drawBand(Band& band, Draw& draw)
    {
        if (band.hasLines())
        {
            band.lines();
            draw();
        }
        band.surface();
        draw();
    }

    // The draw of one batch at a Scope's place: draw() once, as a wireframe
    // while a texture of it is not loaded (Scope); twice while its band
    // sweeps it or it flashes (Band; once when a flash has crossed). band_ok
    // false: once, with no band inside either (a
    // draw whose record the caller notes). A drawBatch inside another one's
    // band draws once (that one's pass is set already).
    template <typename Draw>
    void drawBatch(const LLDrawInfo& params, Draw&& draw, bool band_ok = true)
    {
        Scope wire(params);
        if (!band_ok)
        {
            NoBand once;
            draw();
            return;
        }
        if (!wire.on() && bandsActive())
        {
            Band band(params);
            if (band.on())
            {
                drawBand(band, draw);
                return;
            }
        }
        draw();
    }

    // research/259: the draw of an avatar's body or eyes (LLDrawPoolAvatar::renderAvatars), as
    // drawBatch's: a wireframe while the avatar is one as a whole (Scope), twice while its band
    // sweeps it (the eyes: a wireframe until the front passes them).
    template <typename Draw>
    void drawAvatar(const LLVOAvatar* avatar, Draw&& draw, AvatarPart part = AVATAR_BODY)
    {
        Scope wire(avatar, part);
        if (!wire.on() && bandsActive())
        {
            Band band(avatar);
            if (band.on())
            {
                drawBand(band, draw);
                return;
            }
        }
        draw();
    }
}

#endif // LL_LLGRAYWIRE_H
