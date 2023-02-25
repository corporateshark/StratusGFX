
#include <iostream>
#include <StratusLight.h>
#include "StratusGpuCommon.h"
#include "StratusPipeline.h"
#include "StratusRendererBackend.h"
#include <math.h>
#include <cmath>
#include <algorithm>
#include <random>
#include <numeric>
#include "StratusUtils.h"
#include "StratusMath.h"
#include "StratusLog.h"
#include "StratusResourceManager.h"
#include "StratusApplicationThread.h"
#include "StratusEngine.h"
#include "StratusWindow.h"
#include "StratusGraphicsDriver.h"

namespace stratus {
bool IsRenderable(const EntityPtr& p) {
    return p->Components().ContainsComponent<RenderComponent>();
}

bool IsLightInteracting(const EntityPtr& p) {
    auto component = p->Components().GetComponent<LightInteractionComponent>();
    return component.status == EntityComponentStatus::COMPONENT_ENABLED;
}

size_t GetMeshCount(const EntityPtr& p) {
    return p->Components().GetComponent<RenderComponent>().component->GetMeshCount();
}

static MeshPtr GetMesh(const EntityPtr& p, const size_t meshIndex) {
    return p->Components().GetComponent<RenderComponent>().component->GetMesh(meshIndex);
}

static MeshPtr GetMesh(const RenderMeshContainerPtr& p) {
    return p->render->GetMesh(p->meshIndex);
}

static MaterialPtr GetMeshMaterial(const RenderMeshContainerPtr& p) {
    return p->render->GetMaterialAt(p->meshIndex);
}

static const glm::mat4& GetMeshTransform(const RenderMeshContainerPtr& p) {
    return p->transform->transforms[p->meshIndex];
}

// See https://www.khronos.org/opengl/wiki/Debug_Output
void OpenGLDebugCallback(GLenum source, GLenum type, GLuint id,
                         GLenum severity, GLsizei length, const GLchar * message, const void * userParam) {
    if (severity == GL_DEBUG_SEVERITY_MEDIUM || severity == GL_DEBUG_SEVERITY_HIGH) {
       //std::cout << "[OpenGL] " << message << std::endl;
    }
}

RendererBackend::RendererBackend(const uint32_t width, const uint32_t height, const std::string& appName) {
    static_assert(sizeof(GpuVec) == 16, "Memory alignment must match up with GLSL");

    _isValid = true;

    // Set up OpenGL debug logging
    glEnable(GL_DEBUG_OUTPUT);
    glDebugMessageCallback(OpenGLDebugCallback, nullptr);

    const std::filesystem::path shaderRoot("../Source/Shaders");
    const ShaderApiVersion version{GraphicsDriver::GetConfig().majorVersion, GraphicsDriver::GetConfig().minorVersion};

    // Initialize the pipelines
    _state.geometry = std::unique_ptr<Pipeline>(new Pipeline(shaderRoot, version, {
        Shader{"pbr_geometry_pass.vs", ShaderType::VERTEX}, 
        Shader{"pbr_geometry_pass.fs", ShaderType::FRAGMENT}}));
    _state.shaders.push_back(_state.geometry.get());

    _state.forward = std::unique_ptr<Pipeline>(new Pipeline(shaderRoot, version, {
        Shader{"flat_forward_pass.vs", ShaderType::VERTEX}, 
        Shader{"flat_forward_pass.fs", ShaderType::FRAGMENT}}));
    _state.shaders.push_back(_state.forward.get());

    using namespace std;

    _state.skybox = std::unique_ptr<Pipeline>(new Pipeline(shaderRoot, version, {
        Shader{"skybox.vs", ShaderType::VERTEX}, 
        Shader{"skybox.fs", ShaderType::FRAGMENT}}));
    _state.shaders.push_back(_state.skybox.get());

    // Set up the hdr/gamma postprocessing shader

    _state.hdrGamma = std::unique_ptr<Pipeline>(new Pipeline(shaderRoot, version, {
        Shader{"hdr.vs", ShaderType::VERTEX},
        Shader{"hdr.fs", ShaderType::FRAGMENT}}));
    _state.shaders.push_back(_state.hdrGamma.get());

    // Set up the shadow preprocessing shaders
    for (int i = 0; i < 6; ++i) {
        _state.shadows.push_back(std::unique_ptr<Pipeline>(new Pipeline(shaderRoot, version, {
            Shader{"shadow.vs", ShaderType::VERTEX},
            //Shader{"shadow.gs", ShaderType::GEOMETRY},
            Shader{"shadow.fs", ShaderType::FRAGMENT}},
            {{"DEPTH_LAYER", std::to_string(i)}}))
        );
        _state.shaders.push_back(_state.shadows[i].get());

        _state.vplShadows.push_back(std::unique_ptr<Pipeline>(new Pipeline(shaderRoot, version, {
            Shader{"shadow.vs", ShaderType::VERTEX},
            //Shader{"shadow.gs", ShaderType::GEOMETRY},
            Shader{"shadowVpl.fs", ShaderType::FRAGMENT}},
            {{"DEPTH_LAYER", std::to_string(i)}}))
        );
        _state.shaders.push_back(_state.vplShadows[i].get());
    }

    _state.lighting = std::unique_ptr<Pipeline>(new Pipeline(shaderRoot, version, {
        Shader{"pbr.vs", ShaderType::VERTEX},
        Shader{"pbr.fs", ShaderType::FRAGMENT}}));
    _state.shaders.push_back(_state.lighting.get());

    _state.bloom = std::unique_ptr<Pipeline>(new Pipeline(shaderRoot, version, {
        Shader{"bloom.vs", ShaderType::VERTEX},
        Shader{"bloom.fs", ShaderType::FRAGMENT}}));
    _state.shaders.push_back(_state.bloom.get());

    for (int i = 0; i < 6; ++i) {
        _state.csmDepth.push_back(std::unique_ptr<Pipeline>(new Pipeline(shaderRoot, version, {
            Shader{"csm.vs", ShaderType::VERTEX},
            //Shader{"csm.gs", ShaderType::GEOMETRY},
            Shader{"csm.fs", ShaderType::FRAGMENT}},
            // Defines
            {{"DEPTH_LAYER", std::to_string(i)}}))
        );
        _state.shaders.push_back(_state.csmDepth[i].get());
    }

    _state.ssaoOcclude = std::unique_ptr<Pipeline>(new Pipeline(shaderRoot, version, {
        Shader{"ssao.vs", ShaderType::VERTEX},
        Shader{"ssao.fs", ShaderType::FRAGMENT}}));
    _state.shaders.push_back(_state.ssaoOcclude.get());

    _state.ssaoBlur = std::unique_ptr<Pipeline>(new Pipeline(shaderRoot, version, {
        // Intentionally reuse ssao.vs since it works for both this and ssao.fs
        Shader{"ssao.vs", ShaderType::VERTEX},
        Shader{"ssao_blur.fs", ShaderType::FRAGMENT}}));
    _state.shaders.push_back(_state.ssaoBlur.get());

    _state.atmospheric = std::unique_ptr<Pipeline>(new Pipeline(shaderRoot, version, {
        Shader{"atmospheric.vs", ShaderType::VERTEX},
        Shader{"atmospheric.fs", ShaderType::FRAGMENT}}));
    _state.shaders.push_back(_state.atmospheric.get());

    _state.atmosphericPostFx = std::unique_ptr<Pipeline>(new Pipeline(shaderRoot, version, {
        Shader{"atmospheric_postfx.vs", ShaderType::VERTEX},
        Shader{"atmospheric_postfx.fs", ShaderType::FRAGMENT}}));
    _state.shaders.push_back(_state.atmosphericPostFx.get());

    _state.vplCulling = std::unique_ptr<Pipeline>(new Pipeline(shaderRoot, version, {
        Shader{"vpl_light_cull.cs", ShaderType::COMPUTE}}));
    _state.shaders.push_back(_state.vplCulling.get());

    _state.vplTileDeferredCulling = std::unique_ptr<Pipeline>(new Pipeline(shaderRoot, version, {
        Shader{"vpl_tiled_deferred_culling.cs", ShaderType::COMPUTE}}));
    _state.shaders.push_back(_state.vplTileDeferredCulling.get());

    _state.vplGlobalIllumination = std::unique_ptr<Pipeline>(new Pipeline(shaderRoot, version, {
        Shader{"pbr_vpl_gi.vs", ShaderType::VERTEX},
        Shader{"pbr_vpl_gi.fs", ShaderType::FRAGMENT}}));
    _state.shaders.push_back(_state.vplGlobalIllumination.get());

    _state.fxaaLuminance = std::unique_ptr<Pipeline>(new Pipeline(shaderRoot, version, {
        Shader{"fxaa.vs", ShaderType::VERTEX},
        Shader{"fxaa_luminance.fs", ShaderType::FRAGMENT}
    }));
    _state.shaders.push_back(_state.fxaaLuminance.get());

    _state.fxaaSmoothing = std::unique_ptr<Pipeline>(new Pipeline(shaderRoot, version, {
        Shader{"fxaa.vs", ShaderType::VERTEX},
        Shader{"fxaa_smoothing.fs", ShaderType::FRAGMENT}
    }));
    _state.shaders.push_back(_state.fxaaSmoothing.get());

    // Create skybox cube
    _state.skyboxCube = ResourceManager::Instance()->CreateCube();

    // Create the screen quad
    _state.screenQuad = ResourceManager::Instance()->CreateQuad();

    // Use the shader isValid() method to determine if everything succeeded
    _ValidateAllShaders();

    _state.dummyCubeMap = _CreateShadowMap3D(_state.shadowCubeMapX, _state.shadowCubeMapY, false);

    // Init constant SSAO data
    _InitSSAO();

    // Init constant atmospheric data
    _InitAtmosphericShadowing();

    // Create a pool of shadow maps for point lights to use
    _InitPointShadowMaps();

    // Virtual point lights
    _InitializeVplData();
}

void RendererBackend::_InitPointShadowMaps() {
    // Create the normal point shadow map cache
    for (int i = 0; i < _state.numRegularShadowMaps; ++i) {
        _CreateShadowMap3D(_state.shadowCubeMapX, _state.shadowCubeMapY, false);
    }

    // Create the virtual point light shadow map cache
    for (int i = 0; i < _state.vpls.maxTotalVirtualPointLightsPerFrame; ++i) {
        _CreateShadowMap3D(_state.vpls.vplShadowCubeMapX, _state.vpls.vplShadowCubeMapY, true);
    }
}

void RendererBackend::_InitializeVplData() {
    const Bitfield flags = GPU_DYNAMIC_DATA | GPU_MAP_READ | GPU_MAP_WRITE;
    std::vector<int> visibleIndicesData(_state.vpls.maxTotalVirtualPointLightsPerFrame, 0);
    _state.vpls.vplDiffuseMaps = GpuBuffer(nullptr, sizeof(GpuTextureHandle) * _state.vpls.maxTotalVirtualPointLightsPerFrame, flags);
    _state.vpls.vplShadowMaps = GpuBuffer(nullptr, sizeof(GpuTextureHandle) * _state.vpls.maxTotalVirtualPointLightsPerFrame, flags);
    _state.vpls.vplVisibleIndices = GpuBuffer((const void *)visibleIndicesData.data(), sizeof(int) * visibleIndicesData.size(), flags);
    _state.vpls.vplPositions = GpuBuffer(nullptr, sizeof(GpuVec) * _state.vpls.maxTotalVirtualPointLightsPerFrame, flags);
    _state.vpls.vplColors = GpuBuffer(nullptr, sizeof(GpuVec) * _state.vpls.maxTotalVirtualPointLightsPerFrame, flags);
    _state.vpls.vplIntensities = GpuBuffer(nullptr, sizeof(float) * _state.vpls.maxTotalVirtualPointLightsPerFrame, flags);
    _state.vpls.vplShadowFactors = GpuBuffer(nullptr, sizeof(float) * _state.vpls.maxTotalVirtualPointLightsPerFrame, flags);
    _state.vpls.vplFarPlanes = GpuBuffer(nullptr, sizeof(float) * _state.vpls.maxTotalVirtualPointLightsPerFrame, flags);
    _state.vpls.vplRadii = GpuBuffer(nullptr, sizeof(float) * _state.vpls.maxTotalVirtualPointLightsPerFrame, flags);
    _state.vpls.vplShadowSamples = GpuBuffer(nullptr, sizeof(float) * _state.vpls.maxTotalVirtualPointLightsPerFrame, flags);
    _state.vpls.vplNumVisible = GpuBuffer(nullptr, sizeof(int), flags);
}

void RendererBackend::_ValidateAllShaders() {
    _isValid = true;
    for (Pipeline * p : _state.shaders) {
        _isValid = _isValid && p->isValid();
    }
}

RendererBackend::~RendererBackend() {
    for (Pipeline * shader : _shaders) delete shader;
    _shaders.clear();

    // Delete the main frame buffer
    _ClearGBuffer();
}

void RendererBackend::RecompileShaders() {
    for (Pipeline* p : _state.shaders) {
        p->recompile();
    }
    _ValidateAllShaders();
}

bool RendererBackend::Valid() const {
    return _isValid;
}

const Pipeline *RendererBackend::GetCurrentShader() const {
    return nullptr;
}

void RendererBackend::_RecalculateCascadeData() {
    const uint32_t cascadeResolutionXY = _frame->csc.cascadeResolutionXY;
    const uint32_t numCascades = _frame->csc.cascades.size();
    if (_frame->csc.regenerateFbo || !_frame->csc.fbo.valid()) {
        // Create the depth buffer
        // @see https://stackoverflow.com/questions/22419682/glsl-sampler2dshadow-and-shadow2d-clarificationssss
        Texture tex(TextureConfig{ TextureType::TEXTURE_2D_ARRAY, TextureComponentFormat::DEPTH, TextureComponentSize::BITS_DEFAULT, TextureComponentType::FLOAT, cascadeResolutionXY, cascadeResolutionXY, numCascades, false }, NoTextureData);
        tex.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);
        tex.setCoordinateWrapping(TextureCoordinateWrapping::CLAMP_TO_EDGE);
        // We need to set this when using sampler2DShadow in the GLSL shader
        tex.setTextureCompare(TextureCompareMode::COMPARE_REF_TO_TEXTURE, TextureCompareFunc::LEQUAL);

        // Create the frame buffer
        _frame->csc.fbo = FrameBuffer({ tex });
    }
}

