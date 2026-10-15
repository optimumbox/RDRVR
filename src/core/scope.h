#pragma once
// [Scope] (2026-10-15, a player: "the rifle scope doesn't work, there's no scope view"): a magnified lens on the scoped
// rifles (Rolling Block, Carcano), KF2-style. Each XR frame the patch of an eye image around the point the barrel aims
// at is copied into the lens image and shown as a quad at the scope's eyepiece, the patch's angle `Zoom` times smaller
// than the lens's. The source eye is the one farther from the barrel's line: the rifle's own scope model covers the
// target in the sighting eye's image, not in the other's (its parallax at the eyepiece is ~10 degrees). The lens is
// framed by a mask quad over it (filled once): black round it and a crosshair. It lies parallel to the source eye's
// image (upright as that eye saw it) and shows only with the rifle at the face ([Scope] RaiseDistance, RaiseAngle). The presenting thread.

#include <d3d12.h>

#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#ifndef XR_USE_GRAPHICS_API_D3D12
#define XR_USE_GRAPHICS_API_D3D12
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace rdrvr::scope {

void init();
// After the eye blit, before the eye images are released: eyes[2] (RENDER_TARGET), their content iw x ih at the image
// origin, `fmt` their format. Copies this frame's lens image on `queue`; true when a lens layer is ready for layer().
bool prepare(const XrView* views, XrSession session, ID3D12CommandQueue* queue, ID3D12Resource* const eyes[2], uint32_t iw, uint32_t ih,
             DXGI_FORMAT fmt);
// This frame's lens quads (after a true prepare): the lens and its mask (black round it, the crosshair). Returns 2, or 0.
int layers(XrSpace space, XrCompositionLayerQuad out[2]);
// The rifle at the face this XR frame (any thread)
bool raised();
// scope [on|off] [zoom x] [size m] [offset x y z] [raise m deg] [eye auto|left|right] [all on|off]: the session's settings; no args: status
std::string command(const std::string& line);

}  // namespace rdrvr::scope
