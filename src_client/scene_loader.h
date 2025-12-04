#pragma once

#include "scene_types.h"
#include "scene_client.h" // your existing SceneClient
#include <functional>
#include <string>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>

// GL upload task queue type (executed on main thread)
using GLUploadTask = std::function<void()>;

// Forward-declare renderer
class GLRenderer;

class SceneLoader {
public:
    // worker_count: number of background loader threads (default 4)
    SceneLoader(SceneClient* client, GLRenderer* renderer, std::queue<GLUploadTask>& upload_queue, std::mutex& upload_mtx, std::condition_variable& upload_cv, const std::string& tmp_dir = "tmp", size_t worker_count = 4);
    ~SceneLoader();

    // Enqueue a scene to load asynchronously (returns immediately)
    void EnqueueLoad(const std::shared_ptr<SceneDescriptor>& scene);

    // Cancel all work and join threads
    void Shutdown();

private:
    void WorkerThread();

    SceneClient* client_;
    GLRenderer* renderer_;
    std::string tmp_dir_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{ true };
    size_t worker_count_{ 1 };

    std::mutex queue_mtx_;
    std::condition_variable queue_cv_;
    std::deque<std::shared_ptr<SceneDescriptor>> queue_;

    // GL upload queue references (main thread will pop)
    std::queue<GLUploadTask>& upload_queue_;
    std::mutex& upload_mtx_;
    std::condition_variable& upload_cv_;
};