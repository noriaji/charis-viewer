# Local Mesh: origin

| | |
|---|---|
| Source viewer | Firestorm (Phoenix Firestorm Project) |
| Repository | https://github.com/FirestormViewer/phoenix-firestorm |
| Commit | `443cc2b4ebf9e00da5bb52f00a3e45be790d4a75` (2026-05-25, master) |
| License | GNU LGPL 2.1 only (`$LicenseInfo:...license=viewerlgpl$`); the copyright lines of the files taken are kept in every file made from them |
| Authors | Vaalith Jinn (Local Mesh: vjlocalmesh, vjlocalmeshimportdae, vjfloaterlocalmesh, 2022), Beq Janus (the glTF importer, fslocalmeshimportgltf, 2026) |

The code was rewritten for this viewer (a loader thread, the four LODs filled
before the volume is set, one window page), following the design of Local Mesh;
the parts noted below are from the files named.

## Files taken

| Here | Firestorm | Changes |
|---|---|---|
| localmesh.h/.cpp | indra/newview/vjlocalmesh.h/.cpp (`LLLocalMeshSystem`, `LLLocalMeshFile`, `LLLocalMeshObject`, `LLLocalMeshFace`: files, LOD files `_LOD0/1/2`, applying to an object, keeping simulator updates off it) | namespace `plus::localmesh`; files read on a worker thread; faces made like downloaded meshes (`cacheOptimize()`, extents, tangents, normals made when missing); all four LOD volumes made and held before `setVolume()` (in Firestorm the lower LODs are dropped right after filling, so a LOD change asks the mesh repository for the made-up id and falls back to a box); the original volume params, scale and lost texture entries put back on removal; the force-selection mode put back when the window closes; the official hook is `holdsUpdates()` in `LLVOVolume::processUpdateMessage()` (as Firestorm) |
| localmeshdae.cpp | vjlocalmeshimportdae.cpp (`LLLocalMeshImportDAE`) | returns plain triangle lists (localmeshread.h); unit and up axis applied; the path escaped for COLLADA (Japanese paths open); index range checked; duplicate vertices found with a map; rigging not read |
| localmeshgltf.cpp | fslocalmeshimportgltf.cpp (`FSLocalMeshImportGLTF`) | the JSON is parsed here without exceptions and only the geometry parts are handed to the official `LL::GLTF::Asset` structures (not `Asset::load()`, which trusts the file), the arrays read and range-checked here; returns plain triangle lists; rigging not read |
| localmeshread.h | the importer interfaces (vjlocalmeshimportdae.h, fslocalmeshimportgltf.h) | one small interface for both readers |
| localmeshfloater.cpp | vjfloaterlocalmesh.cpp (`LLFloaterLocalMesh`) | one page (Firestorm: three tabs); shows the selected object and why a file cannot be shown on it; several files added at once |
| localmesh.cmake | - | new (plus foundation) |
| skins/default/xui/{en,ja}/floater_plus_local_mesh.xml | floater_vj_local_mesh.xml | new layout |
| app_settings/settings_plus.xml (local mesh part) | - | `PlusLocalMeshAutoReload`, `PlusLocalMeshFileSize` |

## Not taken

- Rigged mesh and its settings (joint offsets), and the shared rigging helpers of
  fslocalmeshimportbase.cpp; rigged files are shown as static meshes, and rigged
  objects cannot be used as the target.
- "Rez Selected" (needs a hook in llviewerobjectlist.cpp).
- The choice of LOD suffixes (fixed to the upload's `_LOD0/1/2`) and the scan interval
  (fixed to 1 s).
