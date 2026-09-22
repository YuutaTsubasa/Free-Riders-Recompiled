# Licence texts shipped with releases

Windows releases carry XenosRecomp's dxc-bin copy of the DirectX Shader
Compiler 1.8.2407 (`dxc.exe`, `dxcompiler.dll`, `dxil.dll`), byte-identical
to Microsoft's redistributable package `dxc_2024_07_31.zip` from
https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.8.2407.
dxc-bin has no licence file, so the package's own texts are kept here,
unmodified:

| File | From the package | Applies to |
| --- | --- | --- |
| `DirectXShaderCompiler-LLVM.txt` | `LICENSE-LLVM.txt` | `dxc.exe`, `dxcompiler.dll` |
| `DirectXShaderCompiler-MS.txt` | `LICENSE-MS.txt` | `dxil.dll` |

`scripts/package_release.py` copies them into each Windows release.
