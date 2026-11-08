/**
 * @file otherland.h
 * @brief Hides the objects on land of another owner than the parcel the agent is on.
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

#ifndef PLUS_OTHERLAND_H
#define PLUS_OTHERLAND_H

// A parcel can keep its avatars from being seen from other parcels (the land's
// "Avatars on other parcels can see..." option): the simulator decides, for every
// visitor. Objects have nothing of the kind, so this is the viewer's own and only
// for the one who turns it on (the World menu, two switches after the land items, menu_viewer.xml):
//   PlusHideOtherParcelObjects  the objects whose linkset root stands on another
//                               parcel than the agent's, whoever owns it (the
//                               parcels as the property lines cut them; one parcel
//                               in two pieces counts as two)
//   PlusHideOtherLandObjects    ... on a parcel of another owner than the agent's
//                               parcel (a neighbouring parcel of the same owner stays)
// With both on, the parcel's lines decide. Hidden objects are not drawn, cast no
// shadow, give no light, show no floating text and are not clicked. Nothing is sent
// to the simulator about them.
//
// The parcels: the region's parcel overlay (the property lines the viewer already
// has) cuts each region into its parcels. When the lines or the ownership bits
// change (a sale, a division, a join), the old cut stays in use until the new one
// is made; a parcel that did not change keeps its owner, another one takes the old
// owner under it for the time being and is asked again.
// Who owns a parcel (PlusHideOtherLandObjects only): for each parcel an object
// stands on, the RemoteParcelRequest capability gives its parcel id and
// ParcelInfoRequest its owner (the same requests as a landmark's place profile),
// asked again after 10 minutes (a sale between two others changes no bit of the
// overlay). Owners compared by id: a group-owned parcel's owner is the group.
//
// Shown all the same: avatars and what they wear, the objects the agent sits on
// or has selected, objects not yet known where they stand (the region's lines not
// settled, or the owner never known), GLTF scenes (drawn and picked apart from the
// drawables). Sounds of hidden objects stay. Their particle sources stop making
// particles (LLViewerPartSource::setSuspended(), as for an orphaned prim; the
// particles already made live out their time) and start again when shown. Your own
// objects on another's land are hidden too: the land decides, not who owns the object.
//
// Hidden by LLDrawable::FORCE_INVISIBLE (as the pathfinding window hides a
// character), which only LLVolumeGeometryManager::rebuildGeom() reads: hiding and
// showing make the prim's spatial group's geometry dirty (dirtySpatialGroup()), so
// the group is made again without (with) the faces in the same frame, and the RHI
// draw and shadow lists follow it (rhiBuilds()). A drawable's own rebuild
// (markRebuild) only refreshes the group's vertices and would leave the faces drawn
// until something else rebuilt the group (a LOD change as the camera moves). An
// update of the object clears the state (LLViewerObject::processUpdateMessage()),
// so it is set again every frame, after the frame's messages and before the world
// is drawn. A prim that was FORCE_INVISIBLE before (the pathfinding window's) is
// left so when shown. New objects, and objects made again from the cache, are
// judged in the frame they come, before they are drawn.
//
// The parcels are followed with the switches off too (the overlay compared once a
// second), so that turning one on judges every object at once on regions already
// cut. Log tag OtherLand: a line when it is turned on (objects judged, time,
// linksets hidden) or off, and a status line when it changed (at most every 5 s).
//
// Hook: LLVOVolume::lineSegmentIntersect() (llvovolume.cpp) leaves out a drawable
// that is FORCE_INVISIBLE (hovering, clicking, the camera's focus).
//
// Nothing to call: the feature registers itself (otherland.cpp) and runs in an
// idle callback.

#endif // PLUS_OTHERLAND_H
