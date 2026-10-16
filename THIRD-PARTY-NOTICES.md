# Third-party notices

RDRVR's binaries are linked statically with these libraries (through vcpkg). Their licences apply to those parts.

| Library | Licence | Source |
|---|---|---|
| OpenXR SDK (loader and headers) | Apache-2.0 | https://github.com/KhronosGroup/OpenXR-SDK |
| SafetyHook | BSL-1.0 | https://github.com/cursey/safetyhook |
| Zydis / Zycore (SafetyHook's disassembler) | MIT | https://github.com/zyantific/zydis |
| Dear ImGui | MIT | https://github.com/ocornut/imgui |

This fork's release zip includes nvngx_dlss.dll (NVIDIA DLSS 310.9.1, unmodified and signed by NVIDIA), redistributed
under the NVIDIA RTX SDKs licence (https://github.com/NVIDIA/DLSS). NVIDIA and DLSS are trademarks of NVIDIA Corporation.

RDRVR.Gameplay.red links against RedHook's import library (RedHook SDK by K3rhos, MIT,
https://github.com/K3rhos/RedHookSDK). RedHook itself is not part of RDRVR and is not distributed with it.

Red Dead Redemption is a trademark of Take-Two Interactive Software. RDRVR is an unofficial fan project, not affiliated
with or endorsed by Rockstar Games or Take-Two Interactive. It contains no game files except RDRVR_shaders\ (this
fork's release only): the game's own shaders, converted for single-pass stereo, remain Rockstar Games' property.
