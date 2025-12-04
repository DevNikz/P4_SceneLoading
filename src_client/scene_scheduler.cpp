#include "scene_scheduler.h"
#include <algorithm>
#include <chrono>

using namespace std::chrono_literals;

// SceneScheduler schedules background loads and keeps scene descriptors.
SceneScheduler::SceneScheduler(SceneLoader* loader) : loader_(loader) {}
SceneScheduler::~SceneScheduler() { Stop(); }

void SceneScheduler::RegisterScene(const std::string& scene_id) {
    std::scoped_lock lk(mtx_);
    if (scenes_.find(scene_id) == scenes_.end()) {
        auto sd = std::make_shared<SceneDescriptor>();
        sd->scene_id = scene_id;
        sd->state.store(SceneState::UNLOADED);
        scenes_.emplace(scene_id, sd);
    }
}

void SceneScheduler::Start() {
    running_.store(true);
    sched_thread_ = std::thread(&SceneScheduler::SchedulerThread, this);
}

void SceneScheduler::Stop() {
    running_.store(false);
    if (sched_thread_.joinable()) sched_thread_.join();
}

// Move prioritized scene to map end so it is considered first.
void SceneScheduler::PrioritizeScene(const std::string& scene_id) {
    std::scoped_lock lk(mtx_);
    auto it = scenes_.find(scene_id);
    if (it != scenes_.end()) {
        auto sd = it->second;
        scenes_.erase(it);
        scenes_.emplace(scene_id, sd);
    }
}

void SceneScheduler::UnloadScene(const std::string& scene_id) {
    std::scoped_lock lk(mtx_);
    auto it = scenes_.find(scene_id);
    if (it != scenes_.end()) {
        it->second->state.store(SceneState::UNLOADED);
        // GL cleanup handled by main thread.
    }
}

std::vector<std::shared_ptr<SceneDescriptor>> SceneScheduler::GetAllScenes() {
    std::scoped_lock lk(mtx_);
    std::vector<std::shared_ptr<SceneDescriptor>> out;
    out.reserve(scenes_.size());
    for (auto& p : scenes_) out.push_back(p.second);
    return out;
}

// Scheduler thread: keep up to N scenes loading concurrently (N=5 here).
void SceneScheduler::SchedulerThread() {
    while (running_.load()) {
        std::vector<std::shared_ptr<SceneDescriptor>> snapshot;
        {
            std::scoped_lock lk(mtx_);
            for (auto& p : scenes_) snapshot.push_back(p.second);
        }

        int loaded_or_loading = 0;
        for (auto& s : snapshot) {
            SceneState st = s->state.load();
            if (st == SceneState::LOADED || st == SceneState::LOADING) ++loaded_or_loading;
        }

        int to_start = std::max(0, 5 - loaded_or_loading);
        if (to_start > 0) {
            for (auto& s : snapshot) {
                if (to_start <= 0) break;
                if (s->state.load() == SceneState::UNLOADED) {
                    s->state.store(SceneState::QUEUED);
                    loader_->EnqueueLoad(s);
                    --to_start;
                }
            }
        }

        std::this_thread::sleep_for(200ms);
    }
}