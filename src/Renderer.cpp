
#include <Renderer.h>
#include <iostream>
#include <Light.h>
#include "Pipeline.h"
#include "Renderer.h"
#include "Quad.h"
#include "Utils.h"
#define STB_IMAGE_IMPLEMENTATION
#include "STBImage.h"

namespace stratus {
bool __RenderEntityObserver::operator==(const __RenderEntityObserver & c) const {
    return (*e) == *(c.e);
}

size_t __RenderEntityObserver::hashCode() const {
    return e->hashCode();
}

bool __MeshObserver::operator==(const __MeshObserver & c) const {
    return (*m) == *(c.m);
}

size_t __MeshObserver::hashCode() const {
    return m->hashCode();
}

static void printGLInfo(const GFXConfig & config) {
    std::cout << "==================== OpenGL Information ====================" << std::endl;
    std::cout << "\tRenderer: "                         << config.renderer << std::endl;
    std::cout << "\tVersion: "                          << config.version << std::endl;
    std::cout << "\tMax draw buffers: "                 << config.maxDrawBuffers << std::endl;
    std::cout << "\tMax combined textures: "            << config.maxCombinedTextures << std::endl;
    std::cout << "\tMax cube map texture size: "        << config.maxCubeMapTextureSize << std::endl;
    std::cout << "\tMax fragment uniform vectors: "     << config.maxFragmentUniformVectors << std::endl;
    std::cout << "\tMax fragment uniform components: "  << config.maxFragmentUniformComponents << std::endl;
    std::cout << "\tMax varying floats: "               << config.maxVaryingFloats << std::endl;
    std::cout << "\tMax render buffer size: "           << config.maxRenderbufferSize << std::endl;
    std::cout << "\tMax texture image units: "          << config.maxTextureImageUnits << std::endl;
    std::cout << "\tMax texture size: "                 << config.maxTextureSize << std::endl;
    std::cout << "\tMax vertex attribs: "               << config.maxVertexAttribs << std::endl;
    std::cout << "\tMax vertex uniform vectors: "       << config.maxVertexUniformVectors << std::endl;
    std::cout << "\tMax vertex uniform components: "    << config.maxVertexUniformComponents << std::endl;
    std::cout << "\tMax viewport dims: "                << "(" << config.maxViewportDims[0] << ", " << config.maxViewportDims[1] << ")" << std::endl;
}

Renderer::Renderer(SDL_Window * window) {
    _window = window;
    //const int32_t maxGLVersion = 3;
    //const int32_t minGLVersion = 2;

    // Set the profile to core as opposed to immediate mode
    SDL_GL_SetAttribute(SDL_GLattr::SDL_GL_CONTEXT_PROFILE_MASK,
            SDL_GL_CONTEXT_PROFILE_CORE);
    // Set max/min version to be 3.2
    //SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, maxGLVersion);
    //SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minGLVersion);
    // Enable double buffering
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    // Create the gl context
    _context = SDL_GL_CreateContext(window);
    if (_context == nullptr) {
        std::cerr << "[error] Unable to create a valid OpenGL context" << std::endl;
        _isValid = false;
        return;
    }

    // Init gl core profile using gl3w
    if (gl3wInit()) {
        std::cerr << "[error] Failed to initialize core OpenGL profile" << std::endl;
        _isValid = false;
        return;
    }

    //if (!gl3wIsSupported(maxGLVersion, minGLVersion)) {
    //    std::cerr << "[error] OpenGL 3.2 not supported" << std::endl;
    //    _isValid = false;
    //    return;
    //}

    // Query OpenGL about various different hardware capabilities
    _config.renderer = (const char *)glGetString(GL_RENDERER);
    _config.version = (const char *)glGetString(GL_VERSION);
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &_config.maxCombinedTextures);
    glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE, &_config.maxCubeMapTextureSize);
    glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &_config.maxFragmentUniformVectors);
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &_config.maxRenderbufferSize);
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &_config.maxTextureImageUnits);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &_config.maxTextureSize);
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &_config.maxVertexAttribs);
    glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &_config.maxVertexUniformVectors);
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &_config.maxDrawBuffers);
    glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, &_config.maxFragmentUniformComponents);
    glGetIntegerv(GL_MAX_VERTEX_UNIFORM_COMPONENTS, &_config.maxVertexUniformComponents);
    glGetIntegerv(GL_MAX_VARYING_FLOATS, &_config.maxVaryingFloats);
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, _config.maxViewportDims);

    printGLInfo(_config);
    _isValid = true;

    // Initialize the pipelines
    _state.geometry = std::unique_ptr<Pipeline>(new Pipeline({
        Shader{"../resources/shaders/pbr_geometry_pass.vs", ShaderType::VERTEX}, 
        Shader{"../resources/shaders/pbr_geometry_pass.fs", ShaderType::FRAGMENT}}));
    _state.shaders.push_back(_state.geometry.get());

    _state.forward = std::unique_ptr<Pipeline>(new Pipeline({
        Shader{"../resources/shaders/flat_forward_pass.vs", ShaderType::VERTEX}, 
        Shader{"../resources/shaders/flat_forward_pass.fs", ShaderType::FRAGMENT}}));
    _state.shaders.push_back(_state.forward.get());

    using namespace std;
    
    // Now we need to establish a mapping between all of the possible render
    // property combinations with a list of entities that match those requirements
    _state.entities.insert(make_pair(FLAT, vector<RenderEntity *>()));
    _state.entities.insert(make_pair(DYNAMIC, vector<RenderEntity *>()));

    // Set up the hdr/gamma postprocessing shader
    _state.hdrGamma = std::unique_ptr<Pipeline>(new Pipeline({
        Shader{"../resources/shaders/hdr.vs", ShaderType::VERTEX},
        Shader{"../resources/shaders/hdr.fs", ShaderType::FRAGMENT}}));
    _state.shaders.push_back(_state.hdrGamma.get());

    // Set up the shadow preprocessing shader
    _state.shadows = std::unique_ptr<Pipeline>(new Pipeline({
        Shader{"../resources/shaders/shadow.vs", ShaderType::VERTEX},
        Shader{"../resources/shaders/shadow.gs", ShaderType::GEOMETRY},
        Shader{"../resources/shaders/shadow.fs", ShaderType::FRAGMENT}}));
    _state.shaders.push_back(_state.shadows.get());

    _state.lighting = std::unique_ptr<Pipeline>(new Pipeline({
        Shader{"../resources/shaders/pbr.vs", ShaderType::VERTEX},
        Shader{"../resources/shaders/pbr.fs", ShaderType::FRAGMENT}}));
    _state.shaders.push_back(_state.lighting.get());

    _state.bloom = std::unique_ptr<Pipeline>(new Pipeline({
        Shader{"../resources/shaders/bloom.vs", ShaderType::VERTEX},
        Shader{"../resources/shaders/bloom.fs", ShaderType::FRAGMENT}}));
    _state.shaders.push_back(_state.bloom.get());

    // Create the screen quad
    _state.screenQuad = std::make_unique<Quad>();

    // Use the shader isValid() method to determine if everything succeeded
    _isValid = _isValid &&
            _state.forward ->isValid() &&
            _state.geometry->isValid() &&
            _state.hdrGamma->isValid() &&
            _state.lighting->isValid() &&
            _state.bloom   ->isValid() &&
            _state.shadows ->isValid();

    _state.dummyCubeMap = createShadowMap3D(_state.shadowCubeMapX, _state.shadowCubeMapY);

    // Create a pool of shadow maps for point lights to use
    for (int i = 0; i < _state.numShadowMaps; ++i) {
        createShadowMap3D(_state.shadowCubeMapX, _state.shadowCubeMapY);
    }
}

