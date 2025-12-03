#pragma once

#include "sceneloader.grpc.pb.h"
#include <string>

class SceneServiceImpl final : public scene::SceneService::Service {
public:
    // media_root: root directory containing Media/<scene_id>/...
    // chunk_size: bytes per Chunk message
    // chunk_delay_ms: artificial delay (ms) after each chunk to simulate slow network
    explicit SceneServiceImpl(const std::string& media_root, size_t chunk_size = 64 * 1024, int chunk_delay_ms = 30);

    grpc::Status GetSceneManifest(grpc::ServerContext* context, const scene::SceneRequest* request, scene::SceneManifest* response) override;
    grpc::Status StreamModel(grpc::ServerContext* context, const scene::ModelRequest* request, grpc::ServerWriter<scene::Chunk>* writer) override;

private:
    std::string media_root_;
    size_t chunk_size_;
    int chunk_delay_ms_;
};