#include "P4_Client.h"

class SceneClient {
private:
	unique_ptr<SceneGRPC::Stub> stub;

public:
	SceneClient(shared_ptr<grpc::Channel> channel):
		stub(SceneGRPC::NewStub(channel)) { }

	void ProcessScene() {
		SceneRequest request;

		SceneReply reply;
		grpc::ClientContext context;
		grpc::Status status = stub->ProcessScene(&context, request, &reply);
		if (status.ok()) {
			cout << "Success" << endl;
		}
		else {
			cout << "Fail" << endl;
		}
	}
};

int main() {
	SceneClient client(grpc::CreateChannel("localhost::50051",
		grpc::InsecureChannelCredentials()));

	client.ProcessScene();

	return 0;
}