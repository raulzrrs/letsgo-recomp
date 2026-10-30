"""patch_sdk.py <sdk root> (tools/build_ffx_android.sh): the FidelityFX SDK 1.1.4 sources made to build with the Android NDK"""
import sys, os
R = sys.argv[1]
def sub(path, old, new):
    p = os.path.join(R, path)
    t = open(p, encoding='utf-8', newline='').read()
    crlf = '\r\n' in t
    t = t.replace('\r\n', '\n')
    if new in t: print('already', path); return
    if old not in t: sys.exit('not found in %s: %r' % (path, old[:60]))
    t = t.replace(old, new, 1)
    open(p, 'w', encoding='utf-8', newline='').write(t.replace('\n', '\r\n') if crlf else t)
    print('patched', path)
sub('sdk/src/backends/vk/ffx_vk.cpp',
    'std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> converter;\n    wcscpy_s(outputBuffer, outputLen, converter.from_bytes(inputName).c_str());',
    'wcscpy_s(outputBuffer, outputLen, ffx_android::from_utf8(inputName).c_str());')
sub('sdk/src/backends/vk/ffx_vk.cpp',
    'std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> converter;\n    strcpy_s(outputBuffer, outputLen, converter.to_bytes(inputName).c_str());',
    'strcpy_s(outputBuffer, outputLen, ffx_android::to_utf8(inputName).c_str());')
# DX12-only: the driver's external providers (amdxc64)
sub('ffx-api/src/ffx_provider.cpp', '#include <d3d12.h>\n',
    '#ifdef _WIN32\n#include <d3d12.h>\n#else\nstruct ID3D12Device;\n#endif\n')
sub('ffx-api/src/ffx_provider.cpp', 'void GetExternalProviders(ID3D12Device* device, uint64_t descType)\n{\n',
    'void GetExternalProviders(ID3D12Device* device, uint64_t descType)\n{\n#ifndef _WIN32\n    (void)device, (void)descType;\n    return;\n#else\n')
sub('ffx-api/src/ffx_provider.cpp', '            }\n        }\n    }\n    \n}\n',
    '            }\n        }\n    }\n#endif\n}\n')
sub('ffx-api/src/ffx_provider.cpp', 'static constexpr ffxProvider* providers[] = {\n    &ffxProvider_FSR3Upscale::Instance,\n',
    'static constexpr ffxProvider* providers[] = {\n    &ffxProvider_FSR3Upscale::Instance,\n#ifdef _WIN32  // (Android: the FSR 3.1 upscaler alone)\n')
sub('ffx-api/src/ffx_provider.cpp', '#endif // FFX_BACKEND_VK\n};\n', '#endif // FFX_BACKEND_VK\n#endif // _WIN32\n};\n')
sub('ffx-api/src/ffx_provider.cpp', 'MIDL_INTERFACE("b58d6601-7401-4234-8180-6febfc0e484c")\nIAmdExtFfxApi : public IUnknown\n{\npublic:\n    // Update FFX API provider\n    virtual HRESULT UpdateFfxApiProvider(void* pData, uint32_t dataSizeInBytes) = 0;\n};\n',
    '#ifdef _WIN32\nMIDL_INTERFACE("b58d6601-7401-4234-8180-6febfc0e484c")\nIAmdExtFfxApi : public IUnknown\n{\npublic:\n    // Update FFX API provider\n    virtual HRESULT UpdateFfxApiProvider(void* pData, uint32_t dataSizeInBytes) = 0;\n};\n#endif\n')
# wchar_t is 4 bytes on Android: the component's private context (resource names in wchar_t) outgrows the public size
sub('sdk/include/FidelityFX/host/ffx_fsr3upscaler.h', '#define FFX_FSR3UPSCALER_CONTEXT_SIZE (FFX_SDK_DEFAULT_CONTEXT_SIZE)',
    '#ifdef _WIN32\n#define FFX_FSR3UPSCALER_CONTEXT_SIZE (FFX_SDK_DEFAULT_CONTEXT_SIZE)\n#else\n#define FFX_FSR3UPSCALER_CONTEXT_SIZE (FFX_SDK_DEFAULT_CONTEXT_SIZE * 2)\n#endif')
# the frame generation's swap chain (FrameInterpolationSwapchainVK.cpp) is not built: the upscaler never configures it
sub('sdk/src/backends/vk/ffx_vk.cpp', '    backendInterface->fpSwapChainConfigureFrameGeneration = ffxSetFrameGenerationConfigToSwapchainVK;',
    '    backendInterface->fpSwapChainConfigureFrameGeneration = nullptr;  // (Android: the FSR 3.1 upscaler alone)')
