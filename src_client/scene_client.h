#pragma once

#include "sceneloader.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <functional>
#include <string>
#include <atomic>

class SceneClient {
public:
    SceneClient(std::shared_ptr<grpc::Channel> channel);

    // Returns scene manifest (synchronous)
    bool GetSceneManifest(const std::string& scene_id, scene::SceneManifest& out_manifest);

    // Streams model to disk. progress_cb(bytes_received, total_bytes)
    // Added optional cancel token pointer. If non-null, StreamModelToFile should abort early when *cancel is true.
    bool StreamModelToFile(const std::string& scene_id,
                           const std::string& rel_path,
                           const std::string& out_path,
                           int64_t total_bytes,
                           std::function<void(int64_t, int64_t)> progress_cb,
                           std::atomic<bool>* cancel = nullptr);

private:
    std::unique_ptr<scene::SceneService::Stub> stub_;
};