Renderer::~Renderer() {
    if (_context) {
        SDL_GL_DeleteContext(_context);
        _context = nullptr;
    }
    for (Pipeline * shader : _shaders) delete shader;
    _shaders.clear();
    invalidateAllTextures();

    // Delete the main frame buffer
    _clearGBuffer();
}

void Renderer::recompileShaders() {
    for (Pipeline* p : _state.shaders) {
        p->recompile();
    }
}

const GFXConfig & Renderer::config() const {
    return _config;
}

bool Renderer::valid() const {
    return _isValid;
}

void Renderer::setClearColor(const Color &c) {
    _state.clearColor = c;
}

const Pipeline *Renderer::getCurrentShader() const {
    return nullptr;
}

void Renderer::_recalculateProjMatrices() {
    _state.perspective = glm::perspective(glm::radians(_state.fov),
            float(_state.windowWidth) / float(_state.windowHeight),
            _state.znear,
            _state.zfar);
    // arguments: left, right, bottom, top, near, far - this matrix
    // transforms [0,width] to [-1, 1] and [0, height] to [-1, 1]
    _state.orthographic = glm::ortho(0.0f, float(_state.windowWidth),
            float(_state.windowHeight), 0.0f, -1.0f, 1.0f);
}

void Renderer::_clearGBuffer() {
    _state.buffer = GBuffer();

    // for (auto postFx : _state.postFxBuffers) {
    //     glDeleteFramebuffers(1, &postFx.fbo);
    //     glDeleteTextures(1, &postFx.colorBuffer);
    //     glDeleteTextures(postFx.additionalBuffers.size(), &postFx.additionalBuffers[0]);
    // }
    _state.postFxBuffers.clear();
}

void Renderer::_setWindowDimensions(int w, int h) {
    if (_state.windowWidth == w && _state.windowHeight == h) return;
    if (w < 0 || h < 0) return;
    _state.windowWidth = w;
    _state.windowHeight = h;
    _recalculateProjMatrices();
    glViewport(0, 0, w, h);

    // Regenerate the main frame buffer
    _clearGBuffer();

    GBuffer & buffer = _state.buffer;
    // glGenFramebuffers(1, &buffer.fbo);
    // glBindFramebuffer(GL_FRAMEBUFFER, buffer.fbo);

    // Position buffer
    buffer.position = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::RGB, TextureComponentSize::BITS_32, TextureComponentType::FLOAT, _state.windowWidth, _state.windowHeight, false}, nullptr);
    buffer.position.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);

    // Normal buffer
    buffer.normals = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::RGB, TextureComponentSize::BITS_32, TextureComponentType::FLOAT, _state.windowWidth, _state.windowHeight, false}, nullptr);
    buffer.normals.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);

    // Create the color buffer - notice that is uses higher
    // than normal precision. This allows us to write color values
    // greater than 1.0 to support things like HDR.
    buffer.albedo = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::RGB, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, _state.windowWidth, _state.windowHeight, false}, nullptr);
    buffer.albedo.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);

    // Base reflectivity buffer
    buffer.baseReflectivity = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::RGB, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, _state.windowWidth, _state.windowHeight, false}, nullptr);
    buffer.baseReflectivity.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);

    // Roughness-Metallic-Ambient buffer
    buffer.roughnessMetallicAmbient = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::RGB, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, _state.windowWidth, _state.windowHeight, false}, nullptr);
    buffer.roughnessMetallicAmbient.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);

    // Create the depth buffer
    buffer.depth = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::DEPTH, TextureComponentSize::BITS_DEFAULT, TextureComponentType::FLOAT, _state.windowWidth, _state.windowHeight, false}, nullptr);
    buffer.depth.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);

    // Create the frame buffer with all its texture attachments
    buffer.fbo = FrameBuffer({buffer.position, buffer.normals, buffer.albedo, buffer.baseReflectivity, buffer.roughnessMetallicAmbient, buffer.depth});
    if (!buffer.fbo.valid()) {
        _isValid = false;
        return;
    }

    // Code to create the lighting fbo
    _state.lightingColorBuffer = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::RGB, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, _state.windowWidth, _state.windowHeight, false}, nullptr);
    _state.lightingColorBuffer.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);
    _state.lightingColorBuffer.setCoordinateWrapping(TextureCoordinateWrapping::CLAMP_TO_EDGE);

    // Create the buffer we will use to add bloom as a post-processing effect
    _state.lightingHighBrightnessBuffer = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::RGB, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, _state.windowWidth, _state.windowHeight, false}, nullptr);
    _state.lightingHighBrightnessBuffer.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);
    _state.lightingHighBrightnessBuffer.setCoordinateWrapping(TextureCoordinateWrapping::CLAMP_TO_EDGE);

    // Create the depth buffer
    _state.lightingDepthBuffer = Texture(TextureConfig{TextureType::TEXTURE_2D, TextureComponentFormat::DEPTH, TextureComponentSize::BITS_DEFAULT, TextureComponentType::FLOAT, _state.windowWidth, _state.windowHeight, false}, nullptr);
    _state.lightingDepthBuffer.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);

    // Attach the textures to the FBO
    _state.lightingFbo = FrameBuffer({_state.lightingColorBuffer, _state.lightingHighBrightnessBuffer, _state.lightingDepthBuffer});
    if (!_state.lightingFbo.valid()) {
        _isValid = false;
        return;
    }

    _initializePostFxBuffers();
}