void RendererBackend::_ClearGBuffer() {
    _state.buffer = GBuffer();
    _state.gaussianBuffers.clear();
    _state.postFxBuffers.clear();
}

void RendererBackend::_UpdateWindowDimensions() {
    if ( !_frame->viewportDirty ) return;
    glViewport(0, 0, _frame->viewportWidth, _frame->viewportHeight);

    // Set up VPL tile data
    const Bitfield flags = GPU_DYNAMIC_DATA | GPU_MAP_READ | GPU_MAP_WRITE;
    const int totalTiles = (_frame->viewportWidth) * (_frame->viewportHeight);
    const int totalTileEntries = totalTiles * _state.vpls.maxTotalVirtualLightsPerTile;
    std::vector<int> indicesPerTileData(totalTileEntries, 0);
    _state.vpls.vplLightIndicesVisiblePerTile = GpuBuffer((const void *)indicesPerTileData.data(), sizeof(int) * totalTileEntries, flags);
    std::vector<int> totalTilesData(totalTiles, 0);
    _state.vpls.vplNumLightsVisiblePerTile = GpuBuffer((const void *)totalTilesData.data(), sizeof(int) * totalTiles, flags);

    // Regenerate the main frame buffer
    _ClearGBuffer();

    GBuffer & buffer = _state.buffer;
    // glGenFramebuffers(1, &buffer.fbo);
    // glBindFramebuffer(GL_FRAMEBUFFER, buffer.fbo);

    // Position buffer
    buffer.position = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::RGB, TextureComponentSize::BITS_32, TextureComponentType::FLOAT, _frame->viewportWidth, _frame->viewportHeight, 0, false}, NoTextureData);
    buffer.position.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);

    // Normal buffer
    buffer.normals = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::RGB, TextureComponentSize::BITS_32, TextureComponentType::FLOAT, _frame->viewportWidth, _frame->viewportHeight, 0, false}, NoTextureData);
    buffer.normals.setMinMagFilter(TextureMinificationFilter::NEAREST, TextureMagnificationFilter::NEAREST);

    // Create the color buffer - notice that is uses higher
    // than normal precision. This allows us to write color values
    // greater than 1.0 to support things like HDR.
    buffer.albedo = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::RGB, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, _frame->viewportWidth, _frame->viewportHeight, 0, false}, NoTextureData);
    buffer.albedo.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);

    // Base reflectivity buffer
    buffer.baseReflectivity = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::RGB, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, _frame->viewportWidth, _frame->viewportHeight, 0, false}, NoTextureData);
    buffer.baseReflectivity.setMinMagFilter(TextureMinificationFilter::NEAREST, TextureMagnificationFilter::NEAREST);

    // Roughness-Metallic-Ambient buffer
    buffer.roughnessMetallicAmbient = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::RGB, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, _frame->viewportWidth, _frame->viewportHeight, 0, false}, NoTextureData);
    buffer.roughnessMetallicAmbient.setMinMagFilter(TextureMinificationFilter::NEAREST, TextureMagnificationFilter::NEAREST);

    // Create the Structure buffer which contains rgba where r=partial x-derivative of camera-space depth, g=partial y-derivative of camera-space depth, b=16 bits of depth, a=final 16 bits of depth (b+a=32 bits=depth)
    buffer.structure = Texture(TextureConfig{TextureType::TEXTURE_RECTANGLE, TextureComponentFormat::RGBA, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, _frame->viewportWidth, _frame->viewportHeight, 0, false}, NoTextureData);
    buffer.structure.setMinMagFilter(TextureMinificationFilter::NEAREST, TextureMagnificationFilter::NEAREST);
    buffer.structure.setCoordinateWrapping(TextureCoordinateWrapping::CLAMP_TO_EDGE);

    // Create the depth buffer
    buffer.depth = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::DEPTH, TextureComponentSize::BITS_DEFAULT, TextureComponentType::FLOAT, _frame->viewportWidth, _frame->viewportHeight, 0, false}, NoTextureData);
    buffer.depth.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);

    // Create the frame buffer with all its texture attachments
    buffer.fbo = FrameBuffer({buffer.position, buffer.normals, buffer.albedo, buffer.baseReflectivity, buffer.roughnessMetallicAmbient, buffer.structure, buffer.depth});
    if (!buffer.fbo.valid()) {
        _isValid = false;
        return;
    }

    // Code to create the lighting fbo
    _state.lightingColorBuffer = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::RGB, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, _frame->viewportWidth, _frame->viewportHeight, 0, false}, NoTextureData);
    _state.lightingColorBuffer.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);
    _state.lightingColorBuffer.setCoordinateWrapping(TextureCoordinateWrapping::CLAMP_TO_EDGE);

    // Create the buffer we will use to add bloom as a post-processing effect
    _state.lightingHighBrightnessBuffer = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::RGB, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, _frame->viewportWidth, _frame->viewportHeight, 0, false}, NoTextureData);
    _state.lightingHighBrightnessBuffer.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);
    _state.lightingHighBrightnessBuffer.setCoordinateWrapping(TextureCoordinateWrapping::CLAMP_TO_EDGE);

    // Create the depth buffer
    _state.lightingDepthBuffer = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::DEPTH, TextureComponentSize::BITS_DEFAULT, TextureComponentType::FLOAT, _frame->viewportWidth, _frame->viewportHeight, 0, false}, NoTextureData);
    _state.lightingDepthBuffer.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);

    // Attach the textures to the FBO
    _state.lightingFbo = FrameBuffer({_state.lightingColorBuffer, _state.lightingHighBrightnessBuffer, _state.lightingDepthBuffer});
    if (!_state.lightingFbo.valid()) {
        _isValid = false;
        return;
    }

    // Code to create the SSAO fbo
    _state.ssaoOcclusionTexture = Texture(TextureConfig{TextureType::TEXTURE_RECTANGLE, TextureComponentFormat::RED, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, _frame->viewportWidth, _frame->viewportHeight, 0, false}, NoTextureData);
    _state.ssaoOcclusionTexture.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);
    _state.ssaoOcclusionTexture.setCoordinateWrapping(TextureCoordinateWrapping::CLAMP_TO_EDGE);
    _state.ssaoOcclusionBuffer = FrameBuffer({_state.ssaoOcclusionTexture});
    if (!_state.ssaoOcclusionBuffer.valid()) {
        _isValid = false;
        return;
    }

    // Code to create the SSAO blurred fbo
    _state.ssaoOcclusionBlurredTexture = Texture(TextureConfig{TextureType::TEXTURE_RECTANGLE, TextureComponentFormat::RED, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, _frame->viewportWidth, _frame->viewportHeight, 0, false}, NoTextureData);
    _state.ssaoOcclusionBlurredTexture.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);
    _state.ssaoOcclusionBlurredTexture.setCoordinateWrapping(TextureCoordinateWrapping::CLAMP_TO_EDGE);
    _state.ssaoOcclusionBlurredBuffer = FrameBuffer({_state.ssaoOcclusionBlurredTexture});
    if (!_state.ssaoOcclusionBlurredBuffer.valid()) {
        _isValid = false;
        return;
    }

    // Code to create the Virtual Point Light Global Illumination fbo
    _state.vpls.vplGIColorBuffer = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::RGB, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, _frame->viewportWidth, _frame->viewportHeight, 0, false}, NoTextureData);
    _state.vpls.vplGIColorBuffer.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);
    _state.vpls.vplGIColorBuffer.setCoordinateWrapping(TextureCoordinateWrapping::CLAMP_TO_EDGE);
    _state.vpls.vplGIFbo = FrameBuffer({_state.vpls.vplGIColorBuffer});
    if (!_state.vpls.vplGIFbo.valid()) {
        _isValid = false;
        return;
    }

    // Code to create the Atmospheric fbo
    _state.atmosphericTexture = Texture(TextureConfig{TextureType::TEXTURE_RECTANGLE, TextureComponentFormat::RED, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, _frame->viewportWidth / 2, _frame->viewportHeight / 2, 0, false}, NoTextureData);
    _state.atmosphericTexture.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);
    _state.atmosphericTexture.setCoordinateWrapping(TextureCoordinateWrapping::CLAMP_TO_EDGE);
    _state.atmosphericFbo = FrameBuffer({_state.atmosphericTexture});
    if (!_state.atmosphericFbo.valid()) {
        _isValid = false;
        return;
    }

    _InitializePostFxBuffers();
}

