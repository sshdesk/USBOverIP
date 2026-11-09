#include "tcptunnel.h"
#include <iostream>
#include <spdlog/spdlog.h>

#if defined(_WIN32) || defined(_WIN64)
#include <winsock2.h>
inline void CloseSocketCrossPlatform(SOCKET s) { closesocket(s); }
inline int GetLastSocketError() { return WSAGetLastError(); }
inline bool IsWouldBlockError(int err) { return err == WSAEWOULDBLOCK; }
inline void SetSocketNonBlocking(SOCKET sock) {
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
}
#else
#include <fcntl.h>
inline void CloseSocketCrossPlatform(SOCKET s) { close(s); }
inline int GetLastSocketError() { return errno; }
inline bool IsWouldBlockError(int err) { return err == EWOULDBLOCK || err == EAGAIN; }
inline void SetSocketNonBlocking(SOCKET sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}
#endif


inline void EnableTcpKeepAlive(SOCKET sock) {
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&opt), sizeof(opt));
}

WebRtcToTcpTunnel::WebRtcToTcpTunnel(std::atomic<SOCKET>& client_socket, rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel)
  : client_socket_(client_socket)
  , _dataChannel(data_channel)
{
}

void WebRtcToTcpTunnel::OnStateChange()
{
  SPDLOG_INFO("DataChannel [ {} ] state changed to: {}", _dataChannel->label(), webrtc::DataChannelInterface::DataStateString(_dataChannel->state()));

  webrtc::DataChannelInterface::DataState state = _dataChannel->state();
  if (state == webrtc::DataChannelInterface::kClosed)
  {
    _dataChannel->UnregisterObserver();

    // Proactively drop and close any active TCP client socket when WebRTC drops
    SOCKET socket_snap = client_socket_.exchange(INVALID_SOCKET);
    if (socket_snap != INVALID_SOCKET)
    {
      SPDLOG_WARN("WebRTC DataChannel closed. Forcibly closing active TCP client socket.");
      CloseSocketCrossPlatform(socket_snap);
    } 
   
   if (state == webrtc::DataChannelInterface::kOpen)
   {
     //Test code
    //std::vector<uint8_t> raw_binary { 1, 2, 3 };
    //rtc::CopyOnWriteBuffer buffer(raw_binary.data(), raw_binary.size());
    //webrtc::DataBuffer data_buffer(buffer, true);
    //_dataChannel->Send(data_buffer);
    }	
 
  }
}

void WebRtcToTcpTunnel::OnMessage(const webrtc::DataBuffer& buffer)
{
  if (!buffer.binary)
  {
    SPDLOG_ERROR("TCP Tunnel only supports binary data.");
    return;
  }

  SPDLOG_INFO("TCP Tunnel received {} bytes from data channel", (int)buffer.data.size());
  SOCKET socket_snap = client_socket_.load();
  if (socket_snap != INVALID_SOCKET)
  {
    const uint8_t* raw_data = buffer.data.data();
    size_t data_size        = buffer.data.size();
    size_t total_sent       = 0;

    while (total_sent < data_size)
    {
      int bytes_sent = send(socket_snap, reinterpret_cast<const char*>(raw_data + total_sent), static_cast<int>(data_size - total_sent), 0);
      if (bytes_sent == SOCKET_ERROR)
      {
        SPDLOG_ERROR("TCP Tunnel write failed. Evicting client");
        CloseSocketCrossPlatform(socket_snap);
        client_socket_.store(INVALID_SOCKET);
        break;
      }
      total_sent += bytes_sent;
    }
  }
}

WebRtcTcpTunnelManager::WebRtcTcpTunnelManager(int port)
  : port_(port)
  , running_(false)
  , listen_socket_(INVALID_SOCKET)
  , client_socket_(INVALID_SOCKET)
  , tcp_server_started_callback_(nullptr)
  , tcp_server_stopped_callback_(nullptr)
  , tcp_client_connected_callback_(nullptr)
  , tcp_client_disconnected_callback_(nullptr)
{
  int tcpport = 3240;
  if (tcpport > 1024)
  {
    port_ = tcpport;
  }
}

WebRtcTcpTunnelManager::~WebRtcTcpTunnelManager()
{
  Stop();
}

void WebRtcTcpTunnelManager::RegisterOnTcpServerStartedCallback(TcpServerStartedCallback callback)
{
  tcp_server_started_callback_ = std::move(callback);
}

void WebRtcTcpTunnelManager::RegisterOnTcpServerStoppedCallback(TcpServerStoppedCallback callback)
{
  tcp_server_stopped_callback_ = std::move(callback);
}

void WebRtcTcpTunnelManager::RegisterOnTcpClientConnectedCallback(TcpClientConnectedCallback callback)
{
  tcp_client_connected_callback_ = std::move(callback);
}

void WebRtcTcpTunnelManager::RegisterOnTcpClientDisconnectedCallback(TcpClientDisconnectedCallback callback)
{
  tcp_client_disconnected_callback_ = std::move(callback);
}

