
# RTGL1

  

  

RTGL1 is a library that aims to simplify the process of porting 3D applications to *real-time path tracing*, via hardware accelerated ray tracing, denoising algorithms (A-SVGF) and sampling algorithms (ReSTIR, ReSTIR GI) to improve the image quality by reusing spatio-temporal data.

  

This fork includes many improvements on the original repository, including FSR3 upscaling support, FSR3 framegen (eventually), newer versions of DLSS and many more.

  

It is intended to be used in addition to my fork of xash-rt, which also includes many changes and improvements to the overall experience.

  

## Improvements over original RTGL1

Removed FSR2 and added FSR3 support - FSR3 works on exactly the same devices, but with better performance and better visual quality.

FSR3 Framegen support

Ability to set color of brushes/textures marked with "isWater:true" with "waterColor:[xxx,xxx,xxx] and "Density" with "waterDensity:xx" - Mainly used for making water look dirty in areas that are supposed to have dirty water (ba_canal2, residue processing etc.)

Improved SPIRV shaders allowing for more accurate bloom, tonemapping, lightning etc.

nVidia NRD denoiser replacing SVGF Atrous.

## Build



1. Requirements:

* 64-bit CPU

* GPU with a ray tracing support

* [Git](https://github.com/git-for-windows/git/releases)

* [CMake](https://cmake.org/download/)

* [Vulkan SDK](https://vulkan.lunarg.com/)

* [Python 3](https://www.python.org/downloads/) (for building the shaders)

* [FSR3 SDK](https://gpuopen.com/fidelityfx-super-resolution-3/#:~:text=Download%20the%20latest%20version%20%2D%20v3.1.5)




1. Clone the repository

*  `git clone https://github.com/boofiboi/RayTracedGL1.git --recursive`
  
1. Extract the FSR3 sdk into Source/FSR3
2. 
3. Configure with CMake

* on Windows, with Visual Studio:

* open the folder as CMake project

* otherwise:

* specify windowing systems to build the library with, by enabling some of the CMake options:

*  `RG_WITH_SURFACE_WIN32`

*  `RG_WITH_SURFACE_METAL`

*  `RG_WITH_SURFACE_WAYLAND`

*  `RG_WITH_SURFACE_XCB`

*  `RG_WITH_SURFACE_XLIB`

* configure (Command defaults to WIN32)

```
cmake -S E:/Xash/RayTracedGL1 -B E:/Xash/RayTracedGL1/Build/x64-Release -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRG_WITH_NRD=ON -DRG_WITH_AMD_FSR3=ON -DRG_WITH_EXPORTS=ON -DRG_WITH_SHADERS=ON -DRG_WITH_IMGUI=ON -DRG_WITH_SURFACE_WIN32=ON -DSHADERMAKE_FIND_DXC=OFF -DSHADERMAKE_FIND_DXC_VK=ON

```

* Compile

*  `cmake --build E:/Xash/RayTracedGL1/Build/x64-Release -j` - Builds "Release with debug info" build.

  

### Notes:

  

* RTGL1 requires a set of blue noise images on start-up: `RgInstanceCreateInfo::pBlueNoiseFilePath`. A ready-to-use resource can be found here: `Tools/BlueNoise_LDR_RGBA_128.ktx2`

  

  

## Tools

  

  

### Shader development

  

  

RTGL1 supports shader hot-reloading (a target application sets `RgStartFrameInfo::requestShaderReload=true` in runtime).

  

  

But to ease the process of *building* the shaders, instead of running `GenerateShaders.py` from a terminal manually, you can install [Visual Studio Code](https://code.visualstudio.com/) and [Script Runner extension](https://marketplace.visualstudio.com/items?itemName=easterapps.script-runner) to it. Open `Sources/Shaders` folder, add such config to VS Code's `.json` settings file (TODO: VS Code workspace).

  

```

  

"script-runner.definitions": { "commands": [ { "identifier": "shaderBuild", "description": "Build shaders", "command": "cls; python .\\GenerateShaders.py -ps", "working_directory": "${workspaceFolder}", }, { "identifier": "shaderGenAndBuild", "description": "Build shaders with generating common files", "command": "cls; python .\\GenerateShaders.py -ps -g", "working_directory": "${workspaceFolder}", } ], },

  

```

  

Then assign hotkeys to `shaderBuild` and `shaderGenAndBuild` commands in `File->Preferences->Keyboard Shortcuts`.

  

  

### Textures

Some games don't have PBR materials, but to add them, RTGL1 provides 'texture overriding' functionality: application requests to upload an original texture and specifies its name, then RTGL1 tries to find files with such name (appending some suffixes, e.g. `_n` for normal maps, or none for albedo maps) and loads them instead of original ones. These files are in `.ktx2` format with a specific compression and contain image data.

  

To generate such textures:

Some games don't have PBR materials, but to add them, RTGL1 provides 'texture overriding' functionality: application requests to upload an original texture and specifies its name, then RTGL1 tries to find files with such name (appending some suffixes, e.g. `_n` for normal maps, or none for albedo maps) and loads them instead of original ones. These files are in `.ktx2` format with a specific compression and contain image data.

  

  

To generate such textures:

  

1. [Compressonator CLI](https://gpuopen.com/compressonator/) and `Python3` are required

  

1. Create a folder, put `Tools/CreateKTX2.py`, create folder named `Raw` and `Compressed`.

  

1. The script:

1. scans files (with `INPUT_EXTENSIONS`) in `Raw` folder

1. generates corresponding `.ktx2` file to `Compressed` folder, preserving the hierarchy

  

On RTGL1 initialization, `RgInstanceCreateInfo::pOverridenTexturesFolderPath` should contain a path to the `Compressed` folder.

1. scans files (with `INPUT_EXTENSIONS`) in `Raw` folder

  

1. generates corresponding `.ktx2` file to `Compressed` folder, preserving the hierarchy

  

  

On RTGL1 initialization, `RgInstanceCreateInfo::pOverridenTexturesFolderPath` should contain a path to the `Compressed` folder.

  

  

# Projects

  

* https://github.com/sultim-t/Serious-Engine-RT/releases

  

* https://github.com/sultim-t/prboom-plus-rt

  

* https://github.com/sultim-t/vkquake-rt

  

* https://github.com/sultim-t/xash-rt
