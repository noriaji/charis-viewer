/**
 * @file llvowlsky.h
 * @brief LLVOWLSky class definition
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_VOWLSKY_H
#define LL_VOWLSKY_H

#include "llviewerobject.h"
#include "plus/starfield/starfield.h" // <plus> </plus>
#include "plus/starfield/skymilky.h" // <plus> </plus>
#include "plus/realstars/realstarart.h" // <plus> research/288 </plus>
#include "plus/realstars/realstars.h" // <plus> research/288: the pictures' reader </plus>

class LLVOWLSky : public LLStaticViewerObject {
private:
    inline static U32 getNumStacks(void);
    inline static U32 getNumSlices(void);
    inline static U32 getStripsNumVerts(void);
    inline static U32 getStripsNumIndices(void);
    inline U32 getStarsNumVerts(void) const; // <plus> was static: the star field has its own count </plus>
    inline static U32 getStarsNumIndices(void);

public:
    LLVOWLSky(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp);

    /*virtual*/ void         idleUpdate(LLAgent &agent, const F64 &time);
    /*virtual*/ bool         isActive(void) const;
    /*virtual*/ LLDrawable * createDrawable(LLPipeline *pipeline);
    /*virtual*/ bool         updateGeometry(LLDrawable *drawable);

    void drawStars(void);
    // <plus> plus/starfield (research/176)
    void drawStarGlow(void);                        // the Milky Way's patches (PlusSkyStars 2)
    void refreshStars(void);                        // follows PlusSkyStars; main thread
    S32  getStarMode(void) const { return mStarMode; }
    bool hasStarGlow(void) const { return mGlowCount > 0; }
    // plus/realstars (research/177): the real sky (stars in J2000 over the whole sphere,
    // turned by lldrawpoolwlsky.cpp) and its constellation lines
    bool isStarReal(void) const { return mStarReal; }
    bool hasStarLines(void) const { return mLineCount > 0; }
    void drawStarLines(void);
    // research/288: the constellation pictures over the real sky, plates after the lines. The
    // pictures (a 2D array, BC4) are read on a thread when first wanted and uploaded the frame they
    // are in (prepareStarArt(), before the pass state is declared: no texture is made inside the
    // pass); let go when PlusSkyStarsArt is off (refreshStars()), when the real sky goes and with
    // the GL (cleanupGL(), LLSky::cleanup()).
    bool hasStarArt(void) const { return mArtCount > 0; }
    U32  prepareStarArt(void);     // the pictures' texture name, made once their file is read; 0: none yet
    void drawStarArt(void);
    void releaseStarArt(void);     // lets the texture and a read in flight go: LLSky::cleanup() too
    void dropStarArtIfUnwanted(void);   // releaseStarArt() unless the pictures are drawn (switch on, the
                                        // real sky); switched off, a failed texture may be tried again
    // research/224: the baked Milky Way (PlusSkyStarsMilkyWayHQ). The picture is there once its
    // bake is uploaded (the sky has no Milky Way until then); the box is drawn by the program
    // gDeferredStarMilkyProgram with the star field's matrix.
    bool wantsMilkyPicture(void) const { return mStarHQ && mStarMode >= plus::starfield::MODE_MILKY_WAY; }
    LLImageGL* getMilkyTexture(void) const { return mMilky ? mMilky->texture() : nullptr; }
    // Makes the box's vertex buffer if it is not there (false: it could not be made, nothing to draw).
    // Before the pass state is declared, so no buffer is made inside the pass (research/207 T4).
    bool prepareMilkyBox(void);
    void drawMilkyBox(void);   // the box as prepareMilkyBox() made it
    // </plus>
    void drawDome(void);
    void drawFsSky(void); // fullscreen sky for advanced atmo
    void resetVertexBuffers(void);

    void cleanupGL();
    void restoreGL();

private:

    // helper function for initializing the stars.
    void initStars();

    // helper function for building the strips vertex buffer.
    // note begin_stack and end_stack follow stl iterator conventions,
    // begin_stack is the first stack to be included, end_stack is the first
    // stack not to be included.
    static void buildStripsBuffer(U32 begin_stack, U32 end_stack,
                                  LLStrider<LLVector3> & vertices,
                                  LLStrider<LLVector2> & texCoords,
                                  LLStrider<U16> & indices,
                                  const F32 RADIUS,
                                  const U32& num_slices,
                                  const U32& num_stacks);

    // helper function for updating the stars colors.
    void updateStarColors();

    // helper function for updating the stars geometry.
    bool updateStarGeometry(LLDrawable *drawable);

    // <plus> research/224: asks the baker for the picture of the Milky Way's plane (the real sky's
    // or the made-up one's) while it is wanted, lets it go when it is not
    void refreshMilky();
    // </plus>

    // <plus> plus/starfield: the star field's vertex buffer (stars, then patches)
    bool updateStarFieldGeometry();
    // </plus>

private:
    LLPointer<LLVertexBuffer>                   mFsSkyVerts;
    std::vector< LLPointer<LLVertexBuffer> >    mStripsVerts;
    LLPointer<LLVertexBuffer>                   mStarsVerts;

    std::vector<LLVector3>  mStarVertices;              // Star verticies
    std::vector<LLColor4>   mStarColors;                // Star colors
    std::vector<F32>        mStarIntensities;           // Star intensities

    // <plus> plus/starfield (research/176): PlusSkyStars the stars were made for, the
    // catalogue (mStarVertices / mStarColors hold its stars), the patches' count
    S32                         mStarMode = 0;
    U32                         mGlowCount = 0;
    plus::starfield::Field      mStarField;
    // plus/realstars: PlusSkyStarsReal as asked when the stars were made, whether the real sky
    // came of it (its catalogue read), the constellation lines' segments (after the patches)
    bool                        mStarRealAsked = false;
    bool                        mStarReal = false;
    U32                         mLineCount = 0;
    F32                         mStarBoost = 0.f;   // PlusSkyStarsConstellationBoost the colours were made with
    // research/288: the constellation pictures' plates (read with the real sky), their count in
    // the buffer (after the lines), the pictures (a GL name; 0: not made) and their file's reader
    // while it reads (a thread). A texture that could not be made is not tried again until the
    // pictures are switched off and on, or the stars are made again.
    plus::realstars::Art        mStarArt;
    U32                         mArtCount = 0;
    U32                         mArtTexture = 0;
    bool                        mArtTextureFailed = false;
    std::unique_ptr<plus::realstars::ArtPicturesReader> mArtReader;
    // research/224: PlusSkyStarsDense and the baked Milky Way (PlusSkyStarsMilkyWayHQ, with
    // PlusSkyStars 2) as they were when the stars were made
    bool                        mStarDense = false;
    bool                        mStarHQ = false;
    std::unique_ptr<plus::starfield::MilkyBaker> mMilky;    // made when first wanted
    LLPointer<LLVertexBuffer>   mMilkyBox;                  // the 12 triangles of a box round the camera
    // </plus>
};

#endif // LL_VOWLSKY_H