bool WebRtcTcpTunnelManager::Start()
{
#if defined(_WIN32) || defined(_WIN64)
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
  {
    SPDLOG_ERROR("TCP Server [Winsock] Startup failed.");
    return false;
  }
#endif

  struct addrinfo hints = {}, *result = nullptr;
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags    = AI_PASSIVE;

  getaddrinfo(nullptr, std::to_string(port_).c_str(), &hints, &result);

  SOCKET listen_sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
  if (listen_sock == INVALID_SOCKET)
  {
    freeaddrinfo(result);
#if defined(_WIN32) || defined(_WIN64)
    WSACleanup();
#endif
    return false;
  }

  EnableTcpKeepAlive(listen_sock);

#ifndef _WIN32
  int opt = 1;
  setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

  if (bind(listen_sock, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR)
  {
    CloseSocketCrossPlatform(listen_sock);
    freeaddrinfo(result);
#if defined(_WIN32) || defined(_WIN64)
    WSACleanup();
#endif
    return false;
  }

  freeaddrinfo(result);
  listen_socket_.store(listen_sock);

  if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR)
  {
    CloseSocketCrossPlatform(listen_sock);
#if defined(_WIN32) || defined(_WIN64)
    WSACleanup();
#endif
    return false;
  }

  running_.store(true);
  server_thread_ = std::thread(&WebRtcTcpTunnelManager::RunServerLoop, this);
  SPDLOG_INFO("TCP Server Listening on port {}", port_);

  if (tcp_server_started_callback_)
  {
    tcp_server_started_callback_();
  }

  return true;
}

void WebRtcTcpTunnelManager::Stop()
{
  if (!running_.exchange(false))
    return;

    
 tunnel_observer_.reset();
  
  SOCKET listen_snap = listen_socket_.exchange(INVALID_SOCKET);
  SOCKET client_snap = client_socket_.exchange(INVALID_SOCKET);

  if (listen_snap != INVALID_SOCKET)
    CloseSocketCrossPlatform(listen_snap);
  
  if (client_snap != INVALID_SOCKET)
  {
    SPDLOG_INFO("TCP Client connection closed due to server shutdown.");
    CloseSocketCrossPlatform(client_snap);
    if (tcp_client_disconnected_callback_)
    {
      tcp_client_disconnected_callback_();
    }
  }

#if defined(_WIN32) || defined(_WIN64)
  WSACleanup();
#endif

  if (server_thread_.joinable())
    server_thread_.join();
  if (proxy_thread_.joinable())
    proxy_thread_.join();

  if (tcp_server_stopped_callback_)
  {
    tcp_server_stopped_callback_();
  }
  
  
}

void WebRtcTcpTunnelManager::RunServerLoop()
{
  while (running_.load())
  {
    SOCKET listen_sock = listen_socket_.load();
    if (listen_sock == INVALID_SOCKET)
      break;

    SOCKET client_sock = accept(listen_sock, nullptr, nullptr);
    if (client_sock != INVALID_SOCKET)
    {
      SPDLOG_INFO("TCP Local client connected.");
      EnableTcpKeepAlive(client_sock);
      SetSocketNonBlocking(client_sock);

      SOCKET old_client = client_socket_.exchange(client_sock);
      if (old_client != INVALID_SOCKET)
      {
        CloseSocketCrossPlatform(old_client);
        if (tcp_client_disconnected_callback_)
        {
          tcp_client_disconnected_callback_();
        }
      }

      if (tcp_client_connected_callback_)
      {
        tcp_client_connected_callback_();
      }
    }
  }
}

void WebRtcTcpTunnelManager::HandleDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel, rtc::Thread* webrtc_thread)
{
  tunnel_observer_ = std::make_unique<WebRtcToTcpTunnel>(client_socket_, data_channel);
  data_channel->RegisterObserver(tunnel_observer_.get());

  if (proxy_thread_.joinable())
  {
    proxy_thread_.join();
  }
  proxy_thread_ = std::thread(&WebRtcTcpTunnelManager::RunTcpToWebRtcLoop, this, data_channel, webrtc_thread);
}

void WebRtcTcpTunnelManager::RunTcpToWebRtcLoop(rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel, rtc::Thread* webrtc_thread)
{
  std::vector<char> recvbuf(65536);
  const uint64_t kMaxWebRtcBuffer = 1024 * 1024;

  while (running_.load())
  {
    SOCKET socket_snap = client_socket_.load();
    if (socket_snap == INVALID_SOCKET)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    if (!data_channel || data_channel->state() != webrtc::DataChannelInterface::kOpen)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }

    if (data_channel->buffered_amount() > kMaxWebRtcBuffer)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    int bytes_received = recv(socket_snap, recvbuf.data(), static_cast<int>(recvbuf.size()), 0);

    if (bytes_received > 0)
    {
      SPDLOG_INFO("TCP Tunnel received {} bytes from local client.", bytes_received);
      std::vector<uint8_t> raw_binary(recvbuf.begin(), recvbuf.begin() + bytes_received);

      webrtc_thread->PostTask(
          [data_channel, raw_binary = std::move(raw_binary)]()
          {
            if (data_channel->state() == webrtc::DataChannelInterface::kOpen)
            {
              rtc::CopyOnWriteBuffer buffer(raw_binary.data(), raw_binary.size());
              webrtc::DataBuffer data_buffer(buffer, true);
              data_channel->Send(data_buffer);
            }
          });
    }
    else if (bytes_received == 0)
    {
      SPDLOG_INFO("TCP Local client disconnected.");
      CloseSocketCrossPlatform(socket_snap);
      client_socket_.store(INVALID_SOCKET);
      if (tcp_client_disconnected_callback_)
      {
        tcp_client_disconnected_callback_();
      }
    }
    else
    {
      int err = GetLastSocketError();
      if (!IsWouldBlockError(err))
      {
        CloseSocketCrossPlatform(socket_snap);
        client_socket_.store(INVALID_SOCKET);
        if (tcp_client_disconnected_callback_)
        {
          tcp_client_disconnected_callback_();
        }
      }
    }
  }
}
