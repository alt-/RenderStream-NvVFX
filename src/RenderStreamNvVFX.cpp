// A Nvidia Maxine VideoEffects RenderStream application that receives and sends back textures using DX11
//
// Usage: Compile, copy the executable into your RenderStream Projects folder and launch via d3

#define WIN32_LEAN_AND_MEAN

#include <iostream>
#include <vector>
#include <windows.h>
#include <shlwapi.h>
#include <tchar.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl.h>
#include <unordered_map>
#include <sstream>

// auto-generated from hlsl
#include "Generated_Code/VertexShader.h"
#include "Generated_Code/PixelShader.h"

#include "../renderstream/d3renderstream.h"
#include "../nvvfx/include/nvVideoEffects.h"
#include "../nvvfx/include/nvTransferD3D11.h"

#if defined(UNICODE) || defined(_UNICODE)
#define tcout std::wcout
#define tcerr std::wcerr
#else
#define tcout std::cout
#define tcerr std::cerr
#endif

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "d3d11.lib")

struct ScopedSchema
{
    ScopedSchema()
    {
        clear();
    }
    ~ScopedSchema()
    {
        reset();
    }
    void reset()
    {
        for (size_t i = 0; i < schema.channels.nChannels; ++i)
            free(const_cast<char*>(schema.channels.channels[i]));
        free(schema.channels.channels);
        for (size_t i = 0; i < schema.scenes.nScenes; ++i)
        {
            RemoteParameters& scene = schema.scenes.scenes[i];
            free(const_cast<char*>(scene.name));
            for (size_t j = 0; j < scene.nParameters; ++j)
            {
                RemoteParameter& parameter = scene.parameters[j];
                free(const_cast<char*>(parameter.group));
                free(const_cast<char*>(parameter.displayName));
                free(const_cast<char*>(parameter.key));
                if (parameter.type == RS_PARAMETER_TEXT)
                    free(const_cast<char*>(parameter.defaults.text.defaultValue));
                for (size_t k = 0; k < parameter.nOptions; ++k)
                {
                    free(const_cast<char*>(parameter.options[k]));
                }
                free(parameter.options);
            }
            free(scene.parameters);
        }
        free(schema.scenes.scenes);
        clear();
    }

    ScopedSchema(const ScopedSchema&) = delete;
    ScopedSchema(ScopedSchema&& other)
    {
        schema = std::move(other.schema);
        other.reset();
    }
    ScopedSchema& operator=(const ScopedSchema&) = delete;
    ScopedSchema& operator=(ScopedSchema&& other)
    {
        schema = std::move(other.schema);
        other.reset();
        return *this;
    }

    Schema schema;

private:
    void clear()
    {
        schema.channels.nChannels = 0;
        schema.channels.channels = nullptr;
        schema.scenes.nScenes = 0;
        schema.scenes.scenes = nullptr;
    }
};

// Load renderstream DLL from disguise software's install path
HMODULE loadRenderStream()
{
    HKEY hKey;
    if (FAILED(RegOpenKeyEx(HKEY_CURRENT_USER, TEXT("Software\\d3 Technologies\\d3 Production Suite"), 0, KEY_READ, &hKey)))
    {
        tcerr << "Failed to open 'Software\\d3 Technologies\\d3 Production Suite' registry key" << std::endl;
        return nullptr;
    }

    TCHAR buffer[512];
    DWORD bufferSize = sizeof(buffer);
    if (FAILED(RegQueryValueEx(hKey, TEXT("exe path"), 0, nullptr, reinterpret_cast<LPBYTE>(buffer), &bufferSize)))
    {
        tcerr << "Failed to query value of 'exe path'" << std::endl;
        return nullptr;
    }

    if (!PathRemoveFileSpec(buffer))
    {
        tcerr << "Failed to remove file spec from path: " << buffer << std::endl;
        return nullptr;
    }

    if (_tcscat_s(buffer, bufferSize, TEXT("\\d3renderstream.dll")) != 0)
    {
        tcerr << "Failed to append filename to path: " << buffer << std::endl;
        return nullptr;
    }

    HMODULE hLib = ::LoadLibraryEx(buffer, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!hLib)
    {
        tcerr << "Failed to load dll: " << buffer << std::endl;
        return nullptr;
    }
    return hLib;
}

