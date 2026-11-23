// WXL: "<root>_csN" WMO groups are drawn and portal-culled as part of their "<root>" group.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
//
// WMOs converted from modern ones have groups whose name is another group's name plus "_cs1", "_cs2", ... (a group that
// was split off because of the vertex limit). They have no portals of their own, so portal culling only reaches them
// when the camera stands inside their bounds. Here a "cluster" is a root group together with all of its _csN groups:
//   - visiting any member of a cluster (as a portal destination or as a start group) visits the whole cluster;
//   - the portals of every member count as portals of the cluster, so a chain of portals through a _csN group is
//     unbroken (the same holds when a portal's destination is a _csN group: it leads into the root's cluster);
//   - a group that is not part of a cluster is its own root: everything behaves as before.
// Names live in the group headers, so a group's cluster is only known once the group is loaded. Refresh() is called at the
// start of every portal traversal and rebuilds the table when a group was loaded or unloaded since the last call.
//
// [WMO] GroupClusters=0 in WXLSetup.ini turns it off (default 1).
#pragma once

#include <cstdint>

class CMapObj;

namespace wxl::wmocs
{
    /// Brings the cluster table of `mapObj` up to date with the groups that are loaded right now.
    void Refresh(CMapObj* mapObj);

    /// Frees the table of `mapObj`. Safe when there is none.
    void Unload(CMapObj* mapObj);

    /// The root group of `group`'s cluster; `group` itself when it has none (or is out of range / 0xFFFF).
    uint32_t RootOf(const CMapObj* mapObj, uint32_t group);

    /// Number of groups in the cluster of root group `root` (the root included), 1 for a group without a cluster.
    uint32_t MemberCount(const CMapObj* mapObj, uint32_t root);

    /// The i-th group of the cluster of `root`; index 0 is the root itself.
    uint32_t Member(const CMapObj* mapObj, uint32_t root, uint32_t i);
}