void Renderer::_initializePostFxBuffers() {
    uint32_t currWidth = _state.windowWidth;
    uint32_t currHeight = _state.windowHeight;
    _state.numDownsampleIterations = 0;
    _state.numUpsampleIterations = 0;

    // Initialize bloom
    for (; _state.numDownsampleIterations < 6; ++_state.numDownsampleIterations) {
        currWidth /= 2;
        currHeight /= 2;
        if (currWidth < 8 || currHeight < 8) break;
        PostFXBuffer buffer;
        auto color = Texture(TextureConfig{ TextureType::TEXTURE_2D, TextureComponentFormat::RGB, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, currWidth, currHeight, false }, nullptr);
        color.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);
        color.setCoordinateWrapping(TextureCoordinateWrapping::CLAMP_TO_EDGE); // TODO: Does this make sense for bloom textures?
        buffer.fbo = FrameBuffer({ color });
        if (!buffer.fbo.valid()) {
            _isValid = false;
            std::cerr << "Unable to initialize bloom buffer" << std::endl;
            return;
        }
        _state.postFxBuffers.push_back(buffer);
    }

    for (;;) {
        currWidth *= 2;
        currHeight *= 2;
        PostFXBuffer buffer;
        ++_state.numUpsampleIterations;
        auto color = Texture(TextureConfig{ TextureType::TEXTURE_2D, TextureComponentFormat::RGB, TextureComponentSize::BITS_16, TextureComponentType::FLOAT, currWidth, currHeight, false }, nullptr);
        color.setMinMagFilter(TextureMinificationFilter::LINEAR, TextureMagnificationFilter::LINEAR);
        color.setCoordinateWrapping(TextureCoordinateWrapping::CLAMP_TO_EDGE); // TODO: Does this make sense for bloom textures?
        buffer.fbo = FrameBuffer({ color });
        if (!buffer.fbo.valid()) {
            _isValid = false;
            std::cerr << "Unable to initialize bloom buffer" << std::endl;
            return;
        }
        _state.postFxBuffers.push_back(buffer);
        if (currWidth == _state.windowWidth || currHeight == _state.windowHeight) break;
    }
}

void Renderer::setPerspectiveData(float fov, float fnear, float ffar) {
    // TODO: Find the best lower bound for fov instead of arbitrary 25.0f
    if (fov < 25.0f) return;
    _state.fov = fov;
    _state.znear = fnear;
    _state.zfar = ffar;
    _recalculateProjMatrices();
}

void Renderer::setRenderMode(RenderMode mode) {
    _state.mode = mode;
}

