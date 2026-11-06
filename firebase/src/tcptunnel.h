#pragma once
#ifndef TCPTUNNEL_H
#define TCPTUNNEL_H

#include <atomic>
#include <memory>
#include <thread>
#include <vector>
#include <functional>
#include <chrono>

#if defined(_WIN32) || defined(_WIN64)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#ifndef SOCKET_H_DEFINED
#define SOCKET_H_DEFINED
using SOCKET = int;

#undef INVALID_SOCKET
const SOCKET INVALID_SOCKET = -1;

#undef SOCKET_ERROR
const int SOCKET_ERROR = -1;
#endif
#endif

#include "api/data_channel_interface.h"
#include "api/peer_connection_interface.h"
#include "rtc_base/copy_on_write_buffer.h"
#include "rtc_base/thread.h"

class WebRtcToTcpTunnel : public webrtc::DataChannelObserver
{
public:
  using DataChannelTotalReceivedCallback = std::function<void(uint64_t total_bytes)>;

  WebRtcToTcpTunnel(std::atomic<SOCKET>& client_socket, 
                    rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel,
                    DataChannelTotalReceivedCallback callback);
  
  virtual ~WebRtcToTcpTunnel() {
   _dataChannel = nullptr;
  }

  void OnStateChange() override;
  void OnMessage(const webrtc::DataBuffer& buffer) override;

private:
  std::atomic<SOCKET>& client_socket_;
  rtc::scoped_refptr<webrtc::DataChannelInterface> _dataChannel;
  DataChannelTotalReceivedCallback total_received_callback_;
  
  uint64_t total_bytes_received_;
  std::chrono::steady_clock::time_point last_callback_time_;
};

class WebRtcTcpTunnelManager
{
public:
  using TcpServerStartedCallback = std::function<void()>;
  using TcpServerStoppedCallback = std::function<void()>;
  using TcpClientConnectedCallback = std::function<void()>;
  using TcpClientDisconnectedCallback = std::function<void()>;
  using DataChannelTotalReceivedCallback = WebRtcToTcpTunnel::DataChannelTotalReceivedCallback;

  WebRtcTcpTunnelManager(int port);
  ~WebRtcTcpTunnelManager();

  bool Start();
  void Stop();
  void HandleDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel, rtc::Thread* webrtc_thread);
  
  void RegisterOnTcpServerStartedCallback(TcpServerStartedCallback callback);
  void RegisterOnTcpServerStoppedCallback(TcpServerStoppedCallback callback);
  void RegisterOnTcpClientConnectedCallback(TcpClientConnectedCallback callback);
  void RegisterOnTcpClientDisconnectedCallback(TcpClientDisconnectedCallback callback);
  void RegisterOnDataChannelTotalReceivedCallback(DataChannelTotalReceivedCallback callback);

private:
  void RunServerLoop();
  void RunTcpToWebRtcLoop(rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel, rtc::Thread* webrtc_thread);

  int port_;
  std::atomic<bool> running_;
  std::atomic<SOCKET> listen_socket_;
  std::atomic<SOCKET> client_socket_;

  std::thread server_thread_;
  std::thread proxy_thread_;
  std::unique_ptr<WebRtcToTcpTunnel> tunnel_observer_;
  
  TcpServerStartedCallback tcp_server_started_callback_;
  TcpServerStoppedCallback tcp_server_stopped_callback_;
  TcpClientConnectedCallback tcp_client_connected_callback_;
  TcpClientDisconnectedCallback tcp_client_disconnected_callback_;
  DataChannelTotalReceivedCallback data_channel_total_received_callback_;
};

#endif // TCPTUNNEL_H