void RendererBackend::_InitializePostFxBuffers() {
    uint32_t currWidth = _frame->viewportWidth;
    uint32_t currHeight = _frame->viewportHeight;
    _state.numDownsampleIterations = 0;
    _state.numUpsampleIterations = 0;

    // Initialize bloom
    for (; _state.numDownsampleIterations < 8; ++_state.numDownsampleIterations) {
        currWidth /= 2;
        currHeight /= 2;
        if (currWidth < 8 || currHeight < 8) break;
        PostFXBuffer buffer;
        auto color = Texture(TextureConfig{ TextureType::TEXTURE_2D, TextureComponentFormat::RGBA, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, currWidth, currHeight, 0, false }, NoTextureData);
        color.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);
        color.setCoordinateWrapping(TextureCoordinateWrapping::CLAMP_TO_EDGE); // TODO: Does this make sense for bloom textures?
        buffer.fbo = FrameBuffer({ color });
        if (!buffer.fbo.valid()) {
            _isValid = false;
            STRATUS_ERROR << "Unable to initialize bloom buffer" << std::endl;
            return;
        }
        _state.postFxBuffers.push_back(buffer);

        // Create the Gaussian Blur buffers
        PostFXBuffer dualBlurFbos[2];
        for (int i = 0; i < 2; ++i) {
            FrameBuffer& blurFbo = dualBlurFbos[i].fbo;
            Texture tex = Texture(color.getConfig(), NoTextureData);
            tex.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);
            tex.setCoordinateWrapping(TextureCoordinateWrapping::CLAMP_TO_EDGE);
            blurFbo = FrameBuffer({tex});
            _state.gaussianBuffers.push_back(dualBlurFbos[i]);
        }
    }

    std::vector<std::pair<uint32_t, uint32_t>> sizes;
    for (int i = _state.numDownsampleIterations - 2; i >= 0; --i) {
        auto tex = _state.postFxBuffers[i].fbo.getColorAttachments()[0];
        sizes.push_back(std::make_pair(tex.width(), tex.height()));
    }
    sizes.push_back(std::make_pair(_frame->viewportWidth, _frame->viewportHeight));
    
    for (auto&[width, height] : sizes) {
        PostFXBuffer buffer;
        ++_state.numUpsampleIterations;
        auto color = Texture(TextureConfig{ TextureType::TEXTURE_2D, TextureComponentFormat::RGBA, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, width, height, 0, false }, NoTextureData);
        color.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);
        color.setCoordinateWrapping(TextureCoordinateWrapping::CLAMP_TO_EDGE); // TODO: Does this make sense for bloom textures?
        buffer.fbo = FrameBuffer({ color });
        if (!buffer.fbo.valid()) {
            _isValid = false;
            STRATUS_ERROR << "Unable to initialize bloom buffer" << std::endl;
            return;
        }
        _state.postFxBuffers.push_back(buffer);
    }

    // Create the atmospheric post fx buffer
    Texture atmosphericTexture = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::RGBA, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, _frame->viewportWidth, _frame->viewportHeight, 0, false}, NoTextureData);
    atmosphericTexture.setMinMagFilter(TextureMinificationFilter::NEAREST, TextureMagnificationFilter::NEAREST);
    atmosphericTexture.setCoordinateWrapping(TextureCoordinateWrapping::CLAMP_TO_EDGE);
    _state.atmosphericPostFxBuffer.fbo = FrameBuffer({atmosphericTexture});
    if (!_state.atmosphericPostFxBuffer.fbo.valid()) {
        _isValid = false;
        STRATUS_ERROR << "Unable to initialize atmospheric post fx buffer" << std::endl;
        return;
    }
    _state.postFxBuffers.push_back(_state.atmosphericPostFxBuffer);

    // Create the FXAA buffers
    Texture fxaa = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::RGBA, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, _frame->viewportWidth, _frame->viewportHeight, 0, false}, NoTextureData);
    fxaa.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);
    fxaa.setCoordinateWrapping(TextureCoordinateWrapping::CLAMP_TO_EDGE);
    _state.fxaaFbo1.fbo = FrameBuffer({fxaa});
    if (!_state.fxaaFbo1.fbo.valid()) {
        _isValid = false;
        STRATUS_ERROR << "Unable to initialize fxaa luminance buffer" << std::endl;
        return;
    }
    _state.postFxBuffers.push_back(_state.fxaaFbo1);

    fxaa = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::RGBA, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, _frame->viewportWidth, _frame->viewportHeight, 0, false}, NoTextureData);
    fxaa.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);
    fxaa.setCoordinateWrapping(TextureCoordinateWrapping::CLAMP_TO_EDGE);
    _state.fxaaFbo2.fbo = FrameBuffer({fxaa});
    if (!_state.fxaaFbo2.fbo.valid()) {
        _isValid = false;
        STRATUS_ERROR << "Unable to initialize fxaa smoothing buffer" << std::endl;
        return;
    }
    _state.postFxBuffers.push_back(_state.fxaaFbo2);
}

void RendererBackend::_ClearFramebufferData(const bool clearScreen) {
    // Always clear the main screen buffer, but only
    // conditionally clean the custom frame buffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0); // default
    glDepthMask(GL_TRUE);
    glClearDepthf(1.0f);
    glClearColor(_frame->clearColor.r, _frame->clearColor.g, _frame->clearColor.b, _frame->clearColor.a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (clearScreen) {
        const glm::vec4& color = _frame->clearColor;
        _state.buffer.fbo.clear(color);
        _state.ssaoOcclusionBuffer.clear(color);
        _state.ssaoOcclusionBlurredBuffer.clear(color);
        _state.atmosphericFbo.clear(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        _state.lightingFbo.clear(color);
        _state.vpls.vplGIFbo.clear(color);

        // Depending on when this happens we may not have generated cascadeFbo yet
        if (_frame->csc.fbo.valid()) {
            _frame->csc.fbo.clear(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            //const int index = Engine::Instance()->FrameCount() % 4;
            //_frame->csc.fbo.ClearDepthStencilLayer(index);
        }

        for (auto& gaussian : _state.gaussianBuffers) {
            gaussian.fbo.clear(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        }

        for (auto& postFx : _state.postFxBuffers) {
            postFx.fbo.clear(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        }

        _state.atmosphericPostFxBuffer.fbo.clear(glm::vec4(0.0f));
    }
}

void RendererBackend::_InitSSAO() {
    // Create k values 0 to 15 and randomize them
    std::vector<float> ks(16);
    std::iota(ks.begin(), ks.end(), 0.0f);
    std::shuffle(ks.begin(), ks.end(), std::default_random_engine{});

    // Create the data for the 4x4 lookup table
    float table[16 * 3]; // RGB
    for (size_t i = 0; i < ks.size(); ++i) {
        const float k = ks[i];
        const Radians r(2.0f * float(STRATUS_PI) * k / 16.0f);
        table[i * 3    ] = cosine(r).value();
        table[i * 3 + 1] = sine(r).value();
        table[i * 3 + 2] = 0.0f;
    }

    // Create the lookup texture
    _state.ssaoOffsetLookup = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::RGB, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, 4, 4, 0, false}, TextureArrayData{(const void *)table});
    _state.ssaoOffsetLookup.setMinMagFilter(TextureMinificationFilter::NEAREST, TextureMagnificationFilter::NEAREST);
    _state.ssaoOffsetLookup.setCoordinateWrapping(TextureCoordinateWrapping::REPEAT);
}

void RendererBackend::_InitAtmosphericShadowing() {
    auto re = std::default_random_engine{};
    // On the range [0.0, 1.0) --> we technically want [0.0, 1.0] but it's close enough
    std::uniform_real_distribution<float> real(0.0f, 1.0f);

    // Create the 64x64 noise texture
    const size_t size = 32 * 32;
    std::vector<float> table(size);
    for (size_t i = 0; i < size; ++i) {
        table[i] = real(re);
    }

    const void* ptr = (const void *)table.data();
    _state.atmosphericNoiseTexture = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::RED, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, 32, 32, 0, false}, TextureArrayData{ptr});
    _state.atmosphericNoiseTexture.setMinMagFilter(TextureMinificationFilter::NEAREST, TextureMagnificationFilter::NEAREST);
    _state.atmosphericNoiseTexture.setCoordinateWrapping(TextureCoordinateWrapping::REPEAT);
}

void RendererBackend::_ClearRemovedLightData() {
    int lightsCleared = 0;
    for (auto ptr : _frame->lightsToRemove) {
        _RemoveLightFromShadowMapCache(ptr);
        ++lightsCleared;
    }

    if (lightsCleared > 0) STRATUS_LOG << "Cleared " << lightsCleared << " lights this frame" << std::endl;
}

void RendererBackend::Begin(const std::shared_ptr<RendererFrame>& frame, bool clearScreen) {
    CHECK_IS_APPLICATION_THREAD();

    _frame = frame;

    // Make sure we set our context as the active one
    GraphicsDriver::MakeContextCurrent();

    // Clear out instanced data from previous frame
    //_ClearInstancedData();

    // Clear out light data for lights that were removed
    _ClearRemovedLightData();

    // Checks to see if any framebuffers need to be generated or re-generated
    _RecalculateCascadeData();

    // Update all dimension, texture and framebuffer data if the viewport changed
    _UpdateWindowDimensions();

    // Includes screen data
    _ClearFramebufferData(clearScreen);

    // Generate the GPU data for all instanced entities
    //_InitAllInstancedData();

    glDisable(GL_BLEND);
    //glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CW);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_POLYGON_SMOOTH);

    // This is important! It prevents z-fighting if you do multiple passes.
    glDepthFunc(GL_LEQUAL);
}

/**
 * During the lighting phase, we need each of the 6 faces of the shadow map to have its own view transform matrix.
 * This enables us to convert vertices to be in various different light coordinate spaces.
 */
static std::vector<glm::mat4> GenerateLightViewTransforms(const glm::mat4 & projection, const glm::vec3 & lightPos) {
    return std::vector<glm::mat4>{
        //                       pos       pos + dir                                  up
        projection * glm::lookAt(lightPos, lightPos + glm::vec3( 1.0f,  0.0f,  0.0f), glm::vec3(0.0f, -1.0f,  0.0f)),
        projection * glm::lookAt(lightPos, lightPos + glm::vec3(-1.0f,  0.0f,  0.0f), glm::vec3(0.0f, -1.0f,  0.0f)),
        projection * glm::lookAt(lightPos, lightPos + glm::vec3( 0.0f,  1.0f,  0.0f), glm::vec3(0.0f,  0.0f,  1.0f)),
        projection * glm::lookAt(lightPos, lightPos + glm::vec3( 0.0f, -1.0f,  0.0f), glm::vec3(0.0f,  0.0f, -1.0f)),
        projection * glm::lookAt(lightPos, lightPos + glm::vec3( 0.0f,  0.0f,  1.0f), glm::vec3(0.0f, -1.0f,  0.0f)),
        projection * glm::lookAt(lightPos, lightPos + glm::vec3( 0.0f,  0.0f, -1.0f), glm::vec3(0.0f, -1.0f,  0.0f))
    };
}

void RendererBackend::_BindShader(Pipeline * s) {
    _UnbindShader();
    s->bind();
    _state.currentShader = s;
}

void RendererBackend::_UnbindShader() {
    if (!_state.currentShader) return;
    //_unbindAllTextures();
    _state.currentShader->unbind();
    _state.currentShader = nullptr;
}

static void SetCullState(const RenderFaceCulling & mode) {
    // Set the culling state
    switch (mode) {
    case RenderFaceCulling::CULLING_NONE:
        glDisable(GL_CULL_FACE);
        break;
    case RenderFaceCulling::CULLING_CW:
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CW);
        break;
    default:
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);
        break;
    }
}

