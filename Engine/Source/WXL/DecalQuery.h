// Ground decals (projected textures): which WMO faces they may land on. Opt-out.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
//
// Every projected texture of the client, the targeting circle under the cursor, the selection rings, the spell area
// indicator and the ground textures of spell animations, collects the triangles it is drawn on with one world query
// (World::GetTris). For WMOs that query takes a face only if it is not flagged as collision geometry (flag 0x08 of
// the face), so that a decal does not land on the invisible walls and ramps WMOs have for the physics. A WMO whose
// floor faces are flagged both "render" (0x20) and "collision" (0x08), which is how custom and converted WMOs are
// usually built, is therefore skipped completely, and the decal falls through to the terrain under it.
//
//     [Decals]
//     WmoCollisionFaces=1     ; 1 (default): faces that are both render and collision count too; 0: as the stock client
//
// With 1, the one thing that can show is a decal on an invisible collision-only surface, wherever a WMO has one right
// under the decal's box.
#pragma once

namespace wxl::decals
{
    /// WXLSetup.ini [Decals] WmoCollisionFaces (default 1). Read once, logged once.
    bool WmoCollisionFaces();

    /// The extra bit this adds to the triangle query flags of a decal (not used by anything else of the world query).
    constexpr unsigned kQueryWmoCollisionFaces = 0x400000;
}