// Get streams into (descMem) buffer and return a pointer into it
const StreamDescriptions* getStreams(decltype(rs_getStreams)* rs_getStreams, std::vector<uint8_t>& descMem)
{
    uint32_t nBytes = 0;
    rs_getStreams(nullptr, &nBytes);

    const static int MAX_TRIES = 3;
    int iterations = 0;

    RS_ERROR res = RS_ERROR_BUFFER_OVERFLOW;
    do
    {
        descMem.resize(nBytes);
        res = rs_getStreams(reinterpret_cast<StreamDescriptions*>(descMem.data()), &nBytes);

        if (res == RS_ERROR_SUCCESS)
            break;

        ++iterations;
    } while (res == RS_ERROR_BUFFER_OVERFLOW && iterations < MAX_TRIES);

    if (res != RS_ERROR_SUCCESS)
        throw std::runtime_error("Failed to get streams");

    if (nBytes < sizeof(StreamDescriptions))
        throw std::runtime_error("Invalid stream descriptions");

    return reinterpret_cast<const StreamDescriptions*>(descMem.data());
}

struct Vertex
{
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT2 uv;
};

static constexpr Vertex quadVertices[] =
{
    { DirectX::XMFLOAT3(-1.f, 1.f, 0.5f), DirectX::XMFLOAT2(0.0f, 0.0f) },
    { DirectX::XMFLOAT3(1.f, 1.f, 0.5f), DirectX::XMFLOAT2(1.0f, 0.0f) },
    { DirectX::XMFLOAT3(-1.f, -1.f, 0.5f), DirectX::XMFLOAT2(0.0f, 1.0f) },
    { DirectX::XMFLOAT3(1.f, -1.f, 0.5f), DirectX::XMFLOAT2(1.0f, 1.0f) },
};

struct ConstantBufferStruct 
{
    uint32_t iTechnique;
    uint8_t padding[16-sizeof(iTechnique)];
};

struct Texture
{
    uint32_t width = 0;
    uint32_t height = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> resource;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    std::shared_ptr<NvCVImage> image;
};

Texture createTexture(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format)
{
    Texture texture;
    texture.width = width;
    texture.height = height;

    D3D11_TEXTURE2D_DESC rtDesc;
    ZeroMemory(&rtDesc, sizeof(D3D11_TEXTURE2D_DESC));
    rtDesc.Width = texture.width;
    rtDesc.Height = texture.height;
    rtDesc.MipLevels = 1;
    rtDesc.ArraySize = 1;
    rtDesc.Format = format;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Usage = D3D11_USAGE_DEFAULT;
    rtDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    rtDesc.CPUAccessFlags = 0;
    rtDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    if (FAILED(device->CreateTexture2D(&rtDesc, nullptr, texture.resource.GetAddressOf())))
        throw std::runtime_error("Failed to create texture for image parameter");

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    ZeroMemory(&srvDesc, sizeof(D3D11_SHADER_RESOURCE_VIEW_DESC));
    srvDesc.Format = rtDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = rtDesc.MipLevels;
    if (FAILED(device->CreateShaderResourceView(texture.resource.Get(), &srvDesc, texture.srv.GetAddressOf())))
        throw std::runtime_error("Failed to create shader resource view for image parameter");

    texture.image = std::make_shared<NvCVImage>();
    if (NvCVImage_InitFromD3D11Texture(texture.image.get(), texture.resource.Get()) != NVCV_SUCCESS)
        throw std::runtime_error("Failed to create Nvidia CV image for image parameter");

    return texture;
}

enum class NVVFXMode : uint32_t
{
    Quality = 0,
    Performance = 1
};

NvVFX_Handle createEffect(NvVFX_EffectSelector effectName, CUstream stream)
{
    const std::string displayName = std::string(effectName);

    NvVFX_Handle effect;
    if (NvVFX_CreateEffect(effectName, &effect) != NVCV_SUCCESS)
    {
        throw std::runtime_error("Failed to create " + displayName + " effect");
    }

    const char* cstr;
    if (NvVFX_GetString(effect, NVVFX_INFO, &cstr) == NVCV_SUCCESS)
    {
        tcout << displayName.c_str() << " effect info:\n" << cstr << std::endl;
    }

    if (NvVFX_SetCudaStream(effect, NVVFX_CUDA_STREAM, stream)  != NVCV_SUCCESS)
    {
        throw std::runtime_error("Failed to set Cuda stream on " + displayName + " effect");
    }

    return effect;
}