static bool ValidateTexture(const Async<Texture> & tex) {
    return tex.Completed() && !tex.Failed();
}

void RendererBackend::_RenderImmediate(const RenderFaceCulling cull, GpuCommandBufferPtr& buffer) {
    if (buffer->NumDrawCommands() == 0) return;

    _frame->materialInfo.materialsBuffer.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 30);
    buffer->BindMaterialIndicesBuffer(31);
    buffer->BindModelTransformBuffer(13);
    buffer->BindIndirectDrawCommands();

    SetCullState(cull);

    glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, (const void *)0, (GLsizei)buffer->NumDrawCommands(), (GLsizei)0);

    buffer->UnbindIndirectDrawCommands();
}

void RendererBackend::_Render(const RenderFaceCulling cull, GpuCommandBufferPtr& buffer, bool isLightInteracting, bool removeViewTranslation) {
    if (buffer->NumDrawCommands() == 0) return;

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    const Camera& camera = *_frame->camera;
    const glm::mat4 & projection = _frame->projection;
    //const glm::mat4 & view = c.getViewTransform();
    glm::mat4 view;
    if (removeViewTranslation) {
        // Remove the translation component of the view matrix
        view = glm::mat4(glm::mat3(camera.getViewTransform()));
    }
    else {
        view = camera.getViewTransform();
    }

    // Unbind current shader if one is bound
    _UnbindShader();

    // Set up the shader we will use for this batch of entities
    Pipeline * s;
    if (isLightInteracting == false) {
        s = _state.forward.get();
    }
    else {
        s = _state.geometry.get();
    }

    //s->print();
    _BindShader(s);

    if (isLightInteracting) {
        s->setVec3("viewPosition", &camera.getPosition()[0]);
    }

    s->setMat4("projection", &projection[0][0]);
    s->setMat4("view", &view[0][0]);

    _RenderImmediate(cull, buffer);

#undef SETUP_TEXTURE

    _UnbindShader();

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
}

void RendererBackend::_Render(std::unordered_map<RenderFaceCulling, GpuCommandBufferPtr>& map, bool isLightInteracting, bool removeViewTranslation) {
    for (auto& entry : map) {
        _Render(entry.first, entry.second, isLightInteracting, removeViewTranslation);
    }
}

void RendererBackend::_RenderImmediate(std::unordered_map<RenderFaceCulling, GpuCommandBufferPtr>& map) {
    for (auto& entry : map) {
        _RenderImmediate(entry.first, entry.second);
    }
}

void RendererBackend::_RenderSkybox() {
    _BindShader(_state.skybox.get());
    glDepthMask(GL_FALSE);

    Async<Texture> sky = _LookupTexture(_frame->skybox);
    if (ValidateTexture(sky)) {
        const glm::mat4& projection = _frame->projection;
        const glm::mat4 view = glm::mat4(glm::mat3(_frame->camera->getViewTransform()));

        _state.skybox->setMat4("projection", projection);
        _state.skybox->setMat4("view", view);
        _state.skybox->bindTexture("skybox", sky.Get());

        GetMesh(_state.skyboxCube, 0)->Render(1, GpuArrayBuffer());
        //_state.skyboxCube->GetMeshContainer(0)->mesh->Render(1, GpuArrayBuffer());
    }

    _UnbindShader();
    glDepthMask(GL_TRUE);
}

void RendererBackend::_RenderCSMDepth() {
    if (_frame->csc.cascades.size() > _state.csmDepth.size()) {
        throw std::runtime_error("Max cascades exceeded (> 6)");
    }

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    // Allows GPU to perform angle-dependent depth offset to help reduce artifacts such as shadow acne
    glEnable(GL_POLYGON_OFFSET_FILL);
    // See https://paroj.github.io/gltut/Positioning/Tut05%20Depth%20Clamping.html
    glEnable(GL_DEPTH_CLAMP);
    glPolygonOffset(3.0f, 1.0f);
    //glBlendFunc(GL_ONE, GL_ONE);
    // glDisable(GL_CULL_FACE);

    _frame->csc.fbo.bind();
    const Texture * depth = _frame->csc.fbo.getDepthStencilAttachment();
    if (!depth) {
        throw std::runtime_error("Critical error: depth attachment not present");
    }
    glViewport(0, 0, depth->width(), depth->height());

    for (size_t cascade = 0; cascade < _frame->csc.cascades.size(); ++cascade) {
        Pipeline * shader = _state.csmDepth[cascade].get();
        _BindShader(shader);

        shader->setVec3("lightDir", &_frame->csc.worldLightCamera->getDirection()[0]);
        shader->setFloat("nearClipPlane", _frame->znear);

        // Set up each individual view-projection matrix
        // for (int i = 0; i < _frame->csc.cascades.size(); ++i) {
        //     auto& csm = _frame->csc.cascades[i];
        //     _state.csmDepth->setMat4("shadowMatrices[" + std::to_string(i) + "]", &csm.projectionViewRender[0][0]);
        // }

        // Select face (one per frame)
        //const int face = Engine::Instance()->FrameCount() % 4;
        //_state.csmDepth->setInt("face", face);

        // Render everything
        auto& csm = _frame->csc.cascades[cascade];
        shader->setMat4("shadowMatrix", csm.projectionViewRender);
        _RenderImmediate(_frame->instancedStaticPbrMeshes);
        _RenderImmediate(_frame->instancedDynamicPbrMeshes);

        _UnbindShader();
    }
    
    _frame->csc.fbo.unbind();

    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_DEPTH_CLAMP);
}

void RendererBackend::_RenderSsaoOcclude() {
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);

    // Aspect ratio
    const float ar        = float(_frame->viewportWidth) / float(_frame->viewportHeight);
    // Distance to the view projection plane
    const float g         = 1.0f / glm::tan(_frame->fovy.value() / 2.0f);
    const float w         = _frame->viewportWidth;
    // Gets fed into sigma value
    const float intensity = 5.0f;

    _BindShader(_state.ssaoOcclude.get());
    _state.ssaoOcclusionBuffer.bind();
    _state.ssaoOcclude->bindTexture("structureBuffer", _state.buffer.structure);
    _state.ssaoOcclude->bindTexture("rotationLookup", _state.ssaoOffsetLookup);
    _state.ssaoOcclude->setFloat("aspectRatio", ar);
    _state.ssaoOcclude->setFloat("projPlaneZDist", g);
    _state.ssaoOcclude->setFloat("windowHeight", _frame->viewportHeight);
    _state.ssaoOcclude->setFloat("windowWidth", w);
    _state.ssaoOcclude->setFloat("intensity", intensity);
    _RenderQuad();
    _state.ssaoOcclusionBuffer.unbind();
    _UnbindShader();

    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
}

void RendererBackend::_RenderSsaoBlur() {
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);

    _BindShader(_state.ssaoBlur.get());
    _state.ssaoOcclusionBlurredBuffer.bind();
    _state.ssaoBlur->bindTexture("structureBuffer", _state.buffer.structure);
    _state.ssaoBlur->bindTexture("occlusionBuffer", _state.ssaoOcclusionTexture);
    _state.ssaoBlur->setFloat("windowWidth", _frame->viewportWidth);
    _state.ssaoBlur->setFloat("windowHeight", _frame->viewportHeight);
    _RenderQuad();
    _state.ssaoOcclusionBlurredBuffer.unbind();
    _UnbindShader();

    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
}

