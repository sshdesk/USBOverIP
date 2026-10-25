#ifndef WEBRTC_H
#define WEBRTC_H

#include <string>
#include <functional>
#include <memory>
#include "api/peer_connection_interface.h"
#include "api/data_channel_interface.h"
#include "rtc_base/thread.h"

void SendTargetedMessage(const std::string& room_id, const std::string& my_client_id, 
                          const std::string& target_client_id, const std::string& text_payload);

class WebRtcManager_usbip {
public:
    using DataChannelStateCallback = std::function<void(bool is_open)>;

    WebRtcManager_usbip(const std::string& room_id, const std::string& client_id, bool is_Client);
    ~WebRtcManager_usbip();

    bool Initialize();
    void StartConnectionSequence();
    void ShutdownConnection();

    void HandleRemoteSdp(const std::string& type, const std::string& sdp);
    void HandleRemoteIceCandidate(const std::string& mid, int mline_index, const std::string& sdp);

    void SetRemotePeerId(const std::string& peer_id) { remote_peer_id_ = peer_id; }
    bool IsDataChannelOpen() const;

    rtc::scoped_refptr<webrtc::DataChannelInterface> GetDataChannel();
    rtc::Thread* GetSignalingThread();

    void SetDataChannelStateCallback(DataChannelStateCallback callback);
    DataChannelStateCallback dc_state_callback_;

private:
    std::string room_id_;
    std::string my_client_id_;
    std::string remote_peer_id_;
    bool is_Client;

    std::unique_ptr<rtc::Thread> network_thread_;
    std::unique_ptr<rtc::Thread> worker_thread_;
    std::unique_ptr<rtc::Thread> signaling_thread_;

    rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
    rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;
    rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel_;

    class InternalPeerConnectionObserver : public webrtc::PeerConnectionObserver {
    private:
        WebRtcManager_usbip& manager_;
    public:
        InternalPeerConnectionObserver(WebRtcManager_usbip& manager) : manager_(manager) {}
        void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override;
        void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override;
        void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) override;
        void OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override;
        
        void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override {}
        void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override {}
        void OnIceCandidatesRemoved(const std::vector<cricket::Candidate>& candidates) override {}
        void OnRenegotiationNeeded() override {}
    };

    class InternalCreateSessionDescriptionObserver : public webrtc::CreateSessionDescriptionObserver {
    private:
        WebRtcManager_usbip& manager_;
    public:
        InternalCreateSessionDescriptionObserver(WebRtcManager_usbip& manager) : manager_(manager) {}
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
        WebRtcManager_usbip& manager_;
    public:
        InternalDataChannelObserver(WebRtcManager_usbip& manager) : manager_(manager) {}
        void OnStateChange() override;
        void OnMessage(const webrtc::DataBuffer& buffer) override;
        void OnBufferedAmountChange(uint64_t previous_amount) override {}
    };

    InternalPeerConnectionObserver pc_observer_;
    InternalDataChannelObserver dc_observer_;
};

#endif // WEBRTC_H