void Renderer::begin(bool clearScreen) {
    // Make sure we set our context as the active one
    SDL_GL_MakeCurrent(_window, _context);

    // Check for changes in the window size
    int w, h;
    SDL_GetWindowSize(_window, &w, &h);
    // This won't resize anything if the width/height
    // didn't change
    _setWindowDimensions(w, h);

    // Always clear the main screen buffer, but only
    // conditionally clean the custom frame buffer
    glClearColor(_state.clearColor.r, _state.clearColor.g,
                 _state.clearColor.b, _state.clearColor.a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (clearScreen) {
        glm::vec4 color = glm::vec4(_state.clearColor.r, _state.clearColor.g, _state.clearColor.b, _state.clearColor.a);
        _state.buffer.fbo.clear(color);
        _state.lightingFbo.clear(color);

        for (auto& postFx : _state.postFxBuffers) {
            postFx.fbo.clear(color);
        }
    }

    // Clear all entities from the previous frame
    for (auto & e : _state.entities) {
        e.second.clear();
    }

    // Clear all instanced entities
    _state.instancedMeshes.clear();

    // Clear all lights
    _state.lights.clear();
    _state.lightInteractingEntities.clear();

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

void Renderer::addDrawable(RenderEntity * e) {
    _addDrawable(e, glm::mat4(1.0f));
}

static void addEntityMeshData(RenderEntity * e, std::unordered_map<__RenderEntityObserver, std::unordered_map<__MeshObserver, __MeshContainer>> & map) {
    __RenderEntityObserver c(e);
    if (map.find(c) == map.end()) {
        map.insert(std::make_pair(c, std::unordered_map<__MeshObserver, __MeshContainer>{}));
    }
    std::unordered_map<__MeshObserver, __MeshContainer> & existing = map.find(c)->second;
    
    for (std::shared_ptr<Mesh> m : e->meshes) {
        __MeshObserver o(m.get());
        if (existing.find(o) == existing.end()) {
            existing.insert(std::make_pair(o, __MeshContainer(m.get())));
        }
        __MeshContainer & container = existing.find(o)->second;
        container.modelMatrices.push_back(e->model);
        container.diffuseColors.push_back(m->getMaterial().diffuseColor);
        container.baseReflectivity.push_back(m->getMaterial().baseReflectivity);
        container.roughness.push_back(m->getMaterial().roughness);
        container.metallic.push_back(m->getMaterial().metallic);
        ++container.size;
    }
}

void Renderer::_addDrawable(RenderEntity * e, const glm::mat4 & accum) {
    auto it = _state.entities.find(e->getLightProperties());
    if (it == _state.entities.end()) {
        // Not necessarily an error since if an entity is set to
        // invisible, we won't bother adding them
        //std::cerr << "[error] Unable to add entity" << std::endl;
        return;
    }
    e->model = glm::mat4(1.0f);
    matRotate(e->model, e->rotation);
    matScale(e->model, e->scale);
    matTranslate(e->model, e->position);
    e->model = accum * e->model;
    it->second.push_back(e);
    if (e->getLightProperties() & DYNAMIC) {
        _state.lightInteractingEntities.push_back(e);
    }

    // We want to keep track of entities and whether or not they have moved for determining
    // when shadows should be recomputed
    if (_entitiesSeenBefore.find(e) == _entitiesSeenBefore.end()) {
        _entitiesSeenBefore.insert(std::make_pair(e, EntityStateInfo{e->position, e->scale, e->rotation, true}));
    }
    else {
        EntityStateInfo & info = _entitiesSeenBefore.find(e)->second;
        const double distance = glm::distance(e->position, info.lastPos);
        const double scale = glm::distance(e->scale, info.lastScale);
        const double rotation = glm::distance(e->rotation, info.lastRotation);
        info.dirty = false;
        if (distance > 0.25) {
            info.lastPos = e->position;
            info.dirty = true;
        }
        if (scale > 0.25) {
            info.lastScale = e->scale;
            info.dirty = true;
        }
        if (rotation > 0.25) {
            info.lastRotation = e->rotation;
            info.dirty = true;
        }
    }

    //addEntityMeshData(e, _state.instancedMeshes);

    for (RenderEntity & node : e->nodes) {
        _addDrawable(&node, e->model);
    }
}

/**
 * During the lighting phase, we need each of the 6 faces of the shadow map to have its own view transform matrix.
 * This enables us to convert vertices to be in various different light coordinate spaces.
 */
static std::vector<glm::mat4> generateLightViewTransforms(const glm::mat4 & projection, const glm::vec3 & lightPos) {
    return std::vector<glm::mat4>{
        //                       pos       pos + dir                               up
        projection * glm::lookAt(lightPos, lightPos + glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
        projection * glm::lookAt(lightPos, lightPos + glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
        projection * glm::lookAt(lightPos, lightPos + glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)),
        projection * glm::lookAt(lightPos, lightPos + glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)),
        projection * glm::lookAt(lightPos, lightPos + glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
        projection * glm::lookAt(lightPos, 
        lightPos + glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f))
    };
}

void Renderer::_initInstancedData(__MeshContainer & c, std::vector<GLuint> & buffers) {
    Pipeline * pbr = _state.geometry.get();

    auto & modelMats = c.modelMatrices;
    auto & diffuseColors = c.diffuseColors;
    auto & baseReflectivity = c.baseReflectivity;
    auto & roughness = c.roughness;
    auto & metallic = c.metallic;

    // All shaders should use the same location for model, so this should work
    int pos = pbr->getAttribLocation("model");
    const int pos1 = pos + 0;
    const int pos2 = pos + 1;
    const int pos3 = pos + 2;
    const int pos4 = pos + 3;

    c.m->bind();

    GLuint buffer;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, modelMats.size() * sizeof(glm::mat4), &modelMats[0], GL_STATIC_DRAW);

    glEnableVertexAttribArray(pos1);
    glVertexAttribPointer(pos1, 4, GL_FLOAT, GL_FALSE, 64, (void *)0);
    glEnableVertexAttribArray(pos2);
    glVertexAttribPointer(pos2, 4, GL_FLOAT, GL_FALSE, 64, (void *)16);
    glEnableVertexAttribArray(pos3);
    glVertexAttribPointer(pos3, 4, GL_FLOAT, GL_FALSE, 64, (void *)32);
    glEnableVertexAttribArray(pos4);
    glVertexAttribPointer(pos4, 4, GL_FLOAT, GL_FALSE, 64, (void *)48);
    glVertexAttribDivisor(pos1, 1);
    glVertexAttribDivisor(pos2, 1);
    glVertexAttribDivisor(pos3, 1);
    glVertexAttribDivisor(pos4, 1);

    buffers.push_back(buffer);

    buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, diffuseColors.size() * sizeof(glm::vec3), &diffuseColors[0], GL_STATIC_DRAW);
    pos = pbr->getAttribLocation("diffuseColor");
    glEnableVertexAttribArray(pos);
    glVertexAttribPointer(pos, 3, GL_FLOAT, GL_FALSE, 0, (void *)0);
    glVertexAttribDivisor(pos, 1);
    buffers.push_back(buffer);

    buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, baseReflectivity.size() * sizeof(glm::vec3), &baseReflectivity[0], GL_STATIC_DRAW);
    pos = pbr->getAttribLocation("baseReflectivity");
    glEnableVertexAttribArray(pos);
    glVertexAttribPointer(pos, 3, GL_FLOAT, GL_FALSE, 0, (void *)0);
    glVertexAttribDivisor(pos, 1);
    buffers.push_back(buffer);

    buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, metallic.size() * sizeof(float), &metallic[0], GL_STATIC_DRAW);
    // All shaders should use the same location for shininess, so this should work
    pos = pbr->getAttribLocation("metallic");
    glEnableVertexAttribArray(pos);
    glVertexAttribPointer(pos, 1, GL_FLOAT, GL_FALSE, 0, (void *)0);
    glVertexAttribDivisor(pos, 1);
    buffers.push_back(buffer);

    buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, roughness.size() * sizeof(float), &roughness[0], GL_STATIC_DRAW);
    // All shaders should use the same location for shininess, so this should work
    pos = pbr->getAttribLocation("roughness");
    glEnableVertexAttribArray(pos);
    glVertexAttribPointer(pos, 1, GL_FLOAT, GL_FALSE, 0, (void *)0);
    glVertexAttribDivisor(pos, 1);
    buffers.push_back(buffer);

    c.m->unbind();
}

void Renderer::_clearInstancedData(std::vector<GLuint> & buffers) {
    glDeleteBuffers(buffers.size(), &buffers[0]);
    buffers.clear();
}

void Renderer::_bindShader(Pipeline * s) {
    _unbindShader();
    s->bind();
    _state.currentShader = s;
}

void Renderer::_unbindShader() {
    if (!_state.currentShader) return;
    //_unbindAllTextures();
    _state.currentShader->unbind();
    _state.currentShader = nullptr;
}

