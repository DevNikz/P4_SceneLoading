#pragma once

#include <glm/glm.hpp>


struct GLFWwindow;

class Camera {
public:
    Camera();

    // Call once per-frame on main thread to update camera from input.
    // Pass current window and dt (seconds).
    void UpdateFromInput(GLFWwindow* window, double dt);

    // Frame the camera so a sphere (center, radius) fits in view.
    // aspect: width/height viewport aspect ratio.
    void FrameBoundingSphere(const glm::vec3& center, float radius, float aspect);

    // Matrices (call after UpdateFromInput or FrameBoundingSphere)
    glm::mat4 GetViewMatrix() const;
    glm::mat4 GetProjectionMatrix(float aspect) const;

    // Parameters you can tweak
    float fov_deg = 60.0f;
    float near_z = 0.1f;
    float far_z = 1000.0f;

    // transform access
    glm::vec3 GetPosition() const;
    glm::vec3 GetTarget() const;

    // Allow manual set when needed
    void SetTarget(const glm::vec3& t);
    void SetDistance(float d);

private:
    // orbit parameters
    float yaw_deg_;
    float pitch_deg_;
    float distance_;

    glm::vec3 target_;

    // input state
    bool orbiting_ = false;
    double last_mouse_x_ = 0.0;
    double last_mouse_y_ = 0.0;
};