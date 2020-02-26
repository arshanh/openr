#include "ServiceLayerAsyncInit.h" 
#include <google/protobuf/text_format.h>

using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::CompletionQueue;
using grpc::Status;
using service_layer::SLInitMsg;
using service_layer::SLVersion;
using service_layer::SLGlobal;

namespace openr {

std::mutex init_mutex;
std::condition_variable init_condVar;
bool init_success;
bool notifthread_done;

AsyncNotifChannel::AsyncNotifChannel(std::shared_ptr<grpc::Channel> channel)
        : stub_(service_layer::SLGlobal::NewStub(channel)) {}


// Assembles the client's payload and sends it to the server.

void 
AsyncNotifChannel::SendInitMsg(const service_layer::SLInitMsg init_msg)
{
    std::string s;

    if (google::protobuf::TextFormat::PrintToString(init_msg, &s)) {
        VLOG(2) << "###########################" ;
        VLOG(2) << "Transmitted message: IOSXR-SL INIT " << s;
        VLOG(2) << "###########################" ;
    } else {
        VLOG(2) << "###########################" ;
        VLOG(2) << "Message not valid (partial content: "
                  << init_msg.ShortDebugString() << ")";
        VLOG(2) << "###########################" ;
    }

    // Typically when using the asynchronous API, we hold on to the 
    //"call" instance in order to get updates on the ongoing RPC.
    // In our case it isn't really necessary, since we operate within the
    // context of the same class, but anyway, we pass it in as the tag

    
    call.response_reader = stub_->AsyncSLGlobalInitNotif(&call.context, init_msg, &cq_, (void *)&call);
}

void 
AsyncNotifChannel::Shutdown() 
{
    tear_down = true;

    std::unique_lock<std::mutex> channel_lock(channel_mutex);

    while(!channel_closed) {
        channel_condVar.wait(channel_lock);
    }
    cq_.Shutdown();
}

void 
AsyncNotifChannel::Cleanup() 
{
    VLOG(1) << "Asynchronous client shutdown requested"
            << "Let's clean up!";

    // Finish the Async session
    call.HandleResponse(false);

    // Shutdown the completion queue
    call.HandleResponse(false);

    VLOG(1) << "Notifying channel close";
    channel_closed = true;
    // Notify the condition variable;
    channel_condVar.notify_one();
}


// Loop while listening for completed responses.
// Prints out the response from the server.
void 
AsyncNotifChannel::AsyncCompleteRpc() 
{
    void* got_tag;
    bool ok = false;
    // Storage for the status of the RPC upon completion.
    grpc::Status status;

    // Lock the mutex before notifying using the conditional variable
    std::lock_guard<std::mutex> guard(channel_mutex);


    unsigned int timeout = 5;

    // Set timeout for API
    std::chrono::system_clock::time_point deadline =
        std::chrono::system_clock::now() + std::chrono::seconds(timeout);

    while (!tear_down) {
        auto nextStatus = cq_.AsyncNext(&got_tag, &ok, deadline);

        switch(nextStatus) {
        case grpc::CompletionQueue::GOT_EVENT:
             VLOG(2) << "Got event! for Async channel";
             // Verify that the request was completed successfully. Note that "ok"
             // corresponds solely to the request for updates introduced by Finish().
             call.HandleResponse(ok);
             break;
        case grpc::CompletionQueue::SHUTDOWN:
             VLOG(1) << "Shutdown event received for completion queue";
             channel_closed = true;
             // Notify the condition variable;
             channel_condVar.notify_one();
             tear_down = true;
             break;
        case grpc::CompletionQueue::TIMEOUT:
             continue;
             break;
        }
    }

    if(!channel_closed) {
        Cleanup();
    }

    notifthread_done = true;
}


AsyncNotifChannel::AsyncClientCall::AsyncClientCall(): callStatus_(CREATE) {}

void 
AsyncNotifChannel::AsyncClientCall::HandleResponse(bool responseStatus)
{
    //The First completion queue entry indicates session creation and shouldn't be processed - Check?
    switch (callStatus_) {
    case CREATE:
        VLOG(2) << "CallStatus: CREATE, about to Read";
        if (responseStatus) {
            response_reader->Read(&notif, (void*)this);
            callStatus_ = PROCESS;
            VLOG(2) << "CallStatus: create, now set to Process";
        } else {
            response_reader->Finish(&status, (void*)this);
            callStatus_ = FINISH;
            VLOG(2) << "CallStatus: create, now set to Finish";
        }
        break;
    case PROCESS:
        VLOG(2) << "CallStatus: PROCESS, about to Read";
        if (responseStatus) {
            response_reader->Read(&notif, (void *)this);
            auto slerrstatus = static_cast<int>(notif.errstatus().status());
            auto eventtype = static_cast<int>(notif.eventtype());

            if( eventtype == static_cast<int>(service_layer::SL_GLOBAL_EVENT_TYPE_VERSION) ) {
                if((slerrstatus == 
                       service_layer::SLErrorStatus_SLErrno_SL_SUCCESS) ||
                   (slerrstatus == 
                       service_layer::SLErrorStatus_SLErrno_SL_INIT_STATE_READY) ||
                   (slerrstatus == 
                       service_layer::SLErrorStatus_SLErrno_SL_INIT_STATE_CLEAR)) {
                    VLOG(1) << "IOS-XR gRPC Server returned "; 
                    VLOG(1) << "Successfully Initialized, connection Established!";
                            
                    // Lock the mutex before notifying using the conditional variable
                    std::lock_guard<std::mutex> guard(init_mutex);

                    // Set the initsuccess flag to indicate successful initialization
                    init_success = true;
       
                    // Notify the condition variable;
                    init_condVar.notify_one();

                } else {
                    LOG(ERROR) << "Client init error code " << slerrstatus ;
                }
            } else if (eventtype == static_cast<int>(service_layer::SL_GLOBAL_EVENT_TYPE_HEARTBEAT)) {
                VLOG(1) << "Received Heartbeat"; 
            } else if (eventtype == static_cast<int>(service_layer::SL_GLOBAL_EVENT_TYPE_ERROR)) {
                if (slerrstatus == service_layer::SLErrorStatus_SLErrno_SL_NOTIF_TERM) {
                    LOG(ERROR) << "Received notice to terminate. Client Takeover?";
                } else {
                    LOG(ERROR) << "Error Not Handled " << slerrstatus;
                } 
            } else {
                LOG(ERROR) << "client init unrecognized response " << eventtype;
            }
        } else {
            response_reader->Finish(&status, (void*)this);
            callStatus_ = FINISH;
            VLOG(2) << "CallStatus: PROCESS, now set to FINISH";
        }
        break;
    case FINISH:
        VLOG(2) << "CallStatus: FINISH, check status of last finish call";
        if (status.ok()) {
            VLOG(1) << "Server Response Completed: "  
                    << this << " CallData: " 
                    << this;
        }
        else {
            LOG(ERROR) << "RPC failed";
        }
        VLOG(1) << "Shutting down the completion queue";
        //pcq_->Shutdown();
    }
}

} 
