#include "webrtc.h"
#include "signaling.h"
#include "api/create_peerconnection_factory.h"
#include <spdlog/spdlog.h>
#include <vector>

WebRtcManager::WebRtcManager(const std::string& room_id, const std::string& client_id, bool is_caller)
    : room_id_(room_id), my_client_id_(client_id), is_caller_(is_caller), pc_observer_(*this), dc_observer_(*this) {
    SPDLOG_INFO("[WebRTC Lifecycle] WebRtcManager instance constructed. Caller Status: {}", is_caller_ ? "TRUE" : "FALSE");
}

WebRtcManager::~WebRtcManager() {
    SPDLOG_INFO("[WebRTC Lifecycle] WebRtcManager instance destructor invoked.");
    ShutdownConnection();
}

bool WebRtcManager::Initialize() {
    SPDLOG_INFO("[WebRTC Init] Allocating native worker socket servers and core signaling threads...");
    
    // Create socket servers to ensure network interfaces can map network adapters properly
    network_thread_ = rtc::Thread::CreateWithSocketServer();
    worker_thread_ = rtc::Thread::Create();
    signaling_thread_ = rtc::Thread::Create();
    
    network_thread_->Start();
    worker_thread_->Start();
    signaling_thread_->Start();

    webrtc::PeerConnectionFactoryDependencies factory_deps;
    factory_deps.network_thread = network_thread_.get();
    factory_deps.worker_thread = worker_thread_.get();
    factory_deps.signaling_thread = signaling_thread_.get();

    SPDLOG_INFO("[WebRTC Init] Building Modular PeerConnection Factory Interface...");
    factory_ = webrtc::CreateModularPeerConnectionFactory(std::move(factory_deps));
    if (!factory_) {
        SPDLOG_ERROR("[WebRTC Init CRITICAL] Failed to build native WebRTC Factory layer.");
        return false;
    }

    // ====================================================================
    // HIGH-PERFORMANCE INSTANT ICE GATHERING OPTIMIZATION OVERRIDES
    // ====================================================================
    webrtc::PeerConnectionInterface::RTCConfiguration config;
    webrtc::PeerConnectionInterface::IceServer server;
    server.urls.push_back("stun:://google.com");
    config.servers.push_back(server);

    // 1. Force WebRTC to gather all interface candidates in parallel instantly using the correct enum name
    config.continual_gathering_policy = webrtc::PeerConnectionInterface::GATHER_CONTINUALLY;
    
    // 2. Explicitly drop IPv6 adapter loops to completely bypass Docker, virtual machine, and local port stalls
    config.disable_ipv6 = true;

    webrtc::PeerConnectionDependencies dependencies(&pc_observer_);
    SPDLOG_INFO("[WebRTC Init] Mapping socket configurations to PeerConnection entry point...");
    auto pc_result = factory_->CreatePeerConnectionOrError(config, std::move(dependencies));
    if (!pc_result.ok()) {
        SPDLOG_ERROR("[WebRTC Init CRITICAL] PeerConnection initialization rejected by factory stack.");
        return false;
    }
    peer_connection_ = pc_result.MoveValue();
    SPDLOG_INFO("[WebRTC Init] Core PeerConnection constructed. Native pipeline state is healthy.");

    // Allocate the standby Data Channel upfront during boot to wake up the internal ICE loops
    if (is_caller_) {
        SPDLOG_INFO("[WebRTC Init] Client is Caller: Generating proactive local tracking DataChannel...");
        webrtc::DataChannelInit dc_config;
        dc_config.ordered = true;
        auto dc_result = peer_connection_->CreateDataChannelOrError("chat-channel", &dc_config);
        if (dc_result.ok()) {
            data_channel_ = dc_result.MoveValue();
            data_channel_->RegisterObserver(&dc_observer_);
            SPDLOG_INFO("[WebRTC Init] Local Caller DataChannel generated and observer hooks bound.");
        } else {
            SPDLOG_ERROR("[WebRTC Init ERROR] Failed to create local data stream track.");
        }
    }

    return true;
}



