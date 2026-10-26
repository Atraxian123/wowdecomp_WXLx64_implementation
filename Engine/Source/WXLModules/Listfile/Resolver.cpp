// See Resolver.hpp.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)

#include "Resolver.hpp"

#include <cstdio>
#include <cstring>

namespace wxl::listfile
{
    namespace
    {
        constexpr size_t kMaxLoggedKeys = 20000;   // distinct missing files logged before the log stops growing

        std::string Backslashes(const char* path)
        {
            std::string s(path);
            for (char& c : s) if (c == '/') c = '\\';
            return s;
        }

        bool EndsWith(const std::string& s, const char* ext)
        {
            const size_t n = std::strlen(ext);
            if (s.size() < n) return false;
            for (size_t i = 0; i < n; ++i)
            {
                char c = s[s.size() - n + i];
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                if (c != ext[i]) return false;
            }
            return true;
        }

        uint32_t KindOfExtension(const std::string& path)
        {
            if (EndsWith(path, ".blp")) return WXL_LISTFILE_TEXTURE;
            if (EndsWith(path, ".m2") || EndsWith(path, ".mdx")) return WXL_LISTFILE_MODEL;
            return WXL_LISTFILE_ANY;
        }

        const char* KindName(uint32_t kind)
        {
            return kind == WXL_LISTFILE_TEXTURE ? "texture" : kind == WXL_LISTFILE_MODEL ? "model" : "any type";
        }
    }

    void Resolver::Configure(Table* table, Hooks hooks, bool checkExists, std::string notFoundName)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_table = table;
        m_hooks = hooks;
        m_checkExists = checkExists;
        m_notFoundName = Backslashes(notFoundName.c_str());
        m_logged.clear();
        m_logFull = false;
    }

    void Resolver::LogMissing(const std::string& key, const char* what)
    {
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (m_logged.size() >= kMaxLoggedKeys)
            {
                if (m_logFull) return;
                m_logFull = true;
                if (m_hooks.log) m_hooks.log("not-found log is full: further distinct missing files are not logged");
                return;
            }
            if (!m_logged.insert(key).second) return;   // this one was reported already
        }
        if (m_hooks.log) m_hooks.log(what);
    }

    // The name the client gets for a file that is not found. It is not a file anywhere: opening it fails, and the
    // client's own handling for a texture (the green one) or a model (the cube) takes over. The extension is there
    // so the engine sends it down the right loader.
    std::string Resolver::NotFoundFor(uint32_t kind) const
    {
        if (kind == WXL_LISTFILE_TEXTURE) return m_notFoundName + ".blp";
        if (kind == WXL_LISTFILE_MODEL)   return m_notFoundName + ".m2";
        return m_notFoundName;
    }

    int Resolver::Deliver(const std::string& path, int result, char* out, size_t outSize)
    {
        if (!out || path.size() + 1 > outSize) return WXL_LISTFILE_ERROR;
        std::memcpy(out, path.c_str(), path.size() + 1);
        return result;
    }

    int Resolver::LookupPath(uint32_t fileId, char* out, size_t outSize)
    {
        if (!m_table || !out || outSize == 0) return -1;
        std::string path;
        if (!m_table->Lookup(fileId, path))
        {
            char msg[160];
            std::snprintf(msg, sizeof(msg), "file id %u is not in the listfile", fileId);
            LogMissing("id:" + std::to_string(fileId), msg);
            return 0;
        }
        return Deliver(path, 1, out, outSize) == WXL_LISTFILE_ERROR ? -1 : 1;
    }

    int Resolver::ResolvePath(const char* rawPath, uint32_t kind, char* out, size_t outSize)
    {
        return ResolveImpl(rawPath, kind, out, outSize, nullptr);
    }

    // fileId, when the path came from a lookup, goes into the log line so one missing file is one line.
    int Resolver::ResolveImpl(const char* rawPath, uint32_t kind, char* out, size_t outSize, const uint32_t* fileId)
    {
        if (!rawPath || !*rawPath || !out || outSize == 0) return WXL_LISTFILE_ERROR;
        const std::string path = Backslashes(rawPath);

        const bool there = !m_checkExists || !m_hooks.exists || m_hooks.exists(path.c_str());
        if (there) return Deliver(path, WXL_LISTFILE_FOUND, out, outSize);

        if (kind == WXL_LISTFILE_ANY) kind = KindOfExtension(path);
        const std::string answer = NotFoundFor(kind);
        std::string msg = fileId ? "file id " + std::to_string(*fileId) + " -> '" + path + "'" : "'" + path + "'";
        msg += " is not in the client (" + std::string(KindName(kind)) + "): answering '" + answer + "'";
        LogMissing("path:" + path, msg.c_str());
        return Deliver(answer, WXL_LISTFILE_NOTFOUND, out, outSize);
    }

    int Resolver::Resolve(uint32_t fileId, uint32_t kind, char* out, size_t outSize)
    {
        if (!m_table || !out || outSize == 0) return WXL_LISTFILE_ERROR;

        std::string path;
        if (m_table->Lookup(fileId, path))
            return ResolveImpl(path.c_str(), kind, out, outSize, &fileId);   // in the listfile: is it in the client?

        // Not in the listfile: nothing is known about it but what the caller expects it to be.
        const std::string answer = NotFoundFor(kind);
        char msg[320];
        std::snprintf(msg, sizeof(msg), "file id %u is not in the listfile (%s): answering '%s'", fileId, KindName(kind), answer.c_str());
        LogMissing("id:" + std::to_string(fileId), msg);
        return Deliver(answer, WXL_LISTFILE_NOTFOUND, out, outSize);
    }
}
