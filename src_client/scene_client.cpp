#include "scene_client.h"
#include "sceneloader.pb.h"
#include "sceneloader.grpc.pb.h"
#include <fstream>
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

// Thin wrapper around gRPC stub for synchronous calls used by the client UI.
SceneClient::SceneClient(std::shared_ptr<grpc::Channel> channel)
    : stub_(scene::SceneService::NewStub(channel)) {
}

// GetSceneManifest: synchronous RPC, returns false on error.
bool SceneClient::GetSceneManifest(const std::string& scene_id, scene::SceneManifest& out_manifest) {
    grpc::ClientContext ctx;
    scene::SceneRequest req;
    req.set_scene_id(scene_id);
    grpc::Status status = stub_->GetSceneManifest(&ctx, req, &out_manifest);
    if (!status.ok()) {
        std::cerr << "GetSceneManifest failed: " << status.error_message() << "\n";
        return false;
    }
    return true;
}

// StreamModelToFile: streams model data to disk; calls progress_cb during download.
// Accepts optional cancel token pointer; if non-null and *cancel is true the download will abort.
bool SceneClient::StreamModelToFile(const std::string& scene_id,
                                    const std::string& rel_path,
                                    const std::string& out_path,
                                    int64_t total_bytes,
                                    std::function<void(int64_t, int64_t)> progress_cb,
                                    std::atomic<bool>* cancel) {
    grpc::ClientContext ctx;
    scene::ModelRequest req;
    req.set_scene_id(scene_id);
    req.set_model_rel_path(rel_path);
    req.set_offset(0);

    // quick cancellation check before starting
    if (cancel && cancel->load()) {
        std::cerr << "StreamModelToFile: cancel requested before start for " << rel_path << "\n";
        return false;
    }

    std::unique_ptr<grpc::ClientReader<scene::Chunk>> reader(stub_->StreamModel(&ctx, req));
    std::ofstream ofs(out_path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        std::cerr << "Failed to open output file: " << out_path << "\n";
        return false;
    }

    scene::Chunk chunk;
    int64_t bytes_written = 0;
    bool cancelled = false;
    while (reader->Read(&chunk)) {
        // check cancellation periodically
        if (cancel && cancel->load()) {
            std::cerr << "StreamModelToFile: cancellation detected for " << rel_path << "\n";
            // ask gRPC to cancel the RPC (best-effort)
            ctx.TryCancel();
            cancelled = true;
            break;
        }

        if (chunk.data().size() > 0) {
            ofs.write(chunk.data().data(), static_cast<std::streamsize>(chunk.data().size()));
            bytes_written += static_cast<int64_t>(chunk.data().size());
            if (progress_cb) progress_cb(bytes_written, total_bytes);
        }
        if (chunk.last()) {
            break;
        }
    }

    grpc::Status status = reader->Finish();

    ofs.close();

    if (cancelled) {
        // remove partial file to avoid leaving corrupted artifacts
        try {
            if (fs::exists(out_path)) fs::remove(out_path);
        } catch (...) {
            std::cerr << "StreamModelToFile: failed to remove partial file " << out_path << "\n";
        }
        return false;
    }

    if (!status.ok()) {
        std::cerr << "StreamModel failed: " << status.error_message() << "\n";
        // on RPC error remove partial file
        try {
            if (fs::exists(out_path)) fs::remove(out_path);
        } catch (...) {
            std::cerr << "StreamModelToFile: failed to remove partial file after error " << out_path << "\n";
        }
        return false;
    }
    return true;
}