void WebRtcManager::StartConnectionSequence() {
    if (!peer_connection_) {
        SPDLOG_ERROR("[WebRTC Sequence Engine] Aborted: PeerConnection pointer reference is null.");
        return;
    }

    if (is_caller_) {
        SPDLOG_WARN("[WebRTC Sequence Engine] Room Ready Hook captured. Commencing asynchronous CreateOffer execution loop...");
        
        // Wrap the session description observer inside a thread-safe RefCounted scoped_refptr block
        rtc::scoped_refptr<InternalCreateSessionDescriptionObserver> observer(
            new rtc::RefCountedObject<InternalCreateSessionDescriptionObserver>(*this)
        );

        // Enforce explicit data-only parameters to satisfy media-free structural requirements
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        options.offer_to_receive_audio = false;
        options.offer_to_receive_video = false;

        SPDLOG_INFO("[WebRTC Sequence Engine] Invoking asynchronous native CreateOffer() pipeline...");
        peer_connection_->CreateOffer(observer.get(), options);
    } else {
        SPDLOG_INFO("[WebRTC Sequence Engine] Standing by as Callee. Waiting to parse incoming remote Host SDP Offer document...");
    }
}

void WebRtcManager::ShutdownConnection() {
    SPDLOG_INFO("[WebRTC TearDown] Terminating secure media sessions and closing thread attachments.");
    if (data_channel_) {
        data_channel_->UnregisterObserver();
        data_channel_ = nullptr;
    }
    if (peer_connection_) {
        peer_connection_->Close();
        peer_connection_ = nullptr;
    }
    factory_ = nullptr;

    if (network_thread_) network_thread_->Stop();
    if (worker_thread_) worker_thread_->Stop();
    if (signaling_thread_) signaling_thread_->Stop();
    
    SPDLOG_WARN("[WebRTC TearDown] Native execution thread pools successfully unmounted.");
}

void WebRtcManager::HandleRemoteSdp(const std::string& type, const std::string& sdp) {
    if (!peer_connection_) {
        SPDLOG_ERROR("[WebRTC Router] Cannot assign remote SDP payload. PeerConnection pointer reference is null.");
        return;
    }

    SPDLOG_INFO("[WebRTC Router] Ingesting incoming remote SDP payload string type: [{}]", type);
    webrtc::SdpParseError parse_err;
    webrtc::SdpType sdp_type = (type == "offer") ? webrtc::SdpType::kOffer : webrtc::SdpType::kAnswer;
    
    std::unique_ptr<webrtc::SessionDescriptionInterface> desc = 
        webrtc::CreateSessionDescription(sdp_type, sdp, &parse_err);

    if (!desc) {
        SPDLOG_ERROR("[WebRTC Router CRITICAL] Remote SDP string parse rejection: {}", parse_err.description);
        return;
    }

    SPDLOG_INFO("[WebRTC Router] Applying parsed remote description context via SetRemoteDescription()...");
    peer_connection_->SetRemoteDescription(InternalSetSessionDescriptionObserver::Create(), desc.release());

    if (sdp_type == webrtc::SdpType::kOffer) {
        SPDLOG_INFO("[WebRTC Router] Remote Offer successfully processed. Transitioning Callee to generate Answer payload...");
        
        if (!data_channel_) {
            SPDLOG_INFO("[WebRTC Router] Callee registering data track requirements to prepare handshake constraints...");
            webrtc::DataChannelInit dc_config;
            dc_config.ordered = true;
            auto dc_result = peer_connection_->CreateDataChannelOrError("chat-channel", &dc_config);
        }

        rtc::scoped_refptr<InternalCreateSessionDescriptionObserver> observer(
            new rtc::RefCountedObject<InternalCreateSessionDescriptionObserver>(*this)
        );

        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        options.offer_to_receive_audio = false;
        options.offer_to_receive_video = false;

        SPDLOG_INFO("[WebRTC Router] Invoking asynchronous native CreateAnswer() pipeline...");
        peer_connection_->CreateAnswer(observer.get(), options);
    }
}

void WebRtcManager::HandleRemoteIceCandidate(const std::string& mid, int mline_index, const std::string& sdp) {
    if (!peer_connection_) return;

    SPDLOG_INFO("[WebRTC Router] Ingesting remote candidate line parameters -> sdpMid: [{}], sdpMLineIndex: [{}]", mid, mline_index);
    webrtc::SdpParseError parse_err;
    std::unique_ptr<webrtc::IceCandidateInterface> candidate(
        webrtc::CreateIceCandidate(mid, mline_index, sdp, &parse_err));

    if (!candidate) {
        SPDLOG_ERROR("[WebRTC Router ERROR] Candidate string serialization block rejected: {}", parse_err.description);
        return;
    }

    if (peer_connection_->AddIceCandidate(candidate.get())) {
        SPDLOG_INFO("[WebRTC Router] Successfully injected remote candidate tracking line into local P2P engine context.");
    } else {
        SPDLOG_ERROR("[WebRTC Router ERROR] Internal P2P core rejected remote candidate assignment.");
    }
}

