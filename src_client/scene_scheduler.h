#pragma once

#include "scene_types.h"
#include "scene_loader.h"
#include <map>
#include <vector>
#include <memory>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>

class SceneScheduler {
public:
    explicit SceneScheduler(SceneLoader* loader);
    ~SceneScheduler();

    // Add/register a scene id (doesn't start loading immediately)
    void RegisterScene(const std::string& scene_id);

    // Start background scheduling thread
    void Start();

    // Stop and join
    void Stop();

    // Request the scheduler to prioritize a scene (user selects)
    void PrioritizeScene(const std::string& scene_id);

    // Unload a scene (free resources logically)
    void UnloadScene(const std::string& scene_id);

    // Query all descriptors (thread-safe snapshot)
    std::vector<std::shared_ptr<SceneDescriptor>> GetAllScenes();

private:
    void SchedulerThread();

    SceneLoader* loader_;
    std::map<std::string, std::shared_ptr<SceneDescriptor>> scenes_;
    std::mutex mtx_;
    std::thread sched_thread_;
    std::atomic<bool> running_{ false };
};