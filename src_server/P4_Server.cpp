#include "scene_service_impl.h"
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <string>
#include <cstdlib>

int main(int argc, char** argv) {
    // Usage: P4_Server [media_root] [port] [chunk_size_bytes] [chunk_delay_ms]
    std::string media_root = (argc > 1) ? argv[1] : "Media";
    std::string port = (argc > 2) ? argv[2] : "50051";
    size_t chunk_size = (argc > 3) ? static_cast<size_t>(std::stoull(argv[3])) : 64 * 1024;
    int chunk_delay_ms = (argc > 4) ? std::stoi(argv[4]) : 30;

    std::string server_address = "0.0.0.0:" + port;

    SceneServiceImpl service(media_root, chunk_size, chunk_delay_ms);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server) {
        std::cerr << "Failed to start server on " << server_address << std::endl;
        return 1;
    }

    std::cout << "Server listening on " << server_address << "\n";
    std::cout << "Media root: " << media_root << "\n";
    std::cout << "Chunk size: " << chunk_size << " bytes, chunk delay: " << chunk_delay_ms << " ms\n";
    server->Wait();

    return 0;
}