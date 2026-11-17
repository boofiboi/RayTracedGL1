<div align="center">

# RayTracedGL1 (RTGL1)

[![Engine](https://img.shields.io/badge/Engine-xash--rt_Improved-2E3440?logo=github&logoColor=white&style=for-the-badge)](https://github.com/boofiboi/xash-rt)
[![Language](https://img.shields.io/badge/Language-C%2B%2B-00599C?logo=cplusplus&logoColor=white&style=for-the-badge)](https://github.com/boofiboi/RayTracedGL1)
[![Graphics](https://img.shields.io/badge/Graphics-Vulkan-AC162C?logo=vulkan&logoColor=white&style=for-the-badge)](https://github.com/boofiboi/RayTracedGL1)
<br>
![Ray Tracing](https://img.shields.io/badge/Ray_Tracing-Path_Traced-E5A93C?logo=sparkles&logoColor=white&style=for-the-badge)
![AMD FSR](https://img.shields.io/badge/AMD-FSR_3_%2F_4-ED1C24?logo=amd&logoColor=white&style=for-the-badge)
![NVIDIA DLSS](https://img.shields.io/badge/NVIDIA-DLSS_4.5-76B900?logo=nvidia&logoColor=white&style=for-the-badge)

</div>

RTGL1 is a Vulkan-based rendering library designed to simplify porting 3D applications to **real-time path tracing** using hardware-accelerated ray tracing, spatio-temporal sampling (ReSTIR, ReSTIR GI), and modern denoising pipelines.

This fork introduces major graphical, upscaling, and platform improvements over the original repository and is intended to be paired with [Xash-RT Improved](https://github.com/boofiboi/xash-rt).

---

## Key Improvements

### Upscaling & Frame Generation
* **AMD FSR 3 & FSR 4:** Replaced FSR 2 with FSR 3 (supporting the exact same hardware with superior performance and image quality) and FSR 4 on supported GPUs, paired with FSR 3.1.6 Frame Generation.
* **NVIDIA DLSS 4.5:** Full support for DLSS 4.5 Upscaling and Frame Generation.

### Denoising & Shaders
* **NVIDIA Real-Time Denoisers (NRD):** Replaced the legacy A-SVGF denoiser with NRD for cleaner, more stable ray-traced output.
* **SPIR-V Shader Overhaul:** Improved SPIR-V shaders for more accurate bloom, tonemapping, and lighting.
* **Lottes CRT Shader:** Replaced the old NTSC scanline emulation shader with a proper Lottes CRT shader.

### Platform & Material Features
* **Native Linux Support:** Full support for Linux builds.
* **Per-Texture Water Semantics:** Customizable water properties for brushes/textures flagged with `"isWater": true`, allowing dirty or murky water in maps like `ba_canal2` and *Residue Processing*.

| Parameter | Type | Description |
| :--- | :--- | :--- |
| `waterColor` | `[R, G, B]` | The color white light becomes after passing through 1 meter of water. |
| `waterDensity` | `float` | Absorption multiplier (`1.0` = exact `waterColor` absorption, `0.0` = crystal clear). |
| `waterReflectivity` | `[0, 1]` | Multiplier controlling the Fresnel reflection strength of the water surface. |

*(Global fallback values for water surfaces without per-texture parameters are controlled via the engine's `rt_me_water_*` cvars.)*

---

## Building

Pre-compiled binaries are distributed directly through [Xash-RT Improved Releases](https://github.com/boofiboi/xash-rt/releases) and CI artifacts. Manual build instructions are not currently provided.