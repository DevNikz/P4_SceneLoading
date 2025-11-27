#include "P4_Server.h"

class SceneServerImpl final :
	public SceneGRPC::Service {

public:
	grpc::Status ProcessScene(grpc::ServerContext* context,
		const SceneRequest* request, SceneReply* reply) {

		reply->set_status(true);

		return grpc::Status::OK;
	}
};

void RunServer() {
	std::string serverAdd("0.0.0.0:50051"); //localhost using port 500051
	SceneServerImpl service;

	grpc::ServerBuilder builder;
	builder.AddListeningPort(serverAdd, grpc::InsecureServerCredentials());
	builder.RegisterService(&service);

	unique_ptr<grpc::Server> server(builder.BuildAndStart());
	cout << "Server listening on " << serverAdd << endl;

	server->Wait();
}

int main() {
	RunServer();
	return 0;
}