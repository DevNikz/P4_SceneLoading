#include "scene_service_impl.h"
#include "sceneloader.pb.h"
#include "sceneloader.grpc.pb.h"

#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <vector>
#include <iostream>

namespace fs = std::filesystem;

SceneServiceImpl::SceneServiceImpl(const std::string& media_root, size_t chunk_size, int chunk_delay_ms)
    : media_root_(media_root)
    , chunk_size_(chunk_size)
    , chunk_delay_ms_(chunk_delay_ms)
{}

grpc::Status SceneServiceImpl::GetSceneManifest(grpc::ServerContext* /*context*/, const scene::SceneRequest* request, scene::SceneManifest* response) {
    const std::string scene_id = request->scene_id();
    fs::path scene_dir = fs::path(media_root_) / scene_id;
    if (!fs::exists(scene_dir) || !fs::is_directory(scene_dir)) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Scene not found");
    }

    response->set_scene_id(scene_id);

    // enumerate .obj files
    for (auto const& p : fs::directory_iterator(scene_dir)) {
        if (!p.is_regular_file()) continue;
        if (p.path().extension() == ".obj") {
            scene::ModelInfo* mi = response->add_models();
            mi->set_name(p.path().stem().string());
            mi->set_rel_path(p.path().filename().string());
            mi->set_size_bytes(static_cast<int64_t>(fs::file_size(p.path())));
        }
    }

    // optional: include first thumbnail file if present (thumbnail.png / thumbnail.jpg)
    const std::vector<std::string> thumb_names = { "thumbnail.png", "thumbnail.jpg", "thumb.png", "thumb.jpg" };
    for (auto const& tn : thumb_names) {
        fs::path thumb_path = scene_dir / tn;
        if (fs::exists(thumb_path) && fs::is_regular_file(thumb_path)) {
            std::ifstream ifs(thumb_path, std::ios::binary);
            if (ifs) {
                std::vector<char> data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                response->set_thumbnail(data.data(), data.size());
            }
            break;
        }
    }

    return grpc::Status::OK;
}

grpc::Status SceneServiceImpl::StreamModel(grpc::ServerContext* /*context*/, const scene::ModelRequest* request, grpc::ServerWriter<scene::Chunk>* writer) {
    const std::string scene_id = request->scene_id();
    const std::string rel_path = request->model_rel_path();
    fs::path file_path = fs::path(media_root_) / scene_id / rel_path;

    if (!fs::exists(file_path) || !fs::is_regular_file(file_path)) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Model not found");
    }

    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to open model file");
    }

    std::vector<char> buffer(chunk_size_);
    int64_t offset = 0;

    while (ifs && !ifs.eof()) {
        ifs.read(buffer.data(), static_cast<std::streamsize>(chunk_size_));
        std::streamsize read_count = ifs.gcount();
        if (read_count <= 0) break;

        scene::Chunk chunk;
        chunk.set_data(buffer.data(), static_cast<size_t>(read_count));
        chunk.set_offset(offset);
        chunk.set_last(false);

        // send chunk
        if (!writer->Write(chunk)) {
            // client cancelled or write failed
            return grpc::Status(grpc::StatusCode::CANCELLED, "Client cancelled streaming");
        }

        offset += static_cast<int64_t>(read_count);

        // optional artificial delay to simulate network conditions / avoid instant loads
        if (chunk_delay_ms_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(chunk_delay_ms_));
        }
    }

    // final chunk marker (no data, last = true)
    scene::Chunk last_chunk;
    last_chunk.set_offset(offset);
    last_chunk.set_last(true);
    writer->Write(last_chunk);

    return grpc::Status::OK;
}