void RendererBackend::_RenderAtmosphericShadowing() {
    if (!_frame->csc.worldLight->getEnabled()) return;

    constexpr float preventDivByZero = std::numeric_limits<float>::epsilon();

    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);

    auto re = std::default_random_engine{};
    const float n = _frame->atmospheric.numSamples;
    // On the range [0.0, 1/n)
    std::uniform_real_distribution<float> real(0.0f, 1.0f / n);
    const glm::vec2 noiseShift(real(re), real(re));
    const float dmin     = _frame->znear;
    const float dmax     = _frame->csc.cascades[_frame->csc.cascades.size() - 1].cascadeEnds;
    const float lambda   = _frame->atmospheric.fogDensity;
    // cbrt = cube root
    const float cubeR    = std::cbrt(_frame->atmospheric.scatterControl);
    const float g        = (1.0f - cubeR) / (1.0f + cubeR + preventDivByZero);
    // aspect ratio
    const float ar       = float(_frame->viewportWidth) / float(_frame->viewportHeight);
    // g in frustum parameters
    const float projDist = 1.0f / glm::tan(_frame->fovy.value() / 2.0f);
    const glm::vec3 frustumParams(ar / projDist, 1.0f / projDist, dmin);
    const glm::mat4 shadowMatrix = _frame->csc.cascades[0].projectionViewSample * _frame->camera->getWorldTransform();
    const glm::vec3 anisotropyConstants(1 - g, 1 + g * g, 2 * g);
    const glm::vec4 shadowSpaceCameraPos = _frame->csc.cascades[0].projectionViewSample * glm::vec4(_frame->camera->getPosition(), 1.0f);
    const glm::vec3 normalizedCameraLightDirection = _frame->csc.worldLightDirectionCameraSpace;

    _BindShader(_state.atmospheric.get());
    _state.atmosphericFbo.bind();
    _state.atmospheric->setVec3("frustumParams", frustumParams);
    _state.atmospheric->setMat4("shadowMatrix", shadowMatrix);
    _state.atmospheric->bindTexture("structureBuffer", _state.buffer.structure);
    _state.atmospheric->bindTexture("infiniteLightShadowMap", *_frame->csc.fbo.getDepthStencilAttachment());
    
    // Set up cascade data
    for (int i = 0; i < 4; ++i) {
        const auto& cascade = _frame->csc.cascades[i];
        const std::string si = "[" + std::to_string(i) + "]";
        _state.atmospheric->setFloat("maxCascadeDepth" + si, cascade.cascadeEnds);
        if (i > 0) {
            const std::string sim1 = "[" + std::to_string(i - 1) + "]";
            _state.atmospheric->setMat4("cascade0ToCascadeK" + sim1, cascade.sampleCascade0ToCurrent);
        }
    }

    _state.atmospheric->bindTexture("noiseTexture", _state.atmosphericNoiseTexture);
    _state.atmospheric->setFloat("minAtmosphereDepth", dmin);
    _state.atmospheric->setFloat("atmosphereDepthDiff", dmax - dmin);
    _state.atmospheric->setFloat("atmosphereDepthRatio", dmax / dmin);
    _state.atmospheric->setFloat("atmosphereFogDensity", lambda);
    _state.atmospheric->setVec3("anisotropyConstants", anisotropyConstants);
    _state.atmospheric->setVec4("shadowSpaceCameraPos", shadowSpaceCameraPos);
    _state.atmospheric->setVec3("normalizedCameraLightDirection", normalizedCameraLightDirection);
    _state.atmospheric->setVec2("noiseShift", noiseShift);
    const Texture& colorTex = _state.atmosphericFbo.getColorAttachments()[0];
    _state.atmospheric->setFloat("windowWidth", float(colorTex.width()));
    _state.atmospheric->setFloat("windowHeight", float(colorTex.height()));

    glViewport(0, 0, colorTex.width(), colorTex.height());
    _RenderQuad();
    _state.atmosphericFbo.unbind();
    _UnbindShader();

    glViewport(0, 0, _frame->viewportWidth, _frame->viewportHeight);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
}

void RendererBackend::_UpdatePointLights(std::vector<std::pair<LightPtr, double>>& perLightDistToViewer, 
                                         std::vector<std::pair<LightPtr, double>>& perLightShadowCastingDistToViewer,
                                         std::vector<std::pair<LightPtr, double>>& perVPLDistToViewer) {
    const Camera& c = *_frame->camera;

    perLightDistToViewer.reserve(_state.maxTotalRegularLightsPerFrame);
    perLightShadowCastingDistToViewer.reserve(_state.maxShadowCastingLightsPerFrame);
    perVPLDistToViewer.reserve(_state.vpls.maxTotalVirtualPointLightsPerFrame);

    // Init per light instance data
    for (auto& light : _frame->lights) {
        const double distance = glm::distance(c.getPosition(), light->GetPosition());
        if (light->IsVirtualLight()) {
            perVPLDistToViewer.push_back(std::make_pair(light, distance));
        }
        else {
            perLightDistToViewer.push_back(std::make_pair(light, distance));
        }

        if ( !light->IsVirtualLight() && light->castsShadows() ) {
            perLightShadowCastingDistToViewer.push_back(std::make_pair(light, distance));
        }
    }

    // Sort lights based on distance to viewer
    const auto comparison = [](const std::pair<LightPtr, double> & a, const std::pair<LightPtr, double> & b) {
        return a.second < b.second;
    };
    std::sort(perLightDistToViewer.begin(), perLightDistToViewer.end(), comparison);
    std::sort(perLightShadowCastingDistToViewer.begin(), perLightShadowCastingDistToViewer.end(), comparison);
    std::sort(perVPLDistToViewer.begin(), perVPLDistToViewer.end(), comparison);

    // Remove lights exceeding the absolute maximum
    if (perLightDistToViewer.size() > _state.maxTotalRegularLightsPerFrame) {
        perLightDistToViewer.resize(_state.maxTotalRegularLightsPerFrame);
    }

    // Remove shadow-casting lights that exceed our max count
    if (perLightShadowCastingDistToViewer.size() > _state.maxShadowCastingLightsPerFrame) {
        perLightShadowCastingDistToViewer.resize(_state.maxShadowCastingLightsPerFrame);
    }

    // Remove vpls exceeding absolute maximum
    if (perVPLDistToViewer.size() > _state.vpls.maxTotalVirtualPointLightsPerFrame) {
        perVPLDistToViewer.resize(_state.vpls.maxTotalVirtualPointLightsPerFrame);
    }

    // Check if any need to have a new shadow map pulled from the cache
    std::vector<std::vector<std::pair<LightPtr, double>> *> shadowCasters{
        &perLightShadowCastingDistToViewer,
        &perVPLDistToViewer    
    };

    for (const auto* vec : shadowCasters) {
        for (const auto&[light, _] : *vec) {
            if (!_ShadowMapExistsForLight(light)) {
                _frame->lightsToUpate.PushBack(light);
            }
        }
    }

    // Set blend func just for shadow pass
    // glBlendFunc(GL_ONE, GL_ONE);
    glEnable(GL_DEPTH_TEST);
    // Perform the shadow volume pre-pass
    for (int shadowUpdates = 0; shadowUpdates < _state.maxShadowUpdatesPerFrame && _frame->lightsToUpate.Size() > 0; ++shadowUpdates) {
        auto light = _frame->lightsToUpate.PopFront();
        // Ideally this won't be needed but just in case
        if ( !light->castsShadows() ) continue;
        //const double distance = perLightShadowCastingDistToViewer.find(light)->second;
    
        // TODO: Make this work with spotlights
        //PointLightPtr point = (PointLightPtr)light;
        PointLight * point = (PointLight *)light.get();
        ShadowMap3D smap = _GetOrAllocateShadowMapForLight(light);

        const glm::mat4 lightPerspective = glm::perspective<float>(glm::radians(90.0f), float(smap.shadowCubeMap.width()) / smap.shadowCubeMap.height(), point->getNearPlane(), point->getFarPlane());

        // glBindFramebuffer(GL_FRAMEBUFFER, smap.frameBuffer);
        smap.frameBuffer.clear(glm::vec4(1.0f));
        smap.frameBuffer.bind();
        glViewport(0, 0, smap.shadowCubeMap.width(), smap.shadowCubeMap.height());
        // Current pass only cares about depth buffer
        // glClear(GL_DEPTH_BUFFER_BIT);

        auto transforms = GenerateLightViewTransforms(lightPerspective, point->GetPosition());
        for (size_t i = 0; i < transforms.size(); ++i) {
            Pipeline * shader = light->IsVirtualLight() ? _state.vplShadows[i].get() : _state.shadows[i].get();
            _BindShader(shader);

            shader->setMat4("shadowMatrix", transforms[i]);
            shader->setVec3("lightPos", light->GetPosition());
            shader->setFloat("farPlane", point->getFarPlane());

            _RenderImmediate(_frame->instancedStaticPbrMeshes);
            if ( !point->IsStaticLight() ) _RenderImmediate(_frame->instancedDynamicPbrMeshes);

            _UnbindShader();
        }

        // Unbind
        smap.frameBuffer.unbind();
    }
}

