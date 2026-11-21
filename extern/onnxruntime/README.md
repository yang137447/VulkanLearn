# ONNX Runtime C API header

`include/onnxruntime_c_api.h` is copied from ONNX Runtime v1.22.0
(`ORT_API_VERSION 22`). Only the header is vendored; the runtime DLL is loaded
dynamically at process startup by `source/animation/motionBricks/onnxRuntimeApi.cpp`.

Runtime lookup:

1. `VULKANLEARN_ONNXRUNTIME_DLL` if set to an absolute or relative DLL path.
2. `onnxruntime.dll` on the normal Windows loader search path (executable
   directory, working directory, `PATH`).

The header, `LICENSE` and `ThirdPartyNotices.txt` retain their upstream MIT
license and notices. The runtime is not redistributed in this repository; a
package that ships it must include the same license files.
