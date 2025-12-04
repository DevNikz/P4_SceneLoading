#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "camera.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <iostream>

Camera::Camera()
    : yaw_deg_(0.0f), pitch_deg_(20.0f), distance_(10.0f), target_(0.0f, 0.0f, 0.0f) {}

void Camera::UpdateFromInput(GLFWwindow* window, double dt) {
    double mx, my;
    glfwGetCursorPos(window, &mx, &my);

    int left = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT);
    bool now_orbit = (left == GLFW_PRESS);

    if (now_orbit && !orbiting_) {
        orbiting_ = true;
        last_mouse_x_ = mx; last_mouse_y_ = my;
    } else if (!now_orbit && orbiting_) {
        orbiting_ = false;
    }

    if (orbiting_) {
        double dx = mx - last_mouse_x_;
        double dy = my - last_mouse_y_;
        // Inverted horizontal orbit: subtract dx to invert left/right
        yaw_deg_ -= static_cast<float>(dx * 0.15);
        pitch_deg_ += static_cast<float>(-dy * 0.15);
        if (pitch_deg_ > 89.0f) pitch_deg_ = 89.0f;
        if (pitch_deg_ < -89.0f) pitch_deg_ = -89.0f;
        last_mouse_x_ = mx; last_mouse_y_ = my;
    }

    // Keyboard movement (WASD) — move the camera target in the view plane
    if (window) {
        // compute current camera position and forward/right vectors
        glm::vec3 camPos = GetPosition();
        glm::vec3 forward = glm::normalize(target_ - camPos);
        // avoid degenerate forward
        if (glm::length(forward) < 1e-6f) forward = glm::vec3(0.0f, 0.0f, -1.0f);
        glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
        float moveSpeed = 5.0f * static_cast<float>(dt); // units per second * dt


        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
            target_ += forward * moveSpeed;
        }
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
            target_ -= forward * moveSpeed;
        }
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
            target_ -= right * moveSpeed;   
        }
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
            target_ += right * moveSpeed;   
        }

        if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) {
            target_ += glm::vec3(0.0f, moveSpeed, 0.0f);
        }
        if (glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS) {
            target_ -= glm::vec3(0.0f, moveSpeed, 0.0f);
        }
    }
}

void Camera::FrameBoundingSphere(const glm::vec3& center, float radius, float aspect) {
    // compute horizontal fov from vertical fov
    float fovY = glm::radians(fov_deg);
    float fovX = 2.0f * atanf(tanf(fovY * 0.5f) * aspect);
    // choose the tighter angle (smaller) to ensure fit
    float theta = glm::min(fovY, fovX);
    float halfTheta = theta * 0.5f;
    // avoid divide by zero
    float safeSin = sinf(halfTheta);
    float distance = (safeSin > 1e-6f) ? (radius / safeSin) : radius + 2.0f;
    // place camera along current spherical angles but look at center
    target_ = center;
    distance_ = distance;
    // optionally adjust pitch so it's angled down a bit if the model is tall
    pitch_deg_ = glm::clamp(pitch_deg_, -89.0f, 89.0f);
    std::cerr << "[Camera] FrameBoundingSphere center=(" << center.x << "," << center.y << "," << center.z << ") radius=" << radius << " distance=" << distance << " aspect=" << aspect << "\n";
}

glm::mat4 Camera::GetViewMatrix() const {
    float yaw = glm::radians(yaw_deg_);
    float pitch = glm::radians(pitch_deg_);
    glm::vec3 dir;
    dir.x = cos(pitch) * sin(yaw);
    dir.y = sin(pitch);
    dir.z = cos(pitch) * cos(yaw);
    glm::vec3 pos = target_ - dir * distance_;
    return glm::lookAt(pos, target_, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 Camera::GetProjectionMatrix(float aspect) const {
    return glm::perspective(glm::radians(fov_deg), aspect, near_z, far_z);
}

glm::vec3 Camera::GetPosition() const {
    float yaw = glm::radians(yaw_deg_);
    float pitch = glm::radians(pitch_deg_);
    glm::vec3 dir;
    dir.x = cos(pitch) * sin(yaw);
    dir.y = sin(pitch);
    dir.z = cos(pitch) * cos(yaw);
    return target_ - dir * distance_;
}

glm::vec3 Camera::GetTarget() const { return target_; }
void Camera::SetTarget(const glm::vec3& t) { target_ = t; }
void Camera::SetDistance(float d) { distance_ = d; }