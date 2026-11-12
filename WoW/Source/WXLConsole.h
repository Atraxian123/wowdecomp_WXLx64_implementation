// The "wxl" console command: look at WXLSetup.ini and change it live. See WXLConsole.cpp.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
#pragma once

/// Registers the "wxl" console command. Called once from client start-up, after the console exists.
void WxlConsoleInitialize();