void RendererBackend::_PerformVirtualPointLightCulling(std::vector<std::pair<LightPtr, double>>& perVPLDistToViewer) {
    if (perVPLDistToViewer.size() == 0) return;

    // Pack data into system memory
    std::vector<GpuTextureHandle> diffuseHandles(perVPLDistToViewer.size());
    std::vector<GpuTextureHandle> smapHandles(perVPLDistToViewer.size());
    std::vector<GpuVec> lightPositions(perVPLDistToViewer.size());
    std::vector<float> lightIntensities(perVPLDistToViewer.size());
    std::vector<float> lightFarPlanes(perVPLDistToViewer.size());
    std::vector<float> lightRadii(perVPLDistToViewer.size());
    std::vector<float> lightShadowSamples(perVPLDistToViewer.size());
    for (size_t i = 0; i < perVPLDistToViewer.size(); ++i) {
        VirtualPointLight * point = (VirtualPointLight *)perVPLDistToViewer[i].first.get();
        auto smap = _GetOrAllocateShadowMapForLight(perVPLDistToViewer[i].first);
        diffuseHandles[i] = smap.diffuseCubeMap.GpuHandle();
        smapHandles[i] = smap.shadowCubeMap.GpuHandle();
        lightPositions[i] = GpuVec(glm::vec4(point->GetPosition(), 1.0f));
        lightFarPlanes[i] = point->getFarPlane();
        lightRadii[i] = point->getRadius();
        lightIntensities[i] = point->getIntensity();
        lightShadowSamples[i] = float(point->GetNumShadowSamples());
    }

    // Move data to GPU memory
    _state.vpls.vplDiffuseMaps.CopyDataToBuffer(0, sizeof(GpuTextureHandle) * diffuseHandles.size(), (const void *)diffuseHandles.data());
    _state.vpls.vplShadowMaps.CopyDataToBuffer(0, sizeof(GpuTextureHandle) * smapHandles.size(), (const void *)smapHandles.data());
    _state.vpls.vplPositions.CopyDataToBuffer(0, sizeof(GpuVec) * lightPositions.size(), (const void *)lightPositions.data());
    _state.vpls.vplFarPlanes.CopyDataToBuffer(0, sizeof(float) * lightFarPlanes.size(), (const void *)lightFarPlanes.data());
    _state.vpls.vplIntensities.CopyDataToBuffer(0, sizeof(float) * lightIntensities.size(), (const void *)lightIntensities.data());
    _state.vpls.vplRadii.CopyDataToBuffer(0, sizeof(float) * lightRadii.size(), (const void *)lightRadii.data());
    _state.vpls.vplShadowSamples.CopyDataToBuffer(0, sizeof(float) * lightShadowSamples.size(), (const void *)lightShadowSamples.data());

    _state.vplCulling->bind();

    const Camera & lightCam = *_frame->csc.worldLightCamera;
    // glm::mat4 lightView = lightCam.getViewTransform();
    const glm::vec3 direction = lightCam.getDirection();

    _state.vplCulling->setVec3("infiniteLightDirection", direction);
    _state.vplCulling->setVec3("infiniteLightColor", _frame->csc.worldLight->getLuminance());

    // Set up # visible atomic counter
    int numVisible = 0;
    _state.vpls.vplNumVisible.CopyDataToBuffer(0, sizeof(int), (const void *)&numVisible);
    _state.vpls.vplNumVisible.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 1);

    // Bind light data and visibility indices
    _state.vpls.vplShadowFactors.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 0);
    _state.vpls.vplPositions.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 4);
    _state.vpls.vplVisibleIndices.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 3);
    _state.vpls.vplDiffuseMaps.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 5);
    _state.vpls.vplColors.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 6);
    _state.vpls.vplIntensities.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 7);

    _InitCoreCSMData(_state.vplCulling.get());
    _state.vplCulling->dispatchCompute((unsigned int)perVPLDistToViewer.size(), 1, 1);
    _state.vplCulling->synchronizeCompute();
    _state.vplCulling->unbind();

    // Now perform culling per tile since we now know which lights are active
    _state.vplTileDeferredCulling->bind();

    // Bind inputs
    _state.vplTileDeferredCulling->bindTexture("gPosition", _state.buffer.position);
    _state.vplTileDeferredCulling->bindTexture("gNormal", _state.buffer.normals);
    _state.vplTileDeferredCulling->setInt("viewportWidth", _frame->viewportWidth);
    _state.vplTileDeferredCulling->setInt("viewportHeight", _frame->viewportHeight);

    _state.vpls.vplPositions.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 0);
    _state.vpls.vplRadii.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 7);
    _state.vpls.vplNumVisible.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 1);
    _state.vpls.vplVisibleIndices.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 3);
    _state.vpls.vplColors.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 10);
    _state.vpls.vplShadowMaps.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 11);

    // Bind outputs
    _state.vpls.vplLightIndicesVisiblePerTile.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 5);
    _state.vpls.vplNumLightsVisiblePerTile.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 6);

    // Dispatch and synchronize
    _state.vplTileDeferredCulling->dispatchCompute((unsigned int)_frame->viewportWidth / _state.vpls.tileXDivisor,
                                                   (unsigned int)_frame->viewportHeight / _state.vpls.tileYDivisor,
                                                   1);
    _state.vplTileDeferredCulling->synchronizeCompute();

    _state.vplTileDeferredCulling->unbind();

    // int * v = (int *)_state.vpls.vplNumLightsVisiblePerTile.MapMemory();
    // int * tv = (int *)_state.vpls.vplNumVisible.MapMemory();
    // int m = 0;
    // int mi = std::numeric_limits<int>::max();
    // std::cout << "Total Visible: " << *tv << std::endl;
    // int numNonZero = 0;
    // for (int i = 0; i < 1920 * 1080; ++i) {
    //     m = std::max(m, v[i]);
    //     mi = std::min(mi, v[i]);
    //     if (v[i] > 0) ++numNonZero;
    // }
    // std::cout << "MAX VPL: " << m << std::endl;
    // std::cout << "MIN VPL: " << mi << std::endl;
    // std::cout << "NNZ: " << numNonZero << std::endl;
    // _state.vpls.vplNumLightsVisiblePerTile.UnmapMemory();
    // _state.vpls.vplNumVisible.UnmapMemory();

    //_state.vpls.vplNumVisible.CopyDataFromBufferToSysMem(0, sizeof(int), (void *)&numVisible);
    //std::cout << "Num Visible VPLs: " << numVisible << std::endl;
}

void RendererBackend::_ComputeVirtualPointLightGlobalIllumination(const std::vector<std::pair<LightPtr, double>>& perVPLDistToViewer) {
    if (perVPLDistToViewer.size() == 0) return;

    glDisable(GL_DEPTH_TEST);
    _BindShader(_state.vplGlobalIllumination.get());
    _state.vpls.vplGIFbo.bind();

    // Set up infinite light color
    const glm::vec3 lightColor = _frame->csc.worldLight->getLuminance();
    _state.vplGlobalIllumination->setVec3("infiniteLightColor", lightColor);

    _state.vplGlobalIllumination->setInt("numTilesX", _frame->viewportWidth);
    _state.vplGlobalIllumination->setInt("numTilesY", _frame->viewportHeight);

    // All relevant rendering data is moved to the GPU during the light cull phase
    _state.vpls.vplNumLightsVisiblePerTile.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 3);
    _state.vpls.vplLightIndicesVisiblePerTile.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 4);
    _state.vpls.vplPositions.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 5);
    _state.vpls.vplColors.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 6);
    _state.vpls.vplRadii.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 7);
    _state.vpls.vplFarPlanes.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 8);
    _state.vpls.vplShadowSamples.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 9);
    _state.vpls.vplShadowMaps.BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 11);

    _state.vplGlobalIllumination->bindTexture("screen", _state.lightingColorBuffer);
    _state.vplGlobalIllumination->bindTexture("gPosition", _state.buffer.position);
    _state.vplGlobalIllumination->bindTexture("gNormal", _state.buffer.normals);
    _state.vplGlobalIllumination->bindTexture("gAlbedo", _state.buffer.albedo);
    _state.vplGlobalIllumination->bindTexture("gBaseReflectivity", _state.buffer.baseReflectivity);
    _state.vplGlobalIllumination->bindTexture("gRoughnessMetallicAmbient", _state.buffer.roughnessMetallicAmbient);
    _state.vplGlobalIllumination->bindTexture("ssao", _state.ssaoOcclusionBlurredTexture);

    const Camera& camera = _frame->camera.get();
    _state.vplGlobalIllumination->setVec3("viewPosition", camera.getPosition());
    _state.vplGlobalIllumination->setInt("viewportWidth", _frame->viewportWidth);
    _state.vplGlobalIllumination->setInt("viewportHeight", _frame->viewportHeight);

    _RenderQuad();
    
    _UnbindShader();
    _state.lightingFbo.copyFrom(_state.vpls.vplGIFbo, BufferBounds{0, 0, _frame->viewportWidth, _frame->viewportHeight}, BufferBounds{0, 0, _frame->viewportWidth, _frame->viewportHeight}, BufferBit::COLOR_BIT, BufferFilter::NEAREST);
}

void RendererBackend::RenderScene() {
    CHECK_IS_APPLICATION_THREAD();

    const Camera& c = *_frame->camera;

    // Bind buffers
    GpuMeshAllocator::BindBase(GpuBaseBindingPoint::SHADER_STORAGE_BUFFER, 32);
    GpuMeshAllocator::BindElementArrayBuffer();

    std::vector<std::pair<LightPtr, double>> perLightDistToViewer;
    // This one is just for shadow-casting lights
    std::vector<std::pair<LightPtr, double>> perLightShadowCastingDistToViewer;
    std::vector<std::pair<LightPtr, double>> perVPLDistToViewer;

    // Perform point light pass
    _UpdatePointLights(perLightDistToViewer, perLightShadowCastingDistToViewer, perVPLDistToViewer);

    // Perform world light depth pass if enabled
    if (_frame->csc.worldLight->getEnabled()) {
        _RenderCSMDepth();
    }

    // TEMP: Set up the light source
    //glm::vec3 lightPos(0.0f, 0.0f, 0.0f);
    //glm::vec3 lightColor(10.0f); 

    // Make sure to bind our own frame buffer for rendering
    _state.buffer.fbo.bind();
    
    // Make sure some of our global GL states are set properly for primary rendering below
    glBlendFunc(_state.blendSFactor, _state.blendDFactor);
    glViewport(0, 0, _frame->viewportWidth, _frame->viewportHeight);

    // Begin geometry pass
    glEnable(GL_DEPTH_TEST);

    _Render(_frame->instancedStaticPbrMeshes, true);
    _Render(_frame->instancedDynamicPbrMeshes, true);
    
    _state.buffer.fbo.unbind();

    //glEnable(GL_BLEND);

    // Begin first SSAO pass (occlusion)
    _RenderSsaoOcclude();

    // Begin second SSAO pass (blurring)
    _RenderSsaoBlur();

    // Begin atmospheric pass
    _RenderAtmosphericShadowing();

    // Begin deferred lighting pass
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    _state.lightingFbo.bind();

    //_unbindAllTextures();
    _BindShader(_state.lighting.get());
    _InitLights(_state.lighting.get(), perLightDistToViewer, _state.maxShadowCastingLightsPerFrame);
    _state.lighting->bindTexture("atmosphereBuffer", _state.atmosphericTexture);
    _state.lighting->bindTexture("gPosition", _state.buffer.position);
    _state.lighting->bindTexture("gNormal", _state.buffer.normals);
    _state.lighting->bindTexture("gAlbedo", _state.buffer.albedo);
    _state.lighting->bindTexture("gBaseReflectivity", _state.buffer.baseReflectivity);
    _state.lighting->bindTexture("gRoughnessMetallicAmbient", _state.buffer.roughnessMetallicAmbient);
    _state.lighting->bindTexture("ssao", _state.ssaoOcclusionBlurredTexture);
    _state.lighting->setFloat("windowWidth", _frame->viewportWidth);
    _state.lighting->setFloat("windowHeight", _frame->viewportHeight);
    _RenderQuad();
    _state.lightingFbo.unbind();
    _UnbindShader();
    _state.finalScreenTexture = _state.lightingColorBuffer;

    // If world light is enabled perform VPL Global Illumination pass
    if (_frame->csc.worldLight->getEnabled() && _frame->globalIlluminationEnabled) {
        // Handle VPLs for global illumination (can't do this earlier due to needing position data from GBuffer)
        _PerformVirtualPointLightCulling(perVPLDistToViewer);
        _ComputeVirtualPointLightGlobalIllumination(perVPLDistToViewer);
    }

    // Forward pass for all objects that don't interact with light (may also be used for transparency later as well)
    _state.lightingFbo.copyFrom(_state.buffer.fbo, BufferBounds{0, 0, _frame->viewportWidth, _frame->viewportHeight}, BufferBounds{0, 0, _frame->viewportWidth, _frame->viewportHeight}, BufferBit::DEPTH_BIT, BufferFilter::NEAREST);
    // Blit to default framebuffer - not that the framebuffer you are writing to has to match the internal format
    // of the framebuffer you are reading to!
    glEnable(GL_DEPTH_TEST);
    _state.lightingFbo.bind();
    
    // Skybox is one that does not interact with light at all
    _RenderSkybox();

    _Render(_frame->instancedFlatMeshes, false);

    _state.lightingFbo.unbind();
    _state.finalScreenTexture = _state.lightingColorBuffer;
    // glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Enable post-FX effects such as bloom
    _PerformPostFxProcessing();

    // Perform final drawing to screen + gamma correction
    _FinalizeFrame();

    // Unbind element array buffer
    GpuMeshAllocator::UnbindElementArrayBuffer();
}

