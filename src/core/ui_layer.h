#pragma once
// R5 UI offscreen layer (DESIGN R5: "UI never in the eye images"; the UI on its own quad). The game draws its UI last,
// onto the post chain's output (Post FXAA Target, or FXAATarget under TAA) with its own cleared depth buffer bound
// (frame graph: [Post FXAA Target | DS] ~150 draws, then the back-buffer gamma blit). With the redirect on, every bind
// of that target WITH a depth view gets the render-target view of a mod-owned RGBA16F target instead, cleared to
// transparent at the frame's first such bind, so the UI lands there alone. Only a CPU descriptor handle changes in the
// game's call; the mod's target stays in RENDER_TARGET except inside the mod's own frame-end list.
// Step 1 (this file): the redirect behind a test switch ("ui redirect on|off", default off) and a capture of the UI
// target ("grabui <path>", RDE1 like the eye grab) to learn how the UI's blending leaves alpha.

#include <d3d12.h>
#include <cstdint>
#include <string>

namespace rdrvr::ui_layer {

void init();  // installs the RTV substitution and a frame-end listener
// The UI target is made from the game's post output at the first frame end with the redirect on (the session's start).
void set_redirect(bool on);
// xr's live-resize guard: the game made its frame again (its new targets may take the old views' handles), so the
// redirect is off for this start and set_redirect(true) keeps it off; the UI stays in the game's own image.
void stop(const char* why);
bool stopped();
std::string grab(const std::string& path, unsigned timeout_ms = 3000);
// The game's alpha-only overwrites (write mask alpha, ONE/ZERO: unseen on the game's own image, whose alpha nothing
// reads) set the UI target's coverage: they give the radar ring's lines theirs, but also zero it over what the UI drew
// before (2026-10-16 capture of the game's own HUD: the health meter's red fill is in the UI target's colour, alpha 0,
// so it showed as thin outlines). The pass's draw log ("pso uidraws"): alpha-only draws write a mask (the meters' arcs),
// then the meters' wedges are drawn through it (colour DestAlpha/InvDestAlpha), then more alpha-only draws. So the
// alpha-only draws before the pass's first DestAlpha draw run as the game's (the mask cut exactly; with MAX the whole
// wedges showed in the radar), those after it are swapped for a copy that keeps the larger alpha (blend MAX; pso.cpp),
// so they no longer erase the fill. "ui alphawrites max|keep" (test; max by default).
bool on_ui_target(ID3D12GraphicsCommandList* cl);  // recording thread: this list's render target is the UI target now
bool drop_alpha_writes();
void set_drop_alpha_writes(bool on);
void count_alpha_write_dropped();
// "ui dual on|off" (on by default since 2026-10-16: the user judged it right in the headset): the UI also drawn onto a white target (~100 MB at 4778x2688), each draw
// twice with the game's own pipelines, the composite from both (ui_layer.cpp); the MAX copies are not used then.
void set_dual(bool on);
bool dual_active();
bool dual_draw_begin(ID3D12GraphicsCommandList* cl);  // recording thread, before a draw: the white target bound, draw now
void dual_draw_end(ID3D12GraphicsCommandList* cl);    // then the UI target bound again for the game's own draw
void note_mask_read();  // recording thread: this UI pass drew through the target's alpha (DestAlpha blend)
bool mask_read();
void status_text(char* out, size_t len);
// Step 2. While the redirect is on, each frame's UI is also drawn back onto the monitor's back buffer (blended in
// display space, the game's Gamma applied), so the desktop keeps its HUD. For the XR quad layer (presenting thread,
// xr.cpp's frame end): whether this frame drew any UI, the UI target's size, and the draw into an acquired sRGB quad
// image (linear premultiplied, coverage = sqrt(stored alpha)).
bool has_ui();
void size(uint32_t* w, uint32_t* h);
// The whole UI into `dst`, without the UI pixels in `hole` (x0 y0 x1 y1; the wrist HUD's part) when given. Texel for
// texel when `dst` is the UI's size; smaller (the quad's swapchain capped), box-filtered so text does not alias.
bool draw_quad(ID3D12Resource* dst, DXGI_FORMAT fmt, const int hole[4] = nullptr, const int hole2[4] = nullptr);
// The wrist HUD: the UI's cw x ch pixels from (x0, y0) into all of `dst` (texel for texel at that size, else filtered).
// which: 0 the wrist HUD's image (its own list and view), 1 the hand-placed wheel's (run 8 item 5)
bool draw_crop(ID3D12Resource* dst, DXGI_FORMAT fmt, int x0, int y0, uint32_t cw, uint32_t ch, int which = 0);

}  // namespace rdrvr::ui_layer