decltype(rs_logToD3)* g_rs_logToD3 = nullptr;

void logToD3(const char* message)
{
    if (g_rs_logToD3)
        g_rs_logToD3(message);
}

int main(int argc, char** argv)
{
    HMODULE hLib = loadRenderStream();
    if (!hLib)
    {
        tcerr << "Failed to load RenderStream DLL" << std::endl;
        return 1;
    }

#define LOAD_FN(FUNC_NAME) \
    decltype(FUNC_NAME)* FUNC_NAME = reinterpret_cast<decltype(FUNC_NAME)>(GetProcAddress(hLib, #FUNC_NAME)); \
    if (!FUNC_NAME) { \
        tcerr << "Failed to get function " #FUNC_NAME " from DLL" << std::endl; \
        return 2; \
    }

    LOAD_FN(rs_registerLoggingFunc);
    LOAD_FN(rs_registerErrorLoggingFunc);
    LOAD_FN(rs_initialise);
    LOAD_FN(rs_initialiseGpGpuWithDX11Device);
    LOAD_FN(rs_saveSchema);
    LOAD_FN(rs_setSchema);
    LOAD_FN(rs_getStreams);
    LOAD_FN(rs_awaitFrameData);
    LOAD_FN(rs_getFrameParameters);
    LOAD_FN(rs_getFrameImageData);
    LOAD_FN(rs_getFrameImage);
    LOAD_FN(rs_getFrameText);
    LOAD_FN(rs_getFrameCamera);
    LOAD_FN(rs_sendFrame);
    LOAD_FN(rs_shutdown);
    LOAD_FN(rs_logToD3);
    LOAD_FN(rs_setNewStatusMessage);

    g_rs_logToD3 = rs_logToD3;
    rs_registerLoggingFunc(logToD3);
    rs_registerErrorLoggingFunc(logToD3);

    if (rs_initialise(RENDER_STREAM_VERSION_MAJOR, RENDER_STREAM_VERSION_MINOR) != RS_ERROR_SUCCESS)
    {
        tcerr << "Failed to initialise RenderStream" << std::endl;
        return 3;
    }

#ifdef _DEBUG
    const uint32_t deviceFlags = D3D11_CREATE_DEVICE_DEBUG;
#else
    const uint32_t deviceFlags = 0;
#endif
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags, nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, context.GetAddressOf())))
    {
        tcerr << "Failed to initialise DirectX 11" << std::endl;
        rs_shutdown();
        return 4;
    }

    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
    {
        CD3D11_BUFFER_DESC vertexDesc(sizeof(quadVertices), D3D11_BIND_VERTEX_BUFFER);
        D3D11_SUBRESOURCE_DATA vertexData;
        ZeroMemory(&vertexData, sizeof(D3D11_SUBRESOURCE_DATA));
        vertexData.pSysMem = quadVertices;
        vertexData.SysMemPitch = 0;
        vertexData.SysMemSlicePitch = 0;
        if (FAILED(device->CreateBuffer(&vertexDesc, &vertexData, vertexBuffer.GetAddressOf())))
        {
tcerr << "Failed to initialise DirectX 11: vertex buffer" << std::endl;
rs_shutdown();
return 41;
        }
    }

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
    {
        if (FAILED(device->CreateVertexShader(VertexShaderBlob, std::size(VertexShaderBlob), nullptr, vertexShader.GetAddressOf())))
        {
            tcerr << "Failed to initialise DirectX 11: vertex shader" << std::endl;
            rs_shutdown();
            return 43;
        }
    }
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
    {
        D3D11_INPUT_ELEMENT_DESC inputElementDesc[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 }
        };

        if (FAILED(device->CreateInputLayout(inputElementDesc, ARRAYSIZE(inputElementDesc), VertexShaderBlob, std::size(VertexShaderBlob), inputLayout.GetAddressOf())))
        {
            tcerr << "Failed to initialise DirectX 11: index buffer" << std::endl;
            rs_shutdown();
            return 44;
        }
    }
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
    {
        if (FAILED(device->CreatePixelShader(PixelShaderBlob, std::size(PixelShaderBlob), nullptr, pixelShader.GetAddressOf())))
        {
            tcerr << "Failed to initialise DirectX 11: pixel shader" << std::endl;
            rs_shutdown();
            return 45;
        }
    }
    Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer;
    {
        CD3D11_BUFFER_DESC constantBufferDesc(sizeof(ConstantBufferStruct), D3D11_BIND_CONSTANT_BUFFER);
        if (FAILED(device->CreateBuffer(&constantBufferDesc, nullptr, constantBuffer.GetAddressOf())))
        {
            tcerr << "Failed to initialise DirectX 11: constant buffer" << std::endl;
            rs_shutdown();
            return 46;
        }
    }

    if (rs_initialiseGpGpuWithDX11Device(device.Get()) != RS_ERROR_SUCCESS)
    {
        tcerr << "Failed to initialise RenderStream GPGPU interop" << std::endl;
        rs_shutdown();
        return 5;
    }

    CUstream cuStream;
    if (NvVFX_CudaStreamCreate(&cuStream) != NVCV_SUCCESS)
    {
        tcerr << "Failed to create Nvidia Maxine VFX Cuda stream" << std::endl;
        rs_shutdown();
        return 51;
    }
    struct Effect
    {
        std::string name;
        NvCVImage_PixelFormat inputPixelFormat;
        NvCVImage_ComponentType inputComponentType;
        unsigned char inputLayout;
        DXGI_FORMAT outputTextureFormat;
        NvCVImage_PixelFormat outputPixelFormat;
        NvCVImage_ComponentType outputComponentType;
        unsigned char outputLayout;
        NvVFX_Handle effect;
        bool upscale;
        int shaderTechnique;
        bool loaded = false;
    };
    std::vector<Effect> effects;
    try
    {
        effects.push_back({
            /*.name = */ "Transfer",
            /*.inputPixelFormat = */ NVCV_BGR,
            /*.inputComponentType = */ NVCV_U8,
            /*.inputLayout = */ NVCV_CHUNKY,
            /*.outputTextureFormat = */ DXGI_FORMAT_B8G8R8A8_UNORM,
            /*.outputPixelFormat = */ NVCV_BGR,
            /*.outputComponentType = */ NVCV_U8,
            /*.outputLayout = */ NVCV_CHUNKY,
            /*.effect = */ createEffect(NVVFX_FX_TRANSFER, cuStream),
            /*.upscale = */ false,
            /*.shaderTechnique = */ 0,
        });
        effects.push_back({
            /*.name = */ "Green screen",
            /*.inputPixelFormat = */ NVCV_BGR,
            /*.inputComponentType = */ NVCV_U8,
            /*.inputLayout = */ NVCV_CHUNKY,
            /*.outputTextureFormat = */ DXGI_FORMAT_A8_UNORM,
            /*.outputPixelFormat = */ NVCV_A,
            /*.outputComponentType = */ NVCV_U8,
            /*.outputLayout = */ NVCV_CHUNKY,
            /*.effect = */ createEffect(NVVFX_FX_GREEN_SCREEN, cuStream),
            /*.upscale = */ false,
            /*.shaderTechnique = */ 1,
        });
        effects.push_back({
            /*.name = */ "Artifact reduction",
            /*.inputPixelFormat = */ NVCV_BGR,
            /*.inputComponentType = */ NVCV_F32,
            /*.inputLayout = */ NVCV_PLANAR,
            /*.outputTextureFormat = */ DXGI_FORMAT_B8G8R8A8_UNORM,
            /*.outputPixelFormat = */ NVCV_BGR,
            /*.outputComponentType = */ NVCV_F32,
            /*.outputLayout = */ NVCV_PLANAR,
            /*.effect = */ createEffect(NVVFX_FX_ARTIFACT_REDUCTION, cuStream),
            /*.upscale = */ false,
            /*.shaderTechnique = */ 0,
        });
        if (NvVFX_SetU32(effects.back().effect, NVVFX_STRENGTH, 1) != NVCV_SUCCESS)
        {
            throw std::runtime_error("Failed to set strength on " + effects.back().name + " effect");
        }
        effects.push_back({
            /*.name = */ "Super resolution",
            /*.inputPixelFormat = */ NVCV_BGR,
            /*.inputComponentType = */ NVCV_F32,
            /*.inputLayout = */ NVCV_PLANAR,
            /*.outputTextureFormat = */ DXGI_FORMAT_B8G8R8A8_UNORM,
            /*.outputPixelFormat = */ NVCV_BGR,
            /*.outputComponentType = */ NVCV_F32,
            /*.outputLayout = */ NVCV_PLANAR,
            /*.effect = */ createEffect(NVVFX_FX_SUPER_RES, cuStream),
            /*.upscale = */ true,
            /*.shaderTechnique = */ 0,
        });
        if (NvVFX_SetU32(effects.back().effect, NVVFX_STRENGTH, 1) != NVCV_SUCCESS)
        {
            throw std::runtime_error("Failed to set strength on " + effects.back().name + " effect");
        }
        effects.push_back({
            /*.name = */ "Upscale",
            /*.inputPixelFormat = */ NVCV_RGBA,
            /*.inputComponentType = */ NVCV_U8,
            /*.inputLayout = */ NVCV_CHUNKY,
            /*.outputTextureFormat = */ DXGI_FORMAT_R8G8B8A8_UNORM,
            /*.outputPixelFormat = */ NVCV_RGBA,
            /*.outputComponentType = */ NVCV_U8,
            /*.outputLayout = */ NVCV_CHUNKY,
            /*.effect = */ createEffect(NVVFX_FX_SR_UPSCALE, cuStream),
            /*.upscale = */ true,
            /*.shaderTechnique = */ 0,
        });
    }
    catch (const std::exception& e)
    {
        tcerr << e.what() << std::endl;
        NvVFX_CudaStreamDestroy(cuStream);
        rs_shutdown();
        return 52;
    }

    ScopedSchema scoped; // C++ helper that cleans up mallocs and strdups
    scoped.schema.scenes.nScenes = uint32_t(effects.size());
    scoped.schema.scenes.scenes = static_cast<RemoteParameters*>(malloc(scoped.schema.scenes.nScenes * sizeof(RemoteParameters)));
    for (size_t i = 0; i < effects.size(); ++i)
    {
        scoped.schema.scenes.scenes[i].name = _strdup(effects[i].name.c_str());
        scoped.schema.scenes.scenes[i].nParameters = 1;
        scoped.schema.scenes.scenes[i].parameters = static_cast<RemoteParameter*>(malloc(scoped.schema.scenes.scenes[i].nParameters * sizeof(RemoteParameter)));
        // Image parameter
        scoped.schema.scenes.scenes[i].parameters[0].group = _strdup("Inputs");
        scoped.schema.scenes.scenes[i].parameters[0].key = _strdup("image_param1");
        scoped.schema.scenes.scenes[i].parameters[0].displayName = _strdup("Texture");
        scoped.schema.scenes.scenes[i].parameters[0].type = RS_PARAMETER_IMAGE;
        scoped.schema.scenes.scenes[i].parameters[0].nOptions = 0;
        scoped.schema.scenes.scenes[i].parameters[0].options = nullptr;
        scoped.schema.scenes.scenes[i].parameters[0].dmxOffset = -1; // Auto
        scoped.schema.scenes.scenes[i].parameters[0].dmxType = 2; // Dmx8 = 0, Dmx16BigEndian = 2
    }
    if (rs_setSchema(&scoped.schema) != RS_ERROR_SUCCESS)
    {
        tcerr << "Failed to set schema" << std::endl;
        for (Effect& effect : effects)
            NvVFX_DestroyEffect(effect.effect);
        NvVFX_CudaStreamDestroy(cuStream);
        rs_shutdown();
        return 6;
    }

    // Saving the schema to disk makes the remote parameters available in d3's UI before the application is launched
    if (rs_saveSchema(argv[0], &scoped.schema) != RS_ERROR_SUCCESS)
    {
        tcerr << "Failed to save schema" << std::endl;
        for (Effect& effect : effects)
            NvVFX_DestroyEffect(effect.effect);
        NvVFX_CudaStreamDestroy(cuStream);
        rs_shutdown();
        return 61;
    }
    std::vector<uint8_t> descMem;
    const StreamDescriptions* header = nullptr;
    struct RenderTarget
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> view;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> depth;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthView;
    };
    std::unordered_map<StreamHandle, RenderTarget> renderTargets;
    Texture input;
    std::shared_ptr<NvCVImage> effectInput;
    Texture output;
    std::shared_ptr<NvCVImage> effectOutput;
    std::shared_ptr<NvCVImage> outputImage;
    std::shared_ptr<NvCVImage> temporary = std::make_shared<NvCVImage>();
    FrameData frameData;
    uint32_t lastScene = 0;
    while (true)
    {
        // Wait for a frame request
        RS_ERROR err = rs_awaitFrameData(5000, &frameData);
        if (err == RS_ERROR_STREAMS_CHANGED)
        {
            try
            {
                header = getStreams(rs_getStreams, descMem);
                // Create render targets for all streams
                const size_t numStreams = header ? header->nStreams : 0;
                for (size_t i = 0; i < numStreams; ++i)
                {
                    const StreamDescription& description = header->streams[i];
                    RenderTarget& target = renderTargets[description.handle];

                    D3D11_TEXTURE2D_DESC rtDesc;
                    ZeroMemory(&rtDesc, sizeof(D3D11_TEXTURE2D_DESC));
                    rtDesc.Width = description.width;
                    rtDesc.Height = description.height;
                    rtDesc.MipLevels = 1;
                    rtDesc.ArraySize = 1;
                    rtDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    rtDesc.SampleDesc.Count = 1;
                    rtDesc.Usage = D3D11_USAGE_DEFAULT;
                    rtDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                    rtDesc.CPUAccessFlags = 0;
                    rtDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
                    if (FAILED(device->CreateTexture2D(&rtDesc, nullptr, target.texture.GetAddressOf())))
                        throw std::runtime_error("Failed to create render target texture for stream");

                    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
                    ZeroMemory(&rtvDesc, sizeof(D3D11_RENDER_TARGET_VIEW_DESC));
                    rtvDesc.Format = rtDesc.Format;
                    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                    if (FAILED(device->CreateRenderTargetView(target.texture.Get(), &rtvDesc, target.view.GetAddressOf())))
                        throw std::runtime_error("Failed to create render target view for stream");

                    D3D11_TEXTURE2D_DESC dsDesc;
                    ZeroMemory(&dsDesc, sizeof(D3D11_TEXTURE2D_DESC));
                    dsDesc.Width = description.width;
                    dsDesc.Height = description.height;
                    dsDesc.MipLevels = 1;
                    dsDesc.ArraySize = 1;
                    dsDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
                    dsDesc.SampleDesc.Count = 1;
                    dsDesc.Usage = D3D11_USAGE_DEFAULT;
                    dsDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
                    dsDesc.CPUAccessFlags = 0;
                    if (FAILED(device->CreateTexture2D(&dsDesc, nullptr, target.depth.GetAddressOf())))
                        throw std::runtime_error("Failed to create depth texture for stream");

                    D3D11_DEPTH_STENCIL_VIEW_DESC  dsvDesc;
                    ZeroMemory(&dsvDesc, sizeof(D3D11_DEPTH_STENCIL_VIEW_DESC));
                    dsvDesc.Format = dsDesc.Format;
                    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                    if (FAILED(device->CreateDepthStencilView(target.depth.Get(), &dsvDesc, target.depthView.GetAddressOf())))
                        throw std::runtime_error("Failed to create depth view for stream");
                }
            }
            catch (const std::exception& e)
            {
                tcerr << e.what() << std::endl;
                for (Effect& effect : effects)
                    NvVFX_DestroyEffect(effect.effect);
                NvVFX_CudaStreamDestroy(cuStream);
                rs_shutdown();
                return 7;
            }
            tcout << "Found " << (header ? header->nStreams : 0) << " streams" << std::endl;
            continue;
        }
        else if (err == RS_ERROR_TIMEOUT)
        {
            continue;
        }
        else if (err != RS_ERROR_SUCCESS)
        {
            tcerr << "rs_awaitFrameData returned " << err << std::endl;
            break;
        }

        if (frameData.scene >= scoped.schema.scenes.nScenes)
        {
            rs_logToD3("Scene out of bounds\n");
            continue;
        }

        const auto& scene = scoped.schema.scenes.scenes[frameData.scene];
        Effect& effect = effects[frameData.scene];

        ImageFrameData image;
        if (rs_getFrameImageData(scene.hash, &image, 1) != RS_ERROR_SUCCESS)
        {
            rs_logToD3("Failed to get image parameter data\n");;
            continue;
        }
        if (input.width != image.width || input.height != image.height || frameData.scene != lastScene)
        {
            input = createTexture(device.Get(), image.width, image.height, DXGI_FORMAT_B8G8R8A8_UNORM);
            effectInput = std::make_shared<NvCVImage>(image.width, image.height, effect.inputPixelFormat, effect.inputComponentType, effect.inputLayout, NVCV_GPU, effect.inputLayout == NVCV_PLANAR ? 1 : 32);
        }

        SenderFrameTypeData data;
        data.dx11.resource = input.resource.Get();

        if (rs_getFrameImage(image.imageId, RS_FRAMETYPE_DX11_TEXTURE, data) != RS_ERROR_SUCCESS)
        {
            rs_logToD3("Failed to get image parameter\n");
            continue;
        }

        bool success = true;
        if (NvCVImage_MapResource(input.image.get(), cuStream) != NVCV_SUCCESS)
        {
            rs_logToD3("Failed to map input image\n");
            continue;
        }
        if (NvCVImage_Transfer(input.image.get(), effectInput.get(), 1/255.f, cuStream, temporary.get()) != NVCV_SUCCESS)
        {
            rs_logToD3("Failed to transfer input image\n");
            success = false;
        }
        if (NvCVImage_UnmapResource(input.image.get(), cuStream) != NVCV_SUCCESS)
        {
            rs_logToD3("Failed to unmap input image\n");
            continue;
        }
        if (!success)
            continue;

        if (NvVFX_SetImage(effect.effect, NVVFX_INPUT_IMAGE, effectInput.get()) != NVCV_SUCCESS)
        {
            rs_logToD3("Failed to set input image\n");
            continue;
        }

        // Run effect
        const uint32_t width = effect.upscale ? image.width * 2 : image.width;
        const uint32_t height = effect.upscale ? image.height * 2 : image.height;
        if (output.width != width || output.height != height || frameData.scene != lastScene)
        {
            output = createTexture(device.Get(), width, height, effect.outputTextureFormat);
            effectOutput = std::make_shared<NvCVImage>(width, height, effect.outputPixelFormat, effect.outputComponentType, effect.outputLayout, NVCV_GPU, effect.outputLayout == NVCV_PLANAR ? 1 : 32);

            // See if we need to manually transfer effect output as NvCVImage_Transfer is missing planar->DX11 conversions
            NvCVImage_PixelFormat outputPixelFormat;
            NvCVImage_ComponentType outputComponentType;
            unsigned char outputLayout;
            if (NvCVImage_FromD3DFormat(effect.outputTextureFormat, &outputPixelFormat, &outputComponentType, &outputLayout) != NVCV_SUCCESS)
            {
                tcerr << "Failed to determine output image format" << std::endl;
                for (Effect& effect : effects)
                    NvVFX_DestroyEffect(effect.effect);
                NvVFX_CudaStreamDestroy(cuStream);
                rs_shutdown();
                return 84;
            }
            if (effect.outputPixelFormat != outputPixelFormat || effect.outputComponentType != outputComponentType || effect.outputLayout != outputLayout)
                outputImage = std::make_shared<NvCVImage>(width, height, outputPixelFormat, outputComponentType, outputLayout, NVCV_GPU, outputLayout == NVCV_PLANAR ? 1 : 32);
            else
                outputImage = effectOutput;
        }

        if (NvVFX_SetImage(effect.effect, NVVFX_OUTPUT_IMAGE, effectOutput.get()) != NVCV_SUCCESS)
        {
            tcerr << "Failed to set output image" << std::endl;
            for (Effect& effect : effects)
                NvVFX_DestroyEffect(effect.effect);
            NvVFX_CudaStreamDestroy(cuStream);
            rs_shutdown();
            return 84;
        }

        if (!effect.loaded && NvVFX_Load(effect.effect) != NVCV_SUCCESS)
        {
            rs_logToD3("Failed to load model\n");
            continue;
        }
        effect.loaded = true;

        NvCV_Status status = NvVFX_Run(effect.effect, 0);
        if (status == NVCV_ERR_INITIALIZATION)
            effect.loaded = false; // Attempt reinitialisation
        if (status != NVCV_SUCCESS)
        {
            std::stringstream ss;
            ss << "Failed to run " << effect.name << " effect, status: " << status << "\n";
            rs_logToD3(ss.str().c_str());
            continue;
        }

        if (effectOutput != outputImage && NvCVImage_Transfer(effectOutput.get(), outputImage.get(), 255.f, cuStream, temporary.get()) != NVCV_SUCCESS)
        {
            rs_logToD3("Failed to transfer effect output to output image\n");
            continue;
        }

        success = true;
        if (NvCVImage_MapResource(output.image.get(), cuStream) != NVCV_SUCCESS)
        {
            rs_logToD3("Failed to map output image\n");
            continue;
        }
        if (NvCVImage_Transfer(outputImage.get(), output.image.get(), 1, cuStream, temporary.get()) != NVCV_SUCCESS)
        {
            rs_logToD3("Failed to transfer output image\n");
            success = false;
        }
        if (NvCVImage_UnmapResource(output.image.get(), cuStream) != NVCV_SUCCESS)
        {
            rs_logToD3("Failed to unmap output image\n");
            continue;
        }
        if (!success)
            continue;

        // Respond to frame request
        const size_t numStreams = header ? header->nStreams : 0;
        for (size_t i = 0; i < numStreams; ++i)
        {
            const StreamDescription& description = header->streams[i];

            CameraResponseData response;
            response.tTracked = frameData.tTracked;
            if (rs_getFrameCamera(description.handle, &response.camera) == RS_ERROR_SUCCESS)
            {
                const RenderTarget& target = renderTargets.at(description.handle);
                context->OMSetRenderTargets(1, target.view.GetAddressOf(), target.depthView.Get());

                const float clearColour[4] = { 0.f, 0.f, 0.f, 0.f };
                context->ClearRenderTargetView(target.view.Get(), clearColour);
                context->ClearDepthStencilView(target.depthView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

                D3D11_VIEWPORT viewport;
                ZeroMemory(&viewport, sizeof(D3D11_VIEWPORT));
                viewport.Width = static_cast<float>(description.width);
                viewport.Height = static_cast<float>(description.height);
                viewport.MinDepth = 0;
                viewport.MaxDepth = 1;
                context->RSSetViewports(1, &viewport);

                ConstantBufferStruct constantBufferData;
                constantBufferData.iTechnique = effect.shaderTechnique;
                context->UpdateSubresource(constantBuffer.Get(), 0, nullptr, &constantBufferData, 0, 0);

                // Draw fullscreen quad
                UINT stride = sizeof(Vertex);
                UINT offset = 0;
                context->IASetVertexBuffers(0, 1, vertexBuffer.GetAddressOf(), &stride, &offset);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                context->IASetInputLayout(inputLayout.Get());
                context->VSSetShader(vertexShader.Get(), nullptr, 0);
                context->PSSetShader(pixelShader.Get(), nullptr, 0);
                context->PSSetConstantBuffers(0, 1, constantBuffer.GetAddressOf());
                context->PSSetShaderResources(0, 1, input.srv.GetAddressOf());
                context->PSSetShaderResources(1, 1, output.srv.GetAddressOf());
                context->Draw(std::extent<decltype(quadVertices)>::value, 0);

                SenderFrameTypeData data;
                data.dx11.resource = target.texture.Get();
                if (rs_sendFrame(description.handle, RS_FRAMETYPE_DX11_TEXTURE, data, &response) != RS_ERROR_SUCCESS)
                {
                    tcerr << "Failed to send frame" << std::endl;
                    for (Effect& effect : effects)
                        NvVFX_DestroyEffect(effect.effect);
                    NvVFX_CudaStreamDestroy(cuStream);
                    rs_shutdown();
                    return 8;
                }
            }
        }
        lastScene = frameData.scene;
    }

    for (Effect& effect : effects)
        NvVFX_DestroyEffect(effect.effect);
    NvVFX_CudaStreamDestroy(cuStream);

    if (rs_shutdown() != RS_ERROR_SUCCESS)
    {
        tcerr << "Failed to shutdown RenderStream" << std::endl;
        return 99;
    }

    return 0;
}
