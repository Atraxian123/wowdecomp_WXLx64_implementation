// The standalone module drew its settings panel through the WXL overlay. The decompiled client has no overlay;
// the module is configured through wxl-modern-water.ini, so these entry points have nothing to do.
// Copyright (C) 2026 WarcraftXL (GPLv3, see LICENSE)

#include "overlay.h"

void AttachOverlay(IDirect3DDevice9*, HWND) {}
void DetachOverlay(IDirect3DDevice9*) {}
void ReleaseOverlayDeviceObjects(IDirect3DDevice9*) {}
void DrawOverlay(IDirect3DDevice9*) {}
bool OverlayVisible() { return false; }
PanelPlacement OverlayPanelPlacement() { return PanelPlacement(); }
