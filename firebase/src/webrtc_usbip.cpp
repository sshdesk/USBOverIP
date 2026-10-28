#include "webrtc_usbip.h"
#include "signaling.h"
#include "api/create_peerconnection_factory.h"
#include <spdlog/spdlog.h>
#include <vector>

WebRtcManager_usbip::WebRtcManager_usbip(const std::string& room_id, const std::string& client_id, bool is_Client)
    : room_id_(room_id), my_client_id_(client_id), is_Client(is_Client), pc_observer_(*this), dc_observer_(*this) {
    SPDLOG_INFO("[WebRTC Lifecycle] WebRtcManager_usbip instance constructed. Caller Status: {}", is_Client ? "TRUE" : "FALSE");
}

WebRtcManager_usbip::~WebRtcManager_usbip() {
    SPDLOG_INFO("[WebRTC Lifecycle] WebRtcManager_usbip instance destructor invoked.");
    ShutdownConnection();
}

bool WebRtcManager_usbip::Initialize() {
    SPDLOG_INFO("[WebRTC Init] Allocating native worker socket servers and core signaling threads...");
    
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

    webrtc::PeerConnectionInterface::RTCConfiguration config;
    webrtc::PeerConnectionInterface::IceServer server;
    server.urls.push_back("stun:stun.l.google.com:19302");
    config.servers.push_back(server);

    config.continual_gathering_policy = webrtc::PeerConnectionInterface::GATHER_CONTINUALLY;

    webrtc::PeerConnectionDependencies dependencies(&pc_observer_);
    SPDLOG_INFO("[WebRTC Init] Mapping socket configurations to PeerConnection entry point...");
    auto pc_result = factory_->CreatePeerConnectionOrError(config, std::move(dependencies));
    if (!pc_result.ok()) {
        SPDLOG_ERROR("[WebRTC Init CRITICAL] PeerConnection initialization rejected by factory stack.");
        return false;
    }
    peer_connection_ = pc_result.MoveValue();
    SPDLOG_INFO("[WebRTC Init] Core PeerConnection constructed. Native pipeline state is healthy.");

    if (is_Client) {
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

void WebRtcManager_usbip::StartConnectionSequence() {
    if (!peer_connection_) {
        SPDLOG_ERROR("[WebRTC Sequence Engine] Aborted: PeerConnection pointer reference is null.");
        return;
    }

    if (is_Client) {
        SPDLOG_WARN("[WebRTC Sequence Engine] Room Ready Hook captured. Commencing asynchronous CreateOffer execution loop...");
        
        rtc::scoped_refptr<InternalCreateSessionDescriptionObserver> observer(
            new rtc::RefCountedObject<InternalCreateSessionDescriptionObserver>(*this)
        );

        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        options.offer_to_receive_audio = false;
        options.offer_to_receive_video = false;

        SPDLOG_INFO("[WebRTC Sequence Engine] Invoking asynchronous native CreateOffer() pipeline...");
        peer_connection_->CreateOffer(observer.get(), options);
    } else {
        SPDLOG_INFO("[WebRTC Sequence Engine] Standing by as Callee. Waiting to parse incoming remote Host SDP Offer document...");
    }
}

void WebRtcManager_usbip::ShutdownConnection() {
    SPDLOG_INFO("[WebRTC TearDown] Terminating secure media sessions and closing thread attachments.");
    if (data_channel_ && dc_state_callback_) {
        dc_state_callback_(false);
    }

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
    
    SPDLOG_INFO("[WebRTC TearDown] Native execution thread pools successfully unmounted.");
}

void WebRtcManager_usbip::HandleRemoteSdp(const std::string& type, const std::string& sdp) {
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

void WebRtcManager_usbip::HandleRemoteIceCandidate(const std::string& mid, int mline_index, const std::string& sdp) {
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

void WebRtcManager_usbip::SetDataChannelStateCallback(DataChannelStateCallback callback) {
    dc_state_callback_ = std::move(callback);
}

rtc::scoped_refptr<webrtc::DataChannelInterface> WebRtcManager_usbip::GetDataChannel() {
    return data_channel_;
}

rtc::Thread* WebRtcManager_usbip::GetSignalingThread() {
    return signaling_thread_.get();
}

void WebRtcManager_usbip::InternalPeerConnectionObserver::OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
    std::string candidate_str;
    if (!candidate->ToString(&candidate_str)) {
        SPDLOG_ERROR("[WebRTC Callback Error] Failed to stringify local gathered candidate line.");
        return;
    }

    SPDLOG_INFO("[WebRTC Callback] New local ICE candidate gathered for target: {} -> sdpMid: {}", manager_.remote_peer_id_, candidate->sdp_mid());
    std::string ice_packet = "ICE:" + candidate->sdp_mid() + ":" + 
                             std::to_string(candidate->sdp_mline_index()) + ":" + candidate_str;

    if (!manager_.remote_peer_id_.empty()) {
        SendTargetedMessage(manager_.room_id_, manager_.my_client_id_, manager_.remote_peer_id_, ice_packet);
    } else {
        SPDLOG_ERROR("[WebRTC Error] Cannot send ICE candidate: remote_peer_id_ is empty!");
    }
}

void WebRtcManager_usbip::InternalPeerConnectionObserver::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) {
    SPDLOG_INFO("==> [WebRTC ICE NETWORK CHANGED]: Status Code Enum Value -> {}", static_cast<int>(new_state));
}

void WebRtcManager_usbip::InternalPeerConnectionObserver::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
    SPDLOG_INFO("==> [WebRTC CORE PIPELINE CHANGED]: Status Code Enum Value -> {}", static_cast<int>(new_state));
}

void WebRtcManager_usbip::InternalPeerConnectionObserver::OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
    SPDLOG_WARN("[WebRTC Callback] Remote negotiated DataChannel caught! Binding Callee observer loops...");
    manager_.data_channel_ = data_channel;
    data_channel->RegisterObserver(&manager_.dc_observer_);
}