void RendererBackend::_PerformPostFxProcessing() {
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    //glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);

    _PerformBloomPostFx();

    _PerformAtmosphericPostFx();

    _PerformFxaaPostFx();

    glEnable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

void RendererBackend::_PerformBloomPostFx() {
    // We use this so that we can avoid a final copy between the downsample and blurring stages
    std::vector<PostFXBuffer> finalizedPostFxFrames(_state.numDownsampleIterations + _state.numUpsampleIterations);
   
    Pipeline* bloom = _state.bloom.get();
    _BindShader(bloom);

    // Downsample stage
    bloom->setBool("downsamplingStage", true);
    bloom->setBool("upsamplingStage", false);
    bloom->setBool("finalStage", false);
    bloom->setBool("gaussianStage", false);
    for (int i = 0, gaussian = 0; i < _state.numDownsampleIterations; ++i, gaussian += 2) {
        PostFXBuffer& buffer = _state.postFxBuffers[i];
        Texture colorTex = buffer.fbo.getColorAttachments()[0];
        auto width = colorTex.width();
        auto height = colorTex.height();
        bloom->setFloat("viewportX", float(width));
        bloom->setFloat("viewportY", float(height));
        buffer.fbo.bind();
        glViewport(0, 0, width, height);
        if (i == 0) {
            bloom->bindTexture("mainTexture", _state.finalScreenTexture);
        }
        else {
            bloom->bindTexture("mainTexture", _state.postFxBuffers[i - 1].fbo.getColorAttachments()[0]);
        }
        _RenderQuad();
        buffer.fbo.unbind();

        // Now apply Gaussian blurring
        bool horizontal = false;
        bloom->setBool("downsamplingStage", false);
        bloom->setBool("gaussianStage", true);
        BufferBounds bounds = BufferBounds{0, 0, width, height};
        for (int i = 0; i < 2; ++i) {
            FrameBuffer& blurFbo = _state.gaussianBuffers[gaussian + i].fbo;
            FrameBuffer copyFromFbo;
            if (i == 0) {
                copyFromFbo = buffer.fbo;
            }
            else {
                copyFromFbo = _state.gaussianBuffers[gaussian].fbo;
            }

            bloom->setBool("horizontal", horizontal);
            bloom->bindTexture("mainTexture", copyFromFbo.getColorAttachments()[0]);
            horizontal = !horizontal;
            blurFbo.bind();
            _RenderQuad();
            blurFbo.unbind();
        }

        // Copy the end result back to the original buffer
        // buffer.fbo.copyFrom(_state.gaussianBuffers[gaussian + 1].fbo, bounds, bounds, BufferBit::COLOR_BIT, BufferFilter::LINEAR);
        finalizedPostFxFrames[i] = _state.gaussianBuffers[gaussian + 1];
    }

    // Upsample stage
    bloom->setBool("downsamplingStage", false);
    bloom->setBool("upsamplingStage", true);
    bloom->setBool("finalStage", false);
    bloom->setBool("gaussianStage", false);
    int postFXIndex = _state.numDownsampleIterations;
    for (int i = _state.numDownsampleIterations - 1; i >= 0; --i, ++postFXIndex) {
        PostFXBuffer& buffer = _state.postFxBuffers[postFXIndex];
        auto width = buffer.fbo.getColorAttachments()[0].width();
        auto height = buffer.fbo.getColorAttachments()[0].height();
        bloom->setFloat("viewportX", float(width));
        bloom->setFloat("viewportY", float(height));
        buffer.fbo.bind();
        glViewport(0, 0, width, height);
        //bloom->bindTexture("mainTexture", _state.postFxBuffers[postFXIndex - 1].fbo.getColorAttachments()[0]);
        bloom->bindTexture("mainTexture", finalizedPostFxFrames[postFXIndex - 1].fbo.getColorAttachments()[0]);
        if (i == 0) {
            bloom->bindTexture("bloomTexture", _state.lightingColorBuffer);
            bloom->setBool("finalStage", true);
        }
        else {
            //bloom->bindTexture("bloomTexture", _state.postFxBuffers[i - 1].fbo.getColorAttachments()[0]);
            bloom->bindTexture("bloomTexture", finalizedPostFxFrames[i - 1].fbo.getColorAttachments()[0]);
        }
        _RenderQuad();
        buffer.fbo.unbind();
        
        finalizedPostFxFrames[postFXIndex] = buffer;
        _state.finalScreenTexture = buffer.fbo.getColorAttachments()[0];
    }

    _UnbindShader();
}

glm::vec3 RendererBackend::_CalculateAtmosphericLightPosition() const {
    const glm::mat4& projection = _frame->projection;
    // See page 354, eqs. 10.81 and 10.82
    const glm::vec3& normalizedLightDirCamSpace = _frame->csc.worldLightDirectionCameraSpace;
    const Texture& colorTex = _state.atmosphericTexture;
    const float w = colorTex.width();
    const float h = colorTex.height();
    const float xlight = w * ((projection[0][0] * normalizedLightDirCamSpace.x + 
                               projection[0][1] * normalizedLightDirCamSpace.y + 
                               projection[0][2] * normalizedLightDirCamSpace.z) / (2.0f * normalizedLightDirCamSpace.z) + 0.5f);
    const float ylight = h * ((projection[1][0] * normalizedLightDirCamSpace.x + 
                               projection[1][1] * normalizedLightDirCamSpace.y + 
                               projection[1][2] * normalizedLightDirCamSpace.z) / (2.0f * normalizedLightDirCamSpace.z) + 0.5f);
    
    return 2.0f * normalizedLightDirCamSpace.z * glm::vec3(xlight, ylight, 1.0f);
}

void RendererBackend::_PerformAtmosphericPostFx() {
    if (!_frame->csc.worldLight->getEnabled()) return;

    glViewport(0, 0, _frame->viewportWidth, _frame->viewportHeight);

    const glm::vec3 lightPosition = _CalculateAtmosphericLightPosition();
    //const float sinX = stratus::sine(_frame->csc.worldLight->getRotation().x).value();
    //const float cosX = stratus::cosine(_frame->csc.worldLight->getRotation().x).value();
    const glm::vec3 lightColor = _frame->csc.worldLight->getColor();// * glm::vec3(cosX, cosX, sinX);

    _BindShader(_state.atmosphericPostFx.get());
    _state.atmosphericPostFxBuffer.fbo.bind();
    _state.atmosphericPostFx->bindTexture("atmosphereBuffer", _state.atmosphericTexture);
    _state.atmosphericPostFx->bindTexture("screenBuffer", _state.finalScreenTexture);
    _state.atmosphericPostFx->setVec3("lightPosition", lightPosition);
    _state.atmosphericPostFx->setVec3("lightColor", lightColor);
    _RenderQuad();
    _state.atmosphericPostFxBuffer.fbo.unbind();
    _UnbindShader();

    _state.finalScreenTexture = _state.atmosphericPostFxBuffer.fbo.getColorAttachments()[0];
}

void RendererBackend::_PerformFxaaPostFx() {
    glViewport(0, 0, _frame->viewportWidth, _frame->viewportHeight);

    // Perform luminance calculation pass
    _BindShader(_state.fxaaLuminance.get());
    
    _state.fxaaFbo1.fbo.bind();
    _state.fxaaLuminance->bindTexture("screen", _state.finalScreenTexture);
    _RenderQuad();
    _state.fxaaFbo1.fbo.unbind();

    _UnbindShader();

    _state.finalScreenTexture = _state.fxaaFbo1.fbo.getColorAttachments()[0];

    // Perform smoothing pass
    _BindShader(_state.fxaaSmoothing.get());

    _state.fxaaFbo2.fbo.bind();
    _state.fxaaSmoothing->bindTexture("screen", _state.finalScreenTexture);
    _RenderQuad();
    _state.fxaaFbo2.fbo.unbind();

    _UnbindShader();

    _state.finalScreenTexture = _state.fxaaFbo2.fbo.getColorAttachments()[0];
}

void RendererBackend::_FinalizeFrame() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_CULL_FACE);
    glViewport(0, 0, _frame->viewportWidth, _frame->viewportHeight);
    //glEnable(GL_BLEND);

    // Now render the screen
    _BindShader(_state.hdrGamma.get());
    _state.hdrGamma->bindTexture("screen", _state.finalScreenTexture);
    _RenderQuad();
    _UnbindShader();
}

void RendererBackend::End() {
    CHECK_IS_APPLICATION_THREAD();

    GraphicsDriver::SwapBuffers(_frame->vsyncEnabled);

    _frame.reset();
}

void RendererBackend::_RenderQuad() {
    GetMesh(_state.screenQuad, 0)->Render(1, GpuArrayBuffer());
    //_state.screenQuad->GetMeshContainer(0)->mesh->Render(1, GpuArrayBuffer());
}

TextureHandle RendererBackend::_CreateShadowMap3D(uint32_t resolutionX, uint32_t resolutionY, bool vpl) {
    ShadowMap3D smap;
    smap.shadowCubeMap = Texture(TextureConfig{TextureType::TEXTURE_3D, TextureComponentFormat::DEPTH, TextureComponentSize::BITS_DEFAULT, TextureComponentType::FLOAT, resolutionX, resolutionY, 0, false}, NoTextureData);
    smap.shadowCubeMap.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);
    smap.shadowCubeMap.setCoordinateWrapping(TextureCoordinateWrapping::CLAMP_TO_EDGE);
    // We need to set this when using sampler2DShadow in the GLSL shader
    //smap.shadowCubeMap.setTextureCompare(TextureCompareMode::COMPARE_REF_TO_TEXTURE, TextureCompareFunc::LEQUAL);

    if (vpl) {
        smap.diffuseCubeMap = Texture(TextureConfig{TextureType::TEXTURE_3D, TextureComponentFormat::RGB, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, resolutionX, resolutionY, 0, false}, NoTextureData);
        smap.diffuseCubeMap.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);
        smap.diffuseCubeMap.setCoordinateWrapping(TextureCoordinateWrapping::CLAMP_TO_EDGE);

        smap.frameBuffer = FrameBuffer({smap.diffuseCubeMap, smap.shadowCubeMap});
    }
    else {
        smap.frameBuffer = FrameBuffer({smap.shadowCubeMap});
    }
    
    if (!smap.frameBuffer.valid()) {
        _isValid = false;
        return TextureHandle::Null();
    }

    auto& cache = vpl ? _vplSmapCache : _smapCache;

    TextureHandle handle = TextureHandle::NextHandle();
    cache.shadowMap3DHandles.insert(std::make_pair(handle, smap));

    // These will be resident in GPU memory for the entire life cycle of the renderer
    Texture::MakeResident(smap.shadowCubeMap);
    if (vpl) Texture::MakeResident(smap.diffuseCubeMap);

    return handle;
}