static void setCullState(const RenderFaceCulling & mode) {
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

void Renderer::_buildEntityList(const Camera & c) {
    for (auto & entityList : _state.entities) {
        for (RenderEntity * e : entityList.second) {
            const double distance = glm::distance(e->position, c.getPosition());
            if (distance < _state.zfar) {
                addEntityMeshData(e, _state.instancedMeshes);
            }
        }
    }
}

void Renderer::_render(const Camera & c, const RenderEntity * e, const Mesh * m, const size_t numInstances) {
    const glm::mat4 & projection = _state.perspective;
    const glm::mat4 & view = c.getViewTransform();

    // Unbind current shader if one is bound
    _unbindShader();

    // Set up the shader we will use for this batch of entities
    Pipeline * s;
    uint32_t lightProperties = e->getLightProperties();
    uint32_t renderProperties = m->getRenderProperties();
    if (lightProperties == FLAT) {
        s = _state.forward.get();
    }
    else {
        s = _state.geometry.get();
    }

    //s->print();
    _bindShader(s);

    s->setMat4("projection", &projection[0][0]);
    s->setMat4("view", &view[0][0]);

    if (renderProperties & TEXTURED) {
        //_bindTexture(s, "diffuseTexture", m->getMaterial().texture);
        s->bindTexture("diffuseTexture", _lookupTexture(m->getMaterial().texture));
        s->setBool("textured", true);
    }
    else {
        s->setBool("textured", false);
    }

    // Determine which uniforms we should set
    if (lightProperties & FLAT) {
        s->setVec3("diffuseColor", &m->getMaterial().diffuseColor[0]);
    } else if (lightProperties & DYNAMIC) {
        if (renderProperties & NORMAL_MAPPED) {
            //_bindTexture(s, "normalMap", m->getMaterial().normalMap);
            s->bindTexture("normalMap", _lookupTexture(m->getMaterial().normalMap));
            s->setBool("normalMapped", true);
        }
        else {
            s->setBool("normalMapped", false);
        }

        if (renderProperties & HEIGHT_MAPPED) {
            //_bindTexture(s, "depthMap", m->getMaterial().depthMap);
            s->bindTexture("depthMap", _lookupTexture(m->getMaterial().depthMap));
            s->setFloat("heightScale", m->getMaterial().heightScale);
            s->setBool("depthMapped", true);
        }
        else {
            s->setBool("depthMapped", false);
        }

        if (renderProperties & ROUGHNESS_MAPPED) {
            //_bindTexture(s, "roughnessMap", m->getMaterial().roughnessMap);
            s->bindTexture("roughnessMap", _lookupTexture(m->getMaterial().roughnessMap));
            s->setBool("roughnessMapped", true);
        }
        else {
            s->setBool("roughnessMapped", false);
        }

        if (renderProperties & AMBIENT_MAPPED) {
            //_bindTexture(s, "ambientOcclusionMap", m->getMaterial().ambientMap);
            s->bindTexture("ambientOcclusionMap", _lookupTexture(m->getMaterial().ambientMap));
            s->setBool("ambientMapped", true);
        }
        else {
            s->setBool("ambientMapped", false);
        }

        if (renderProperties & SHININESS_MAPPED) {
            //_bindTexture(s, "metalnessMap", m->getMaterial().metalnessMap);
            s->bindTexture("metalnessMap", _lookupTexture(m->getMaterial().metalnessMap));
            s->setBool("metalnessMapped", true);
        }
        else {
            s->setBool("metalnessMapped", false);
        }

        s->setVec3("viewPosition", &c.getPosition()[0]);
    }

    // Perform instanced rendering
    setCullState(m->cullingMode);

    m->bind();
    m->render(numInstances);
    m->unbind();

    _unbindShader();
}

void Renderer::end(const Camera & c) {
    // Pull the view transform/projection matrices
    const glm::mat4 * projection = &_state.perspective;
    const glm::mat4 * view = &c.getViewTransform();
    const int maxInstances = 250;
    const int maxShadowCastingLights = 8;
    const int maxTotalLights = 256;
    const int maxShadowUpdatesPerFrame = maxShadowCastingLights;
    // Need to delete these at the end of the frame
    std::vector<GLuint> buffers;

    //_unbindAllTextures();

    // We need to figure out what we want to attempt to render
    _buildEntityList(c);

    std::unordered_map<Light *, std::unordered_map<__RenderEntityObserver, std::unordered_map<__MeshObserver, __MeshContainer>>> perLightInstancedMeshes;
    std::unordered_map<Light *, bool> perLightIsDirty;
    std::vector<std::pair<Light *, double>> perLightDistToViewer;
    // This one is just for shadow-casting lights
    std::vector<std::pair<Light *, double>> perLightShadowCastingDistToViewer;
    // Init per light instance data
    for (Light * light : _state.lights) {
        const double distance = glm::distance(c.getPosition(), light->position);
        perLightDistToViewer.push_back(std::make_pair(light, distance));
        //if (distance > 2 * light->getRadius()) continue;
        perLightInstancedMeshes.insert(std::make_pair(light, std::unordered_map<__RenderEntityObserver, std::unordered_map<__MeshObserver, __MeshContainer>>()));
        perLightIsDirty.insert(std::make_pair(light, _lightsSeenBefore.find(light)->second.dirty));
        if (light->castsShadows()) {
            perLightShadowCastingDistToViewer.push_back(std::make_pair(light, distance));
        }
    }

    // Sort lights based on distance to viewer
    const auto comparison = [](const std::pair<Light *, double> & a, const std::pair<Light *, double> & b) {
        return a.second < b.second;
    };
    std::sort(perLightDistToViewer.begin(), perLightDistToViewer.end(), comparison);
    std::sort(perLightShadowCastingDistToViewer.begin(), perLightShadowCastingDistToViewer.end(), comparison);

    // Remove lights exceeding the absolute maximum
    if (perLightDistToViewer.size() > maxTotalLights) {
        perLightDistToViewer.resize(maxTotalLights);
    }

    // Remove shadow-casting lights that exceed our max count
    if (perLightShadowCastingDistToViewer.size() > maxShadowCastingLights) {
        perLightShadowCastingDistToViewer.resize(maxShadowCastingLights);
    }

    for (RenderEntity * e : _state.lightInteractingEntities) {
        const bool entityIsDirty = _entitiesSeenBefore.find(e)->second.dirty;
        for (auto&[light, _] : perLightShadowCastingDistToViewer) {
            const double distance = glm::distance(e->position, light->position);
            if (distance > light->getRadius()) continue;
            addEntityMeshData(e, perLightInstancedMeshes.find(light)->second);
            perLightIsDirty.find(light)->second |= entityIsDirty;
        }
    }

    // Set blend func just for shadow pass
    glBlendFunc(GL_ONE, GL_ONE);
    glEnable(GL_DEPTH_TEST);
    // Perform the shadow volume pre-pass
    _bindShader(_state.shadows.get());
    int shadowUpdates = 0;
    for (auto&[light, d] : perLightShadowCastingDistToViewer) {
        if (shadowUpdates > maxShadowUpdatesPerFrame) break;
        ++shadowUpdates;
        const double distance = glm::distance(c.getPosition(), light->position);
        // We want to compute shadows at least once for each light source before we enable the option of skipping it 
        // due to it being too far away
        const bool dirty = _lightsSeenBefore.find(light)->second.dirty || perLightIsDirty.find(light)->second;
        //if (distance > 2 * light->getRadius() || !dirty) continue;
        if (!dirty) continue;

        // Set dirty to false
        _lightsSeenBefore.find(light)->second.dirty = false;

        auto & instancedMeshes = perLightInstancedMeshes.find(light)->second;

        // Init the instance data which enables us to drastically reduce the number of draw calls
        for (auto & entityObservers : instancedMeshes) {
            for (auto & meshObservers : entityObservers.second) {
                _initInstancedData(meshObservers.second, buffers);
            }
        }
    
        // TODO: Make this work with spotlights
        PointLight * point = (PointLight *)light;
        const ShadowMap3D & smap = this->_shadowMap3DHandles.find(_getShadowMapHandleForLight(point))->second;

        const glm::mat4 lightPerspective = glm::perspective<float>(glm::radians(90.0f), float(smap.shadowCubeMap.width()) / smap.shadowCubeMap.height(), point->getNearPlane(), point->getFarPlane());

        // glBindFramebuffer(GL_FRAMEBUFFER, smap.frameBuffer);
        smap.frameBuffer.clear(glm::vec4(1.0f));
        smap.frameBuffer.bind();
        glViewport(0, 0, smap.shadowCubeMap.width(), smap.shadowCubeMap.height());
        // Current pass only cares about depth buffer
        // glClear(GL_DEPTH_BUFFER_BIT);

        auto transforms = generateLightViewTransforms(lightPerspective, point->position);
        for (int i = 0; i < transforms.size(); ++i) {
            const std::string index = "[" + std::to_string(i) + "]";
            _state.shadows->setMat4("shadowMatrices" + index, &transforms[i][0][0]);
        }
        _state.shadows->setVec3("lightPos", &light->position[0]);
        _state.shadows->setFloat("farPlane", point->getFarPlane());

        /*
        for (auto & p : _state.entities) {
            uint32_t properties = p.first;
            if ( !(properties & DYNAMIC) ) continue;
            for (auto & e : p.second) {
                _state.shadows->setMat4("model", &e->model[0][0]);
                e->render();
            }
        }
        */

        for (auto & entityObservers : instancedMeshes) {
            //uint32_t properties = entityObservers.first.e->getLightProperties();
            //if ( !(properties & DYNAMIC) ) continue;
            for (auto & meshObservers : entityObservers.second) {
                // Set up temporary instancing buffers
                //_initInstancedData(meshObservers.second, buffers);
                Mesh * m = meshObservers.first.m;
                setCullState(m->cullingMode);
                m->bind();
                m->render(meshObservers.second.size);
                m->unbind();
                //_clearInstancedData(buffers);
                /**
                const size_t size = modelMats.size();
                for (int i = 0; i < size; i += maxInstances) {
                    const size_t instances = std::min<size_t>(maxInstances, size - i);
                    _state.shadows->setMat4("modelMats", &modelMats[i][0][0], instances);
                    e.second.e->render(instances);
                }
                */
            }
        }

        // Unbind
        //glBindFramebuffer(GL_FRAMEBUFFER, 0);
        smap.frameBuffer.unbind();
        _clearInstancedData(buffers);
    }
    _clearInstancedData(buffers);
    //_unbindAllTextures();
    _unbindShader();

    // Init the instance data which enables us to drastically reduce the number of draw calls
    for (auto & entityObservers : _state.instancedMeshes) {
        for (auto & meshObservers : entityObservers.second) {
            _initInstancedData(meshObservers.second, buffers);
        }
    }

    // TEMP: Set up the light source
    //glm::vec3 lightPos(0.0f, 0.0f, 0.0f);
    //glm::vec3 lightColor(10.0f); 

    // Make sure to bind our own frame buffer for rendering
    _state.buffer.fbo.bind();
    
    // Make sure some of our global GL states are set properly for primary rendering below
    glBlendFunc(_state.blendSFactor, _state.blendDFactor);
    glViewport(0, 0, _state.windowWidth, _state.windowHeight);

    // Begin geometry pass
    //glEnable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    for (auto & entityObservers : _state.instancedMeshes) {
        for (auto & meshObservers : entityObservers.second) {
            //_initInstancedData(meshObservers.second, buffers);
            RenderEntity * e = entityObservers.first.e;
            Mesh * m = meshObservers.first.m;
            const size_t numInstances = meshObservers.second.size;

            // We are only going to render dynamic-lit entities this pass
            if (e->getLightProperties() & FLAT) continue;
            _render(c, e, m, numInstances);
        }
    }
    _state.buffer.fbo.unbind();

    glDisable(GL_CULL_FACE);
    //glEnable(GL_BLEND);

    // Begin deferred lighting pass
    _state.lightingFbo.bind();
    glDisable(GL_DEPTH_TEST);
    //_unbindAllTextures();
    _bindShader(_state.lighting.get());
    _initLights(_state.lighting.get(), c, perLightDistToViewer, maxShadowCastingLights);
    _state.lighting->bindTexture("gPosition", _state.buffer.position);
    _state.lighting->bindTexture("gNormal", _state.buffer.normals);
    _state.lighting->bindTexture("gAlbedo", _state.buffer.albedo);
    _state.lighting->bindTexture("gBaseReflectivity", _state.buffer.baseReflectivity);
    _state.lighting->bindTexture("gRoughnessMetallicAmbient", _state.buffer.roughnessMetallicAmbient);
    _state.screenQuad->bind();
    _state.screenQuad->render(1);
    _state.screenQuad->unbind();
    _state.lightingFbo.unbind();
    _unbindShader();

    // Forward pass for all objects that don't interact with light (may also be used for transparency later as well)
    _state.lightingFbo.copyFrom(_state.buffer.fbo, BufferBounds{0, 0, _state.windowWidth, _state.windowHeight}, BufferBounds{0, 0, _state.windowWidth, _state.windowHeight}, BufferBit::DEPTH_BIT, BufferFilter::NEAREST);
    // Blit to default framebuffer - not that the framebuffer you are writing to has to match the internal format
    // of the framebuffer you are reading to!
    glEnable(GL_DEPTH_TEST);
    _state.lightingFbo.bind();
    for (auto & entityObservers : _state.instancedMeshes) {
        for (auto & meshObservers : entityObservers.second) {
            //_initInstancedData(meshObservers.second, buffers);
            RenderEntity * e = entityObservers.first.e;
            Mesh * m = meshObservers.first.m;
            const size_t numInstances = meshObservers.second.size;

            // We are only going to render flat entities during this pass
            if (e->getLightProperties() & DYNAMIC) continue;
            _render(c, e, m, numInstances);
        }
    }
    _state.lightingFbo.unbind();
    // glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Enable post-FX effects such as bloom
    _performPostFxProcessing();

    // Perform final drawing to screen + gamma correction
    _finalizeFrame();

    // Make sure to clear out all instanced data used this frame
    _clearInstancedData(buffers);
}

