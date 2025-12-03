#pragma once

#include "sceneloader.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <functional>
#include <string>

class SceneClient {
public:
    SceneClient(std::shared_ptr<grpc::Channel> channel);

    // Returns scene manifest (synchronous)
    bool GetSceneManifest(const std::string& scene_id, scene::SceneManifest& out_manifest);

    // Streams model to disk. progress_cb(bytes_received, total_bytes)
    bool StreamModelToFile(const std::string& scene_id, const std::string& rel_path, const std::string& out_path, int64_t total_bytes, std::function<void(int64_t, int64_t)> progress_cb);

private:
    std::unique_ptr<scene::SceneService::Stub> stub_;
};