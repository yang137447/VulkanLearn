# AMD TressFX

The TFX format interpretation and static strand preparation refer to:

- Repository: GPUOpenSoftware/UnrealEngine
- Branch: TressFX5-5.4.4
- Revision: 61f8dd9b51b88d41937ef8d73618aac778642f41
- Engine/Plugins/AMD/TressFX/Source/TressFXCore/Private/TressFXTFXFileHeader.h
- Engine/Plugins/AMD/TressFX/Source/TressFXCore/Private/TressFXAsset.cpp
- Tools/Maya2022/TressFX_Exporter.py
- UE544_TressFX_Engine_Patch.patch:
  Engine/Shaders/Private/TressFX/TressFXVertexFactory.ush

The adapted tangent, rest-length and ribbon-index algorithms in
`strandGeometry.cpp` and the view-aligned expansion adapted in
`shader/glsl/engine/tressfxVertexFactory.glsl` retain the following license.
No UE headers or serialized UE assets are included.

Copyright (c) 2016-2025 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