void Renderer::_performPostFxProcessing() {
    glDisable(GL_CULL_FACE);
    // glEnable(GL_BLEND);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
   
    Pipeline* bloom = _state.bloom.get();
    _bindShader(bloom);

    bloom->setBool("downsamplingStage", true);
    bloom->setBool("upsamplingStage", false);
    for (int i = 0; i < _state.numDownsampleIterations; ++i) {
        PostFXBuffer& buffer = _state.postFxBuffers[i];
        auto width = buffer.fbo.getColorAttachments()[0].width();
        auto height = buffer.fbo.getColorAttachments()[0].height();
        bloom->setFloat("viewportX", float(width));
        bloom->setFloat("viewportY", float(height));
        buffer.fbo.bind();
        glViewport(0, 0, width, height);
        if (i == 0) {
            bloom->bindTexture("mainTexture", _state.lightingColorBuffer);
        }
        else {
            bloom->bindTexture("mainTexture", _state.postFxBuffers[i - 1].fbo.getColorAttachments()[0]);
        }
        _renderQuad();
        buffer.fbo.unbind();
    }

    bloom->setBool("downsamplingStage", false);
    bloom->setBool("upsamplingStage", true);
    int postFXIndex = _state.numDownsampleIterations;
    for (int i = _state.numDownsampleIterations - 1; i >= 0; --i, ++postFXIndex) {
        PostFXBuffer& buffer = _state.postFxBuffers[postFXIndex];
        auto width = buffer.fbo.getColorAttachments()[0].width();
        auto height = buffer.fbo.getColorAttachments()[0].height();
        bloom->setFloat("viewportX", float(width));
        bloom->setFloat("viewportY", float(height));
        buffer.fbo.bind();
        glViewport(0, 0, width, height);
        bloom->bindTexture("mainTexture", _state.postFxBuffers[i].fbo.getColorAttachments()[0]);
        if (i == 0) {
            bloom->bindTexture("bloomTexture", _state.lightingColorBuffer);
        }
        else {
            bloom->bindTexture("bloomTexture", _state.postFxBuffers[i - 1].fbo.getColorAttachments()[0]);
        }
        _renderQuad();
        buffer.fbo.unbind();
        
        _state.finalScreenTexture = buffer.fbo.getColorAttachments()[0];
    }

    _unbindShader();
}

