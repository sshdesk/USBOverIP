#ifndef WEBRTC_H
#define WEBRTC_H

#include <string>
#include <memory>
#include "api/peer_connection_interface.h"
#include "api/data_channel_interface.h"
#include "rtc_base/thread.h"

// Forward declaration of our signaling transport handlers
void SendTargetedMessage(const std::string& room_id, const std::string& my_client_id, 
                          const std::string& target_client_id, const std::string& text_payload);

class WebRtcManager {
public:
    WebRtcManager(const std::string& room_id, const std::string& client_id, bool is_caller);
    ~WebRtcManager();

    bool Initialize();
    void StartConnectionSequence();
    void ShutdownConnection();

    // Handlers to inject remote signals received from the Firestore listener pipeline
    void HandleRemoteSdp(const std::string& type, const std::string& sdp);
    void HandleRemoteIceCandidate(const std::string& mid, int mline_index, const std::string& sdp);

private:
    std::string room_id_;
    std::string my_client_id_;
    bool is_caller_;

    std::unique_ptr<rtc::Thread> network_thread_;
    std::unique_ptr<rtc::Thread> worker_thread_;
    std::unique_ptr<rtc::Thread> signaling_thread_;

    rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
    rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;
    rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel_;

    // ====================================================================
    // Inner Native WebRTC Observer Classes
    // ====================================================================
    
    class InternalPeerConnectionObserver : public webrtc::PeerConnectionObserver {
    private:
        WebRtcManager& manager_;
    public:
        InternalPeerConnectionObserver(WebRtcManager& manager) : manager_(manager) {}
        void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override;
        void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override;
        void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) override;
        void OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override;
        
        // Unused signaling/renegotiation lifecycle overrides
        void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override {}
        void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override {}
        void OnIceCandidatesRemoved(const std::vector<cricket::Candidate>& candidates) override {}
        void OnRenegotiationNeeded() override {}
    };

    class InternalCreateSessionDescriptionObserver : public webrtc::CreateSessionDescriptionObserver {
    private:
        WebRtcManager& manager_;
    public:
        InternalCreateSessionDescriptionObserver(WebRtcManager& manager) : manager_(manager) {}
        void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;
        void OnFailure(webrtc::RTCError error) override;
    };

    class InternalSetSessionDescriptionObserver : public webrtc::SetSessionDescriptionObserver {
    public:
        static InternalSetSessionDescriptionObserver* Create() {
            return new rtc::RefCountedObject<InternalSetSessionDescriptionObserver>();
        }
        void OnSuccess() override {}
        void OnFailure(webrtc::RTCError error) override;
    };

    class InternalDataChannelObserver : public webrtc::DataChannelObserver {
    private:
        WebRtcManager& manager_; // Link reference back to the parent class
    public:
        InternalDataChannelObserver(WebRtcManager& manager) : manager_(manager) {}
        void OnStateChange() override;
        void OnMessage(const webrtc::DataBuffer& buffer) override;
        void OnBufferedAmountChange(uint64_t previous_amount) override {}
    };

    InternalPeerConnectionObserver pc_observer_;
    InternalDataChannelObserver dc_observer_;
};

#endif // WEBRTC_H