// ====================================================================
// WebRTC Internal Observer Implementation Blocks
// ====================================================================

void WebRtcManager::InternalPeerConnectionObserver::OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
    std::string candidate_str;
    if (!candidate->ToString(&candidate_str)) {
        SPDLOG_ERROR("[WebRTC Callback Error] Failed to stringify local gathered candidate line.");
        return;
    }

    SPDLOG_INFO("[WebRTC Callback] New local ICE candidate gathered! -> sdpMid: {}, sdpMLineIndex: {}", candidate->sdp_mid(), candidate->sdp_mline_index());
    std::string ice_packet = "ICE:" + candidate->sdp_mid() + ":" + 
                             std::to_string(candidate->sdp_mline_index()) + ":" + candidate_str;

    SendTargetedMessage(manager_.room_id_, manager_.my_client_id_, "ALL", ice_packet);
}

void WebRtcManager::InternalPeerConnectionObserver::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) {
    SPDLOG_INFO("==> [WebRTC ICE NETWORK CHANGED]: Status Code Enum Value -> {}", static_cast<int>(new_state));
}

void WebRtcManager::InternalPeerConnectionObserver::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
    SPDLOG_INFO("==> [WebRTC CORE PIPELINE CHANGED]: Status Code Enum Value -> {}", static_cast<int>(new_state));
}

void WebRtcManager::InternalPeerConnectionObserver::OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
    SPDLOG_WARN("[WebRTC Callback] Remote negotiated DataChannel caught! Binding Callee observer loops...");
    manager_.data_channel_ = data_channel;
    data_channel->RegisterObserver(&manager_.dc_observer_);
}

void WebRtcManager::InternalCreateSessionDescriptionObserver::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
    SPDLOG_WARN("[WebRTC Callback] SessionDescription generated successfully via CreateOffer/CreateAnswer observer thread!");
    manager_.peer_connection_->SetLocalDescription(InternalSetSessionDescriptionObserver::Create(), desc);
    std::string sdp_text;
    desc->ToString(&sdp_text);
    std::string type_str = (desc->GetType() == webrtc::SdpType::kOffer) ? "SDP_OFFER:" : "SDP_ANSWER:";
    SPDLOG_INFO("[WebRTC Callback] Dispatching generated local SDP string sequence to Firestore room collection stream...");
    SendTargetedMessage(manager_.room_id_, manager_.my_client_id_, "ALL", type_str + sdp_text);
}

void WebRtcManager::InternalCreateSessionDescriptionObserver::OnFailure(webrtc::RTCError error) {
    SPDLOG_ERROR("[WebRTC Callback CRITICAL] Asynchronous CreateOffer/CreateAnswer formulation failed! Reason: {}", error.message());
}

void WebRtcManager::InternalSetSessionDescriptionObserver::OnFailure(webrtc::RTCError error) {
    SPDLOG_ERROR("[WebRTC Callback CRITICAL] Local/Remote description assignment state tracking error: {}", error.message());
}

void WebRtcManager::InternalDataChannelObserver::OnStateChange() {
    if (manager_.data_channel_) {
        auto current_state = manager_.data_channel_->state();
        SPDLOG_INFO("[WebRTC DataChannel State]: Changed to {}", static_cast<int>(current_state));
        
        if (current_state == webrtc::DataChannelInterface::kOpen) {
            SPDLOG_WARN("DataChannel Open! You can now send messages.");
            std::string greeting = "Hello from peer identity token: [" + manager_.my_client_id_ + "]";
            webrtc::DataBuffer buffer(greeting);
            manager_.data_channel_->Send(buffer);
        }
    }
}

void WebRtcManager::InternalDataChannelObserver::OnMessage(const webrtc::DataBuffer& buffer) {
    std::string msg(reinterpret_cast<const char*> (buffer.data.data()), buffer.data.size());
    SPDLOG_INFO("[DATA CHANNEL RECEIVED BUFFER]: {}", msg);
}
