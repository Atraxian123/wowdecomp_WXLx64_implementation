// Safe handling of the client's Direct3D 9 device, for every module that draws or changes device state.
// Copyright (C) 2026 WarcraftXL (GPLv3, see ../Event.h)
//
// THE RULE: no saved state, no changes.
//
//   The client's device is shared with the client. It caches the states it believes are set and does not send them
//   again, so anything a module changes and fails to put back stays changed: no depth test, no culling, the wrong
//   viewport, an unbound vertex stream. The module's own frame may look fine; the client's goes wrong, and keeps going
//   wrong until the client happens to set the same state again.
//
//   Two things make "put it back" less certain than it looks, and both were met in this client:
//
//   * Get* calls can fail. The client creates its device with D3DCREATE_PUREDEVICE, which does not answer for anything
//     a state block can store, and a call that fails leaves its out-parameter untouched. Reading it anyway and writing
//     it back later puts a stale or zero value into the device (a stencil function of 0 is not even a valid enum).
//   * A state block can fail to be created or captured, most easily in the first frames after a loading screen, when
//     video memory is full. A caller that ignores that goes on to change everything and restores nothing.
//
//   So: before the first change, read everything that will be needed to undo it, and check that every read worked.
//   If any did not, do not start: skip the pass and say why in the log. After the work, restore only what was read.
//   Nothing here ever writes back a value that was not first read successfully.
//
// What this header gives you:
//
//   Seen / QueryRenderTarget / QueryDepthStencil / QueryStream0
//       The three bindings a pass most often replaces, read so that "the device said nothing is bound" and "the device
//       did not answer" are different results. Only Seen::No means the state is unknown.
//
//   ReadRenderState / ReadRenderStates
//       Render states read all-or-nothing: the values are only handed out when every read worked.
//
//   ScopedDeviceState
//       A D3DSBT_ALL state block, created and captured in the constructor. Valid() says whether that worked; when it
//       did not, the object does nothing and the caller must not change the device. Restore() (also run by the
//       destructor) applies it once.
//
// This is a header-only helper with no dependency on the rest of WXL, so a module can include it on its own.
#pragma once

#include <cstddef>
#include <d3d9.h>

namespace wxl::d3d9guard
{
    /// What a query of the device answered.
    enum class Seen : unsigned char
    {
        No = 0,   // the device did not answer: the state is unknown, and must not be written back
        Empty,    // the device answered that nothing is bound
        Bound,    // the device answered with an object; the caller owns one reference to it
    };

    /// True when the device answered, whatever it said.
    inline bool Answered(Seen s) { return s != Seen::No; }

    /// Render target `index`. `out` holds a reference only when the result is Bound. After a call that failed it is
    /// set to null without being touched: a failed call is not trusted to have produced an object, and so there is
    /// nothing to release. D3DERR_NOTFOUND is a real answer (no target in that slot).
    inline Seen QueryRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9*& out)
    {
        out = nullptr;
        if (!dev) return Seen::No;
        const HRESULT hr = dev->GetRenderTarget(index, &out);
        if (FAILED(hr))
        {
            out = nullptr;
            return hr == D3DERR_NOTFOUND ? Seen::Empty : Seen::No;
        }
        return out ? Seen::Bound : Seen::Empty;
    }

    /// The depth-stencil surface. Same contract as QueryRenderTarget.
    inline Seen QueryDepthStencil(IDirect3DDevice9* dev, IDirect3DSurface9*& out)
    {
        out = nullptr;
        if (!dev) return Seen::No;
        const HRESULT hr = dev->GetDepthStencilSurface(&out);
        if (FAILED(hr))
        {
            out = nullptr;
            return hr == D3DERR_NOTFOUND ? Seen::Empty : Seen::No;
        }
        return out ? Seen::Bound : Seen::Empty;
    }

    /// Vertex stream 0. `buffer` holds a reference only when the result is Bound; the offset and stride are 0 otherwise.
    inline Seen QueryStream0(IDirect3DDevice9* dev, IDirect3DVertexBuffer9*& buffer, UINT& offset, UINT& stride)
    {
        buffer = nullptr;
        offset = 0;
        stride = 0;
        if (!dev) return Seen::No;
        const HRESULT hr = dev->GetStreamSource(0, &buffer, &offset, &stride);
        if (FAILED(hr))
        {
            buffer = nullptr;
            offset = 0;
            stride = 0;
            return Seen::No;
        }
        return buffer ? Seen::Bound : Seen::Empty;
    }

    /// One render state. `out` is only written when the read worked.
    inline bool ReadRenderState(IDirect3DDevice9* dev, D3DRENDERSTATETYPE state, DWORD& out)
    {
        if (!dev) return false;
        DWORD value = 0;
        if (FAILED(dev->GetRenderState(state, &value))) return false;
        out = value;
        return true;
    }

    /// Several render states, all or nothing: `values` is only written when every read worked.
    template <size_t N>
    inline bool ReadRenderStates(IDirect3DDevice9* dev, const D3DRENDERSTATETYPE (&states)[N], DWORD (&values)[N])
    {
        DWORD read[N] = {};
        for (size_t i = 0; i < N; ++i)
            if (!ReadRenderState(dev, states[i], read[i])) return false;
        for (size_t i = 0; i < N; ++i)
            values[i] = read[i];
        return true;
    }

    /// The whole device state in a state block, or nothing. See the rule at the top of this file.
    class ScopedDeviceState
    {
    public:
        explicit ScopedDeviceState(IDirect3DDevice9* dev)
        {
            if (dev && SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL, &state_)) && state_)
            {
                if (FAILED(state_->Capture()))
                {
                    state_->Release();
                    state_ = nullptr;
                }
            }
        }
        ~ScopedDeviceState() { Restore(); }

        ScopedDeviceState(const ScopedDeviceState&) = delete;
        ScopedDeviceState& operator=(const ScopedDeviceState&) = delete;

        /// False when the state could not be saved: the caller must leave the device alone.
        bool Valid() const { return state_ != nullptr; }

        /// Puts the saved state back, once. Safe to call when not Valid().
        void Restore()
        {
            if (!state_) return;
            state_->Apply();
            state_->Release();
            state_ = nullptr;
        }

    private:
        IDirect3DStateBlock9* state_ = nullptr;
    };
}