Async<Texture> RendererBackend::_LookupTexture(TextureHandle handle) const {
    return INSTANCE(ResourceManager)->LookupTexture(handle);
}

Texture RendererBackend::_LookupShadowmapTexture(TextureHandle handle) const {
    if (handle == TextureHandle::Null()) return Texture();

    // See if it's in the regular cache
    auto it = _smapCache.shadowMap3DHandles.find(handle);
    if (it != _smapCache.shadowMap3DHandles.end()) {
        return it->second.shadowCubeMap;
    }

    // See if it's in the VPL cache
    auto vit = _vplSmapCache.shadowMap3DHandles.find(handle);
    if (vit != _vplSmapCache.shadowMap3DHandles.end()) {
        return vit->second.shadowCubeMap;
    }

    return Texture();
}

// This handles everything that's in pbr.glsl
void RendererBackend::_InitCoreCSMData(Pipeline * s) {
    const Camera & lightCam = *_frame->csc.worldLightCamera;
    // glm::mat4 lightView = lightCam.getViewTransform();
    const glm::vec3 direction = lightCam.getDirection();

    s->setVec3("infiniteLightDirection", direction);    
    s->bindTexture("infiniteLightShadowMap", *_frame->csc.fbo.getDepthStencilAttachment());
    for (int i = 0; i < _frame->csc.cascades.size(); ++i) {
        //s->bindTexture("infiniteLightShadowMaps[" + std::to_string(i) + "]", *_state.csms[i].fbo.getDepthStencilAttachment());
        s->setMat4("cascadeProjViews[" + std::to_string(i) + "]", _frame->csc.cascades[i].projectionViewSample);
        // s->setFloat("cascadeSplits[" + std::to_string(i) + "]", _state.cascadeSplits[i]);
    }

    for (int i = 0; i < 2; ++i) {
        s->setVec4("shadowOffset[" + std::to_string(i) + "]", _frame->csc.cascadeShadowOffsets[i]);
    }

    for (int i = 0; i < _frame->csc.cascades.size() - 1; ++i) {
        // s->setVec3("cascadeScale[" + std::to_string(i) + "]", &_state.csms[i + 1].cascadeScale[0]);
        // s->setVec3("cascadeOffset[" + std::to_string(i) + "]", &_state.csms[i + 1].cascadeOffset[0]);
        s->setVec4("cascadePlanes[" + std::to_string(i) + "]", _frame->csc.cascades[i + 1].cascadePlane);
    }
}

void RendererBackend::_InitLights(Pipeline * s, const std::vector<std::pair<LightPtr, double>> & lights, const size_t maxShadowLights) {
    // Set up point lights

    // Make sure everything is set to some sort of default to prevent shader crashes or huge performance drops
    s->setFloat("lightFarPlanes[0]", 1.0f);
    s->bindTexture("shadowCubeMaps[0]", _LookupShadowmapTexture(_state.dummyCubeMap));
    s->setVec3("lightPositions[0]", glm::vec3(0.0f));
    s->setVec3("lightColors[0]", glm::vec3(0.0f));
    s->setFloat("lightRadii[0]", 1.0f);
    s->setBool("lightCastsShadows[0]", false);

    const Camera& c = *_frame->camera;
    glm::vec3 lightColor;
    int lightIndex = 0;
    int shadowLightIndex = 0;
    for (int i = 0; i < lights.size(); ++i) {
        LightPtr light = lights[i].first;
        PointLight * point = (PointLight *)light.get();
        const double distance = lights[i].second; //glm::distance(c.getPosition(), light->position);
        // Skip lights too far from camera
        //if (distance > (2 * light->getRadius())) continue;

        // VPLs are handled as part of the global illumination compute pipeline
        if (point->IsVirtualLight()) continue;

        if (point->castsShadows()) {
            if (shadowLightIndex >= maxShadowLights) continue;
            s->setFloat("lightFarPlanes[" + std::to_string(shadowLightIndex) + "]", point->getFarPlane());
            //_bindShadowMapTexture(s, "shadowCubeMaps[" + std::to_string(shadowLightIndex) + "]", _GetOrAllocateShadowMapHandleForLight(light));
            s->bindTexture("shadowCubeMaps[" + std::to_string(shadowLightIndex) + "]", _LookupShadowmapTexture(_GetOrAllocateShadowMapHandleForLight(light)));
            ++shadowLightIndex;
        }

        lightColor = point->getBaseColor() * point->getIntensity();
        s->setVec3("lightPositions[" + std::to_string(lightIndex) + "]", point->GetPosition());
        s->setVec3("lightColors[" + std::to_string(lightIndex) + "]", &lightColor[0]);
        s->setFloat("lightRadii[" + std::to_string(lightIndex) + "]", point->getRadius());
        s->setBool("lightCastsShadows[" + std::to_string(lightIndex) + "]", point->castsShadows());
        //_bindShadowMapTexture(s, "shadowCubeMaps[" + std::to_string(lightIndex) + "]", light->getShadowMapHandle());
        ++lightIndex;
    }

    s->setFloat("ambientIntensity", 0.0001f);
    /*
    if (lightIndex == 0) {
        s->setFloat("ambientIntensity", 0.0001f);
    }
    else {
        s->setFloat("ambientIntensity", 0.0f);
    }
    */

    s->setInt("numLights", lightIndex);
    s->setInt("numShadowLights", shadowLightIndex);
    s->setVec3("viewPosition", c.getPosition());
    const glm::vec3 lightPosition = _CalculateAtmosphericLightPosition();
    s->setVec3("atmosphericLightPos", lightPosition);

    // Set up world light if enabled
    //glm::mat4 lightView = constructViewMatrix(_state.worldLight.getRotation(), _state.worldLight.getPosition());
    //glm::mat4 lightView = constructViewMatrix(_state.worldLight.getRotation(), glm::vec3(0.0f));
    // Camera lightCam(false);
    // lightCam.setAngle(_state.worldLight.getRotation());
    const Camera & lightCam = *_frame->csc.worldLightCamera;
    glm::mat4 lightWorld = lightCam.getWorldTransform();
    // glm::mat4 lightView = lightCam.getViewTransform();
    glm::vec3 direction = lightCam.getDirection(); //glm::vec3(-lightWorld[2].x, -lightWorld[2].y, -lightWorld[2].z);
    // STRATUS_LOG << "Light direction: " << direction << std::endl;
    s->setBool("infiniteLightingEnabled", _frame->csc.worldLight->getEnabled());
    lightColor = _frame->csc.worldLight->getLuminance();
    s->setVec3("infiniteLightColor", lightColor);
    s->setFloat("worldLightAmbientIntensity", _frame->csc.worldLight->getAmbientIntensity());

    _InitCoreCSMData(s);

    // s->setMat4("cascade0ProjView", &_state.csms[0].projectionView[0][0]);
}

TextureHandle RendererBackend::_GetOrAllocateShadowMapHandleForLight(LightPtr light) {
    auto& cache = _GetSmapCacheForLight(light);
    assert(cache.shadowMap3DHandles.size() > 0);

    auto it = cache.lightsToShadowMap.find(light);
    // If not found, look for an existing shadow map
    if (it == cache.lightsToShadowMap.end()) {
        TextureHandle handle;
        for (const auto & entry : cache.shadowMap3DHandles) {
            if (cache.usedShadowMaps.find(entry.first) == cache.usedShadowMaps.end()) {
                handle = entry.first;
                break;
            }
        }

        if (handle == TextureHandle::Null()) {
            // Evict oldest since we could not find an available handle
            LightPtr oldest = cache.lruLightCache.front();
            cache.lruLightCache.pop_front();
            handle = cache.lightsToShadowMap.find(oldest)->second;
            _EvictLightFromShadowMapCache(oldest);
        }

        _SetLightShadowMapHandle(light, handle);
        _AddLightToShadowMapCache(light);
        return handle;
    }

    // Update the LRU cache
    _AddLightToShadowMapCache(light);
    return it->second;
}

RendererBackend::ShadowMap3D RendererBackend::_GetOrAllocateShadowMapForLight(LightPtr light) {
    auto& cache = _GetSmapCacheForLight(light);
    return cache.shadowMap3DHandles.find(_GetOrAllocateShadowMapHandleForLight(light))->second;
}

void RendererBackend::_SetLightShadowMapHandle(LightPtr light, TextureHandle handle) {
    auto& cache = _GetSmapCacheForLight(light);
    cache.lightsToShadowMap.insert(std::make_pair(light, handle));
    cache.usedShadowMaps.insert(handle);
}

void RendererBackend::_EvictLightFromShadowMapCache(LightPtr light) {
    auto& cache = _GetSmapCacheForLight(light);
    for (auto it = cache.lruLightCache.begin(); it != cache.lruLightCache.end(); ++it) {
        if (*it == light) {
            cache.lruLightCache.erase(it);
            return;
        }
    }
}

bool RendererBackend::_ShadowMapExistsForLight(LightPtr light) {
    auto& cache = _GetSmapCacheForLight(light);
    return cache.lightsToShadowMap.find(light) != cache.lightsToShadowMap.end();
}

void RendererBackend::_AddLightToShadowMapCache(LightPtr light) {
    auto& cache = _GetSmapCacheForLight(light);
    // First remove the existing light entry if it's already there
    _EvictLightFromShadowMapCache(light);
    // Push to back so that it is seen as most recently used
    cache.lruLightCache.push_back(light);
}

void RendererBackend::_RemoveLightFromShadowMapCache(LightPtr light) {
    if ( !_ShadowMapExistsForLight(light) ) return;

    auto& cache = _GetSmapCacheForLight(light);

    // Deallocate its map
    TextureHandle handle = cache.lightsToShadowMap.find(light)->second;
    cache.lightsToShadowMap.erase(light);
    cache.usedShadowMaps.erase(handle);

    // Remove from LRU cache
    _EvictLightFromShadowMapCache(light);
}

RendererBackend::ShadowMapCache& RendererBackend::_GetSmapCacheForLight(LightPtr light) {
    return light->IsVirtualLight() ? _vplSmapCache : _smapCache;
}
}