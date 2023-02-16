#include "StratusCommon.h"
#include "glm/glm.hpp"
#include <iostream>
#include <StratusPipeline.h>
#include <StratusCamera.h>
#include "StratusAsync.h"
#include <chrono>
#include "StratusEngine.h"
#include "StratusResourceManager.h"
#include "StratusLog.h"
#include "StratusRendererFrontend.h"
#include "StratusWindow.h"
#include <StratusLight.h>
#include <StratusUtils.h>
#include <memory>
#include <filesystem>
#include "CameraController.h"
#include "WorldLightController.h"
#include "LightComponents.h"
#include "LightControllers.h"
#include "StratusTransformComponent.h"
#include "StratusGpuCommon.h"

class Sponza : public stratus::Application {
public:
    virtual ~Sponza() = default;

    const char * GetAppName() const override {
        return "Sponza";
    }

    // Perform first-time initialization - true if success, false otherwise
    virtual bool Initialize() override {
        STRATUS_LOG << "Initializing " << GetAppName() << std::endl;

        LightCreator::Initialize();

        stratus::InputHandlerPtr controller(new CameraController());
        Input()->AddInputHandler(controller);

        const glm::vec3 warmMorningColor = glm::vec3(254.0f / 255.0f, 232.0f / 255.0f, 176.0f / 255.0f);
        controller = stratus::InputHandlerPtr(new WorldLightController(warmMorningColor));
        Input()->AddInputHandler(controller);

        // Moonlight
        //worldLight->setColor(glm::vec3(80.0f / 255.0f, 104.0f / 255.0f, 134.0f / 255.0f));
        //worldLight->setIntensity(0.5f);

        //INSTANCE(RendererFrontend)->SetAtmosphericShadowing(0.2f, 0.3f);

        // Disable culling for this model since there are some weird parts that seem to be reversed
        //stratus::Async<stratus::Entity> e = stratus::ResourceManager::Instance()->LoadModel("../../glTF-Sample-Models/2.0/Sponza/glTF/Sponza.gltf", stratus::RenderFaceCulling::CULLING_CCW);
        //stratus::Async<stratus::Entity>g e = stratus::ResourceManager::Instance()->LoadModel("../local/sponza_scene/scene.gltf", stratus::RenderFaceCulling::CULLING_NONE);
        //stratus::Async<stratus::Entity> e = stratus::ResourceManager::Instance()->LoadModel("../../local/Sponza2022/NewSponza_Main_glTF_002.gltf", stratus::RenderFaceCulling::CULLING_CCW);
        stratus::Async<stratus::Entity> e = stratus::ResourceManager::Instance()->LoadModel("../../Sponza2022/scene.gltf", stratus::RenderFaceCulling::CULLING_CCW);
        e.AddCallback([this](stratus::Async<stratus::Entity> e) { 
            sponza = e.GetPtr(); 
            auto transform = stratus::GetComponent<stratus::LocalTransformComponent>(sponza);
            transform->SetLocalPosition(glm::vec3(0.0f));
            //transform->SetLocalScale(glm::vec3(15.0f));
            transform->SetLocalScale(glm::vec3(15.0f));
            INSTANCE(EntityManager)->AddEntity(sponza);
            //INSTANCE(RendererFrontend)->AddDynamicEntity(sponza);
        });

        INSTANCE(RendererFrontend)->SetSkybox(stratus::ResourceManager::Instance()->LoadCubeMap("../resources/textures/Skyboxes/learnopengl/sbox_", false, "jpg"));

        bool running = true;

        // for (int i = 0; i < 64; ++i) {
        //     float x = rand() % 600;gg
        //     float y = rand() % 600;
        //     float z = rand() % 200;
        //     stratus::VirtualPointLight * vpl = new stratus::VirtualPointLight();
        //     vpl->setIntensity(worldLight->getIntensity() * 50.0f);
        //     vpl->position = glm::vec3(x, y, z);
        //     vpl->setColor(worldLight->getColor());
        //     World()->AddLight(stratus::LightPtr((stratus::Light *)vpl));
        // }

        return true;
    }