void WebRtcManager_usbip::InternalCreateSessionDescriptionObserver::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
    SPDLOG_WARN("[WebRTC Callback] SessionDescription generated successfully!");
    manager_.peer_connection_->SetLocalDescription(InternalSetSessionDescriptionObserver::Create(), desc);
    std::string sdp_text;
    desc->ToString(&sdp_text);
    std::string type_str = (desc->GetType() == webrtc::SdpType::kOffer) ? "SDP_OFFER:" : "SDP_ANSWER:";
    
    SPDLOG_INFO("[WebRTC Callback] Dispatching generated local SDP to target: {}", manager_.remote_peer_id_);
    if (!manager_.remote_peer_id_.empty()) {
        SendTargetedMessage(manager_.room_id_, manager_.my_client_id_, manager_.remote_peer_id_, type_str + sdp_text);
    } else {
        SPDLOG_ERROR("[WebRTC Error] Cannot send SDP: remote_peer_id_ is empty!");
    }
}

void WebRtcManager_usbip::InternalCreateSessionDescriptionObserver::OnFailure(webrtc::RTCError error) {
    SPDLOG_ERROR("[WebRTC Callback CRITICAL] Asynchronous CreateOffer/CreateAnswer formulation failed! Reason: {}", error.message());
}

void WebRtcManager_usbip::InternalSetSessionDescriptionObserver::OnFailure(webrtc::RTCError error) {
    SPDLOG_ERROR("[WebRTC Callback CRITICAL] Local/Remote description assignment state tracking error: {}", error.message());
}

void WebRtcManager_usbip::InternalDataChannelObserver::OnStateChange() {
    if (manager_.data_channel_) {
        auto current_state = manager_.data_channel_->state();
        SPDLOG_INFO("[WebRTC DataChannel State]: Changed to {}", static_cast<int>(current_state));
        
        bool is_open = (current_state == webrtc::DataChannelInterface::kOpen);
        if (manager_.dc_state_callback_) {
            manager_.dc_state_callback_(is_open);
        }
    }
}

void WebRtcManager_usbip::InternalDataChannelObserver::OnMessage(const webrtc::DataBuffer& buffer) {
    // Only intercepting data for tcp tunnel routing done elsewhere in original setup.
    // Kept passive as requested.
}

bool WebRtcManager_usbip::IsDataChannelOpen() const {
    return data_channel_ && data_channel_->state() == webrtc::DataChannelInterface::kOpen;
}
