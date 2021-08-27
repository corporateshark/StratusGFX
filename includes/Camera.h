
#ifndef STRATUSGFX_CAMERA_H
#define STRATUSGFX_CAMERA_H

#include "Entity.h"

namespace stratus {
/**
 * A camera is an object that can view the world
 * from a certain perspective.
 */
class Camera : private Entity {
    glm::vec3 _worldUp = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 _dir = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 _up = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 _side;// = glm::cross(_up, _dir);
    glm::mat4 _viewTransform;
    glm::mat4 _worldTransform;
    bool _rangeCheckAngles;

public:
    Camera(bool rangeCheckAngles = true);
    ~Camera() override = default;

    /**
     * Adjusts the angle pitch/yaw of the camera by
     * some delta amount.
     * @param deltaYaw change in angle yaw
     * @param deltaPitch change in angle pitch
     */
    void modifyAngle(double deltaYaw, double deltaPitch);

    // Sets the x, y and z angles in degrees
    void setAngle(const glm::vec3 & angle);

    void setPosition(float x, float y, float z);
    const glm::vec3 & getPosition() const;

    const glm::vec3 & getDirection() const;

    /**
     * Sets the speed x/y/z of the camera.
     *
     * @param forward negative values send it back, positive send
     *      it forward
     * @param up negative values send it down, positive send it
     *      up
     * @param strafe negative values send it left, positive send
     *      it right
     */
    void setSpeed(float forward, float up, float strafe);
    const glm::vec3 & getSpeed() const;

    /**
     * Helper functions that return the pitch/yaw.
     */
    float getYaw() const;
    float getPitch() const;

    void update(double deltaSeconds) override;

    /**
     * @return view transform associated with this camera (world -> camera)
     */
    const glm::mat4 & getViewTransform() const;

    // Gets the camera -> world transform
    const glm::mat4 & getWorldTransform() const;

private:
    void _updateViewTransform();
    void _updateCameraAxes();
};
}

#endif //STRATUSGFX_CAMERA_H