    // Run a single update for the application (no infinite loops)
    // deltaSeconds = time since last frame
    virtual stratus::SystemStatus Update(const double deltaSeconds) override {
        if (Engine()->FrameCount() % 100 == 0) {
            STRATUS_LOG << "FPS:" << (1.0 / deltaSeconds) << " (" << (deltaSeconds * 1000.0) << " ms)" << std::endl;
        }

        auto worldLight = World()->GetWorldLight();
        const glm::vec3 worldLightColor = worldLight->getColor();

        //STRATUS_LOG << "Camera " << camera.getYaw() << " " << camera.getPitch() << std::endl;

        // Check for key/mouse events
        auto events = Input()->GetInputEventsLastFrame();
        for (auto e : events) {
            switch (e.type) {
                case SDL_QUIT:
                    return stratus::SystemStatus::SYSTEM_SHUTDOWN;
                case SDL_KEYDOWN:
                case SDL_KEYUP: {
                    bool released = e.type == SDL_KEYUP;
                    SDL_Scancode key = e.key.keysym.scancode;
                    switch (key) {
                        case SDL_SCANCODE_ESCAPE:
                            if (released) {
                                return stratus::SystemStatus::SYSTEM_SHUTDOWN;
                            }
                            break;
                        case SDL_SCANCODE_R:
                            if (released) {
                                INSTANCE(RendererFrontend)->RecompileShaders();
                            }
                            break;
                        case SDL_SCANCODE_1: {
                            if (released) {
                                LightCreator::CreateStationaryLight(
                                    LightParams(World()->GetCamera()->getPosition(), glm::vec3(1.0f, 1.0f, 0.5f), 1200.0f)
                                );
                            }
                            break;
                        }
                        case SDL_SCANCODE_2: {
                            if (released) {
                                LightCreator::CreateVirtualPointLight(
                                    LightParams(World()->GetCamera()->getPosition(), worldLightColor, worldLight->getIntensity() * 100.0f)
                                );
                            }
                            break;
                        }
                        case SDL_SCANCODE_3: {
                            if (released) {
                                LightCreator::CreateVirtualPointLight(
                                    LightParams(World()->GetCamera()->getPosition(), worldLightColor, worldLight->getIntensity() * 50.0f)
                                );
                            }
                            break;
                        }
                        case SDL_SCANCODE_4: {
                            if (released) {
                                const auto worldLightColor = glm::vec3(1.0f, 0.0f, 0.0f);
                                const uint32_t numShadowSamples = 3;
                                LightCreator::CreateVirtualPointLight(
                                    LightParams(World()->GetCamera()->getPosition(), worldLightColor, worldLight->getIntensity() * 15.0f, numShadowSamples)
                                );
                            }
                            break;
                        }
                        case SDL_SCANCODE_5: {
                            if (released) {
                                const auto worldLightColor = glm::vec3(0.0f, 1.0f, 0.0f);
                                const uint32_t numShadowSamples = 3;
                                LightCreator::CreateVirtualPointLight(
                                    LightParams(World()->GetCamera()->getPosition(), worldLightColor, worldLight->getIntensity() * 15.0f, numShadowSamples)
                                );
                            }
                            break;
                        }
                        case SDL_SCANCODE_6: {
                            if (released) {
                                const auto worldLightColor = glm::vec3(0.0f, 0.0f, 1.0f);
                                const uint32_t numShadowSamples = 3;
                                LightCreator::CreateVirtualPointLight(
                                    LightParams(World()->GetCamera()->getPosition(), worldLightColor, worldLight->getIntensity() * 15.0f, numShadowSamples)
                                );
                            }
                            break;
                        }
                        case SDL_SCANCODE_7: {
                            if (released) {
                                LightCreator::CreateRandomLightMover(
                                    LightParams(World()->GetCamera()->getPosition(), glm::vec3(1.0f, 1.0f, 0.5f), 1200.0f)
                                );
                            }
                            break;
                        }
                        default: break;
                    }
                    break;
                }
                default: break;
            }
        }

        

        // worldLight->setRotation(glm::vec3(75.0f, 0.0f, 0.0f));
        //worldLight->setRotation(stratus::Rotation(stratus::Degrees(30.0f), stratus::Degrees(0.0f), stratus::Degrees(0.0f)));

        #define LERP(x, v1, v2) (x * v1 + (1.0f - x) * v2)

        //renderer->toggleWorldLighting(worldLightEnabled);
        // worldLight->setColor(glm::vec3(1.0f, 0.75f, 0.5));
        // worldLight->setColor(glm::vec3(1.0f, 0.75f, 0.75f));
        //const float x = std::sinf(stratus::Radians(worldLight->getRotation().x).value());
        
        //worldLight->setRotation(glm::vec3(90.0f, 0.0f, 0.0f));
        //renderer->setWorldLight(worldLight);

        INSTANCE(RendererFrontend)->SetClearColor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));

        //renderer->addDrawable(rocks);

        // Add the camera's light
        //if (camLightEnabled) renderer->addPointLight(&cameraLight);
        //for (auto & entity : entities) {
        //    renderer->addDrawable(entity);
        //}

        //renderer->end(camera);

        //// 0 lets it run as fast as it can
        //SDL_GL_SetSwapInterval(0);
        //// Swap front and back buffer
        //SDL_GL_SwapWindow(window);

        return stratus::SystemStatus::SYSTEM_CONTINUE;
    }

    // Perform any resource cleanup
    virtual void Shutdown() override {
        LightCreator::Shutdown();
    }

private:
    stratus::EntityPtr sponza;
    std::vector<stratus::EntityPtr> entities;
};

STRATUS_ENTRY_POINT(Sponza)