void Renderer::_finalizeFrame() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_CULL_FACE);
    glViewport(0, 0, _state.windowWidth, _state.windowHeight);
    //glEnable(GL_BLEND);

    // Now render the screen
    _bindShader(_state.hdrGamma.get());
    _state.hdrGamma->bindTexture("screen", _state.finalScreenTexture);
    _renderQuad();
    _unbindShader();
}

void Renderer::_renderQuad() {
    _state.screenQuad->bind();
    _state.screenQuad->render(1);
    _state.screenQuad->unbind();
}

static Texture _loadTexture(const std::string & file) {
    Texture texture;
    int width, height, numChannels;
    // @see http://www.redbancosdealimentos.org/homes-flooring-design-sources
    uint8_t * data = stbi_load(file.c_str(), &width, &height, &numChannels, 0);
    if (data) {
        TextureConfig config;
        config.type = TextureType::TEXTURE_2D;
        config.storage = TextureComponentSize::BITS_DEFAULT;
        config.generateMipMaps = true;
        config.dataType = TextureComponentType::UINT;
        config.width = (uint32_t)width;
        config.height = (uint32_t)height;
        // This loads the textures with sRGB in mind so that they get converted back
        // to linear color space. Warning: if the texture was not actually specified as an
        // sRGB texture (common for normal/specular maps), this will cause problems.
        switch (numChannels) {
            case 1:
                config.format = TextureComponentFormat::RED;
                break;
            case 3:
                config.format = TextureComponentFormat::SRGB;
                break;
            case 4:
                config.format = TextureComponentFormat::SRGB_ALPHA;
                break;
            default:
                std::cerr << "[error] Unknown texture loading error - format may be invalid" << std::endl;
                stbi_image_free(data);
                return Texture();
        }

        texture = Texture(config, data);
        texture.setCoordinateWrapping(TextureCoordinateWrapping::REPEAT);
        texture.setMinMagFilter(TextureMinificationFilter::LINEAR_MIPMAP_LINEAR, TextureMagnificationFilter::LINEAR);
    } else {
        std::cerr << "[error] Could not load texture: " << file << std::endl;
        return Texture();
    }
    
    stbi_image_free(data);
    return texture;
}

TextureHandle Renderer::loadTexture(const std::string &file) {
    auto it = _textures.find(file);
    if (it != _textures.end()) return it->second.handle;

    TextureCache tex;
    tex.file = file;
    tex.handle = this->_nextTextureHandle++;
    tex.texture = _loadTexture(file);
    if (!tex.texture.valid()) return -1;

    _textures.insert(std::make_pair(file, tex));
    _textureHandles.insert(std::make_pair(tex.handle, tex));
    return tex.handle;
}

Model Renderer::loadModel(const std::string & file) {
    auto it = this->_models.find(file);
    if (it != this->_models.end()) {
        return it->second;
    }

    std::cout << "Loading " << file << std::endl;
    Model m(*this, file);
    this->_models.insert(std::make_pair(file, m));
    return std::move(m);
}

ShadowMapHandle Renderer::createShadowMap3D(uint32_t resolutionX, uint32_t resolutionY) {
    ShadowMap3D smap;
    smap.shadowCubeMap = Texture(TextureConfig{TextureType::TEXTURE_3D, TextureComponentFormat::DEPTH, TextureComponentSize::BITS_DEFAULT, TextureComponentType::FLOAT, resolutionX, resolutionY, false}, nullptr);
    smap.shadowCubeMap.setMinMagFilter(TextureMinificationFilter::NEAREST, TextureMagnificationFilter::NEAREST);
    smap.shadowCubeMap.setCoordinateWrapping(TextureCoordinateWrapping::CLAMP_TO_EDGE);

    smap.frameBuffer = FrameBuffer({smap.shadowCubeMap});
    if (!smap.frameBuffer.valid()) {
        _isValid = false;
        return -1;
    }
    TextureHandle handle = this->_nextTextureHandle++;
    this->_shadowMap3DHandles.insert(std::make_pair(handle, smap));
    return handle;
}

