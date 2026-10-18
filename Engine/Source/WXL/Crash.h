// WXL crash logger: records where the client died, with function names when the PDB is present.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
#pragma once

namespace wxl
{
    /**
     * @brief Installs a first-chance exception logger (Windows only; a no-op elsewhere).
     *
     * Writes the exception code, faulting address, access type and a symbolized call stack to WXL.log.
     * It never handles the exception, so the client's own crash handling still runs. Access violations
     * are expected now and then (the equip code probes engine memory under __try on purpose), so each
     * distinct faulting address is logged only a limited number of times and marked as possibly handled.
     */
    void InstallCrashLogger();
}
