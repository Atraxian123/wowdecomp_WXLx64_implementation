// Per-model physics files: finds, reads and caches the .phy2 that sits beside a model.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// Nothing in here touches the engine: the caller supplies a ReadFn, so the same code runs in the
// client (SFile) and in a plain test program.
//
// A model's physics lives next to the model, under the model's own name:
//
//     item\objectcomponents\weapon\staff_2h_raidmidnight_d_02.m2
//     item\objectcomponents\weapon\staff_2h_raidmidnight_d_02.phy2      <- this file
//
// When the client is handed a virtual path instead (see VirtualPath.hpp), the .phy2 is looked for
// beside the ORIGINAL .m2 the virtual path was minted from.
#pragma once

#include "physics_domino.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace wxl::equip::phy2
{
    namespace phys = wow::ec::physics;

    /// Normalizes a model path the way the client's archives see it: backslashes, lower case.
    std::string NormalizeModelPath(const char* path);

    /**
     * @brief The .phy2 paths to try for a model path, most specific first, no duplicates.
     *
     *   1. the path itself with its extension replaced by .phy2 (a normal model, or a virtual path
     *      that happens to have a file of its own);
     *   2. a collection key  <stem>_wxl_<...>_cmo<hex>.m2  ->  <stem>.phy2
     *   3. a global key      <stem>[_oh]_<displayId>.m2    ->  <stem>.phy2, and without a trailing _oh
     *      (only when includeGlobal is true)
     *
     * Step 3 is a guess from the SHAPE of the name, and real models have that shape too (the retail
     * variants staff_x_01.m2 / staff_x_02.m2 are different models), so the caller passes
     * includeGlobal only for a path it has confirmed is a minted virtual key.
     * The key shapes are those of VirtualPath.cpp (BuildKey / BuildGlobalVirtualKey).
     */
    std::vector<std::string> Candidates(const char* modelPath, bool includeGlobal);

    /// Reads a whole file; false when it does not exist or cannot be read. Must be thread-safe.
    using ReadFn = bool (*)(const char* path, std::vector<uint8_t>& out);
    /// True when the path is a virtual key minted by VirtualPath (see VPathIsVirtualKey). Thread-safe.
    using VirtualFn = bool (*)(const char* path);

    /// Decodes a PHY2 blob that holds one model's rows into `out`. On failure returns false and
    /// puts a short reason in *why (when given).
    bool Parse(const uint8_t* blob, size_t size, phys::Model& out, const char** why = nullptr);

    struct Result
    {
        std::shared_ptr<phys::Model> model;   // null: this model has no usable physics
        std::string                  path;    // the .phy2 that was used (empty when none)
        std::string                  error;   // a file was found but could not be used, and why
    };

    /// Looks a model's physics up once and remembers the answer (a miss is remembered too).
    class Library
    {
    public:
        explicit Library(ReadFn read, VirtualFn isVirtual = nullptr) : m_read(read), m_isVirtual(isVirtual) {}

        /// Safe to call from several threads; files are read outside the lock.
        /// `fresh` (when given) is set true only for the call that actually went to disk.
        Result For(const char* modelPath, bool* fresh = nullptr);

        /// Forgets everything, so edited .phy2 files are read again.
        void Clear();

        size_t Cached() const;
        size_t Found() const;   // cached models that have physics

    private:
        ReadFn m_read;
        VirtualFn m_isVirtual;
        mutable std::mutex m_lock;
        std::unordered_map<std::string, Result> m_cache;
    };
}