void Renderer::invalidateAllTextures() {
    for (auto & texture : _textures) {
        //glDeleteTextures(1, &texture.second.texture);
        texture.second.texture = Texture();
        // Make sure we mark it as unloaded just in case someone tries
        // to use it in the future
        texture.second.loaded = false;
    }
}

Texture Renderer::_lookupTexture(TextureHandle handle) const {
    if (handle == -1) return Texture();

    auto it = _textureHandles.find(handle);
    // TODO: Make sure that 0 actually signifies an invalid texture in OpenGL
    if (it == _textureHandles.end()) {
        if (_shadowMap3DHandles.find(handle) == _shadowMap3DHandles.end()) return Texture();
        return _shadowMap3DHandles.find(handle)->second.shadowCubeMap;
    }

    // If not in memory then bring it in
    if (!it->second.loaded) {
        TextureCache tex = it->second;
        tex.texture = _loadTexture(tex.file);
        tex.loaded = tex.texture.valid();
        _textures.erase(tex.file);
        _textures.insert(std::make_pair(tex.file, tex));
        _textureHandles.erase(handle);
        _textureHandles.insert(std::make_pair(handle, tex));
        return tex.texture;
    }
    return it->second.texture;
}

// TODO: Need a way to clean up point light resources
void Renderer::addPointLight(Light * light) {
    assert(light->getType() == LightType::POINTLIGHT || light->getType() == LightType::SPOTLIGHT);
    _state.lights.push_back(light);

    // if (light->getType() == LightType::POINTLIGHT) {
    //     PointLight * point = (PointLight *)light;
    //     if (_getShadowMapHandleForLight(light) == -1) {
    //         _setLightShadowMapHandle(light, this->createShadowMap3D(_state.shadowCubeMapX, _state.shadowCubeMapY));
    //     }
    // }

    if (_lightsSeenBefore.find(light) == _lightsSeenBefore.end()) {
        _lightsSeenBefore.insert(std::make_pair(light, EntityStateInfo{light->position, glm::vec3(0.0f), glm::vec3(0.0f), true}));
    }
    else {
        EntityStateInfo & info = _lightsSeenBefore.find(light)->second;
        // If no associated shadow map, mark as dirty
        if (_lightsToShadowMap.find(light) == _lightsToShadowMap.end()) {
            info.dirty = true;
        }
        const double distance = glm::distance(light->position, info.lastPos);
        if (distance > 0.25) {
            info.lastPos = light->position;
            info.dirty = true;
        }
        //else {
        //    info.dirty = false;
        //}
    }
}

void Renderer::_initLights(Pipeline * s, const Camera & c, const std::vector<std::pair<Light *, double>> & lights, const size_t maxShadowLights) {
    glm::vec3 lightColor;
    int lightIndex = 0;
    int shadowLightIndex = 0;
    int i = 0;
    for (; i < lights.size(); ++i) {
        PointLight * light = (PointLight *)lights[i].first;
        const double distance = lights[i].second; //glm::distance(c.getPosition(), light->position);
        // Skip lights too far from camera
        //if (distance > (2 * light->getRadius())) continue;
        lightColor = light->getColor() * light->getIntensity();
        s->setVec3("lightPositions[" + std::to_string(lightIndex) + "]", &light->position[0]);
        s->setVec3("lightColors[" + std::to_string(lightIndex) + "]", &lightColor[0]);
        s->setFloat("lightRadii[" + std::to_string(lightIndex) + "]", light->getRadius());
        s->setBool("lightCastsShadows[" + std::to_string(lightIndex) + "]", light->castsShadows());
        //_bindShadowMapTexture(s, "shadowCubeMaps[" + std::to_string(lightIndex) + "]", light->getShadowMapHandle());
        if (light->castsShadows() && shadowLightIndex < maxShadowLights) {
            s->setFloat("lightFarPlanes[" + std::to_string(shadowLightIndex) + "]", light->getFarPlane());
            //_bindShadowMapTexture(s, "shadowCubeMaps[" + std::to_string(shadowLightIndex) + "]", _getShadowMapHandleForLight(light));
            s->bindTexture("shadowCubeMaps[" + std::to_string(shadowLightIndex) + "]", _lookupTexture(_getShadowMapHandleForLight(light)));
            ++shadowLightIndex;
        }
        ++lightIndex;
    }

    if (shadowLightIndex == 0) {
       // If we don't do this the fragment shader crashes
       s->setFloat("lightFarPlanes[0]", 0.0f);
       //_bindShadowMapTexture(s, "shadowCubeMaps[0]", _state.dummyCubeMap);
       s->bindTexture("shadowCubeMaps[0]", _lookupTexture(_state.dummyCubeMap));
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
    s->setVec3("viewPosition", &c.getPosition()[0]);
}

ShadowMapHandle Renderer::_getShadowMapHandleForLight(Light * light) {
    assert(_shadowMap3DHandles.size() > 0);

    auto it = _lightsToShadowMap.find(light);
    // If not found, look for an existing shadow map
    if (it == _lightsToShadowMap.end()) {
        // Mark the light as dirty since its map will need to be updated
        _lightsSeenBefore.find(light)->second.dirty = true;

        ShadowMapHandle handle = -1;
        for (const auto & entry : _shadowMap3DHandles) {
            if (_usedShadowMaps.find(entry.first) == _usedShadowMaps.end()) {
                handle = entry.first;
                break;
            }
        }

        if (handle == -1) {
            // Evict oldest since we could not find an available handle
            Light * oldest = _lruLightCache.front();
            _lruLightCache.pop_front();
            handle = _lightsToShadowMap.find(oldest)->second;
            _evictLightFromShadowMapCache(oldest);
        }

        _setLightShadowMapHandle(light, handle);
        _addLightToShadowMapCache(light);
        return handle;
    }

    // Update the LRU cache
    _addLightToShadowMapCache(light);
    return it->second;
}

void Renderer::_setLightShadowMapHandle(Light * light, ShadowMapHandle handle) {
    _lightsToShadowMap.insert(std::make_pair(light, handle));
    _usedShadowMaps.insert(handle);
}

void Renderer::_evictLightFromShadowMapCache(Light * light) {
    for (auto it = _lruLightCache.begin(); it != _lruLightCache.end(); ++it) {
        if (*it == light) {
            _lruLightCache.erase(it);
            return;
        }
    }
}

void Renderer::_addLightToShadowMapCache(Light * light) {
    // First remove the existing light entry if it's already there
    _evictLightFromShadowMapCache(light);
    // Push to back so that it is seen as most recently used
    _lruLightCache.push_back(light);
}
}