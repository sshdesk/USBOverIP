'use strict';

import {
    InitializeFirebaseFramework,
    VerifyAndReserveRoomCapacity,
    IsRoomEmpty,
    SetUserPresence,
    HandleGracefulExit,
    ListenForTargetedMessages,
    SendTargetedMessage,
    RegisterOnRoomReadyEvent,
    RegisterOnPeerLeftEvent,
    RegisterOnSdpReceivedEvent,
    RegisterOnIceReceivedEvent
} from "./signalling.js";

const firebaseConfig = {
    apiKey: "AIzaSyDrWo7Fz0I5C_8KIAlp1PfxnCmZ4nuRvQc",
    authDomain: "webrtc-signaling-9316c.firebaseapp.com",
    projectId: "webrtc-signaling-9316c",
    storageBucket: "webrtc-signaling-9316c.firebasestorage.app",
    messagingSenderId: "856316427805",
    appId: "1:856316427805:web:2b45fc786b9f9c33dd50f4",
    measurementId: "G-W7Z0PZJGDL"
};

const global_client_id = "web_peer_" + Math.floor(Math.random() * 90000 + 1000);
let active_room_id = "";
let host_server = false; // true = Caller (Client role), false = Callee (Host Server role)

let peer_connection = null;
let data_channel = null;

const logCanvas = document.getElementById("logCanvas");
const roomInput = document.getElementById("roomInput");

window.log = function(text, type = "info") {
    const time = new Date().toISOString().split('T')[1].slice(0, 8);
    const line = document.createElement("div");
    line.className = `log-${type}`;
    line.innerHTML = `[${time}] ${text}`;
    logCanvas.appendChild(line);
    logCanvas.scrollTop = logCanvas.scrollHeight;
};

window.log(`Your Client Identity Token: [${global_client_id}]`, "success");

const rtcConfig = {
    iceServers: [{ urls: "stun:stun.l.google.com:19302" }]
};

RegisterOnRoomReadyEvent((roomId) => {
    window.log(`=================================================================`, "warn");
    window.log(`[EVENT TRIGGERED] Room [${roomId}] is full and ready for meeting!`, "success");
    window.log(`=================================================================`, "warn");

    if (!host_server) {
        window.log("[Client Init] Instantiating caller node to generate connection offer...", "info");
        InitializeWebRtcStack(true);
    }
});

RegisterOnPeerLeftEvent((peerClientId) => {
    window.log(`=================================================================`, "error");
    window.log(`[EVENT NOTICE] Peer [${peerClientId}] has disconnected or left the room.`, "error");
    window.log(`=================================================================`, "error");

    ShutdownWebRtcConnection();
});

RegisterOnSdpReceivedEvent((senderId, type, sdp) => {
    if (!peer_connection) return;
    window.log(`[Signaling Event] Received remote SDP ${type} from peer: ${senderId}`);

    peer_connection.setRemoteDescription(new RTCSessionDescription({ type: type, sdp: sdp }))
        .then(() => {
            if (type === "offer") {
                window.log("[WebRTC] Remote Offer assigned. Formulating local Answer mapping...", "info");
                return peer_connection.createAnswer();
            }
        })
        .then(desc => {
            if (desc) handleLocalDescription(desc);
        })
        .catch(err => window.log(`[WebRTC Error] SDP routing handling fault: ${err.message}`, "error"));
});

RegisterOnIceReceivedEvent((senderId, mid, mlineIndex, sdp) => {
    if (!peer_connection) return;
    window.log(`[Signaling Event] Received remote ICE candidate (sdpMid: ${mid}) from peer: ${senderId}`);
    peer_connection.addIceCandidate(new RTCIceCandidate({ candidate: sdp, sdpMid: mid, sdpMLineIndex: mlineIndex }))
        .catch(err => window.log(`[WebRTC Error] Candidate routing rejection: ${err.message}`, "error"));
});

function InitializeWebRtcStack(isCaller) {
    window.log("[WebRTC Init] Instantiating core PeerConnection engine context...");
    peer_connection = new RTCPeerConnection(rtcConfig);

    peer_connection.onicecandidate = (event) => {
        if (event.candidate) {
            window.log(`[WebRTC Callback] New local ICE candidate gathered! -> sdpMid: ${event.candidate.sdpMid}`, "info");
            const ice_packet = `ICE:${event.candidate.sdpMid}:${event.candidate.sdpMLineIndex}:${event.candidate.candidate}`;
            SendTargetedMessage(active_room_id, global_client_id, "ALL", ice_packet);
        }
    };

    peer_connection.oniceconnectionstatechange = () => {
        window.log(`==> [WebRTC ICE NETWORK CHANGED]: Status -> ${peer_connection.iceConnectionState}`, "info");
    };

    peer_connection.onconnectionstatechange = () => {
        window.log(`==> [WebRTC CORE PIPELINE CHANGED]: Status -> ${peer_connection.connectionState}`, "info");
    };

    if (isCaller) {
        window.log("[WebRTC Init] Client is Caller: Generating proactive local tracking DataChannel...", "info");
        data_channel = peer_connection.createDataChannel("chat-channel", { ordered: true });
        bindDataChannelObservers(data_channel);

        window.log("[WebRTC] Initiating SDP Offer creation...", "warn");
        peer_connection.createOffer({ offerToReceiveAudio: false, offerToReceiveVideo: false })
            .then(desc => handleLocalDescription(desc))
            .catch(err => window.log(`[WebRTC Error] Offer creation crash: ${err.message}`, "error"));
    } else {
        window.log("[WebRTC] Callee standing by. Awaiting remote host SDP Offer from pipeline channel...", "info");
        peer_connection.ondatachannel = (event) => {
            window.log("[WebRTC Callback] Remote negotiated DataChannel caught! Binding Callee observer loops...", "warn");
            data_channel = event.channel;
            bindDataChannelObservers(data_channel);
        };
    }
}

function handleLocalDescription(desc) {
    peer_connection.setLocalDescription(desc).then(() => {
        window.log("[WebRTC] Local description applied safely via setLocalDescription().", "info");
        const prefix = (desc.type === "offer") ? "SDP_OFFER:" : "SDP_ANSWER:";
        SendTargetedMessage(active_room_id, global_client_id, "ALL", prefix + desc.sdp);
    });
}

function bindDataChannelObservers(channel) {
    channel.onopen = () => {
        window.log("DataChannel Open! You can now send messages.", "success");
        window.log("[WebRTC] Data Channel connected and OPEN. Sending initial greeting...", "warn");
        channel.send(`Hello from peer identity token: [${global_client_id}]`);
    };

    channel.onclose = () => {
        window.log("[WebRTC DataChannel State]: Closed.", "error");
    };

    channel.onmessage = (event) => {
        window.log(`[DATA CHANNEL RECEIVED BUFFER]: ${event.data}`, "chat");
    };
}

function ShutdownWebRtcConnection() {
    window.log("[WebRTC TearDown] Closing active loops and flushing peer contexts...", "info");
    if (data_channel) { data_channel.close(); data_channel = null; }
    if (peer_connection) { peer_connection.close(); peer_connection = null; }

    if (host_server) {
        window.log("[WebRTC State Reset] Standing by as Host. Waiting for new peer connection...", "success");
    } else {
        window.log("Host disconnected. Exiting signaling session room.", "error");
        active_room_id = "";
    }
}

window.onbeforeunload = () => {
    HandleGracefulExit(active_room_id, global_client_id);
};

async function connectRoom() {
    active_room_id = roomInput.value.trim();
    if (!active_room_id) {
        window.log("Error: Room ID Input layout cannot be empty.", "error");
        return;
    }

    if (!InitializeFirebaseFramework(firebaseConfig)) {
        window.log("Critical Error: Firebase authentication crash.", "error");
        return;
    }

    document.getElementById("setupProfile").style.display = "none";
    
    // Automatically determine role based on whether the room is empty (matches C++ logic)
    host_server = await IsRoomEmpty(active_room_id);
    window.log(`[Room Join] Target room ID: ${active_room_id}. Assigned Role -> ${host_server ? "Host Server (Callee)" : "Client (Caller)"}`);

    window.log(`Evaluating capacity constraints for room matrix [${active_room_id}]...`);
    const isAvailable = await VerifyAndReserveRoomCapacity(active_room_id);
    if (!isAvailable) {
        window.log("[CRITICAL REJECT] Access Denied: Room capacity limit reached.", "error");
        return;
    }

    await SetUserPresence(active_room_id, global_client_id, true);
    ListenForTargetedMessages(active_room_id, global_client_id);

    // If we are the second person joining an existing room, initialize WebRTC receiver stack immediately
    if (host_server) {
        window.log("[WebRTC Init] Standing by as Callee. Initializing WebRTC stack to receive incoming offers...", "info");
        InitializeWebRtcStack(false);
    }
}

document.getElementById("btnConnect").onclick = connectRoom;

window.sendMessage = (inputString) => {
    const splitPos = inputString.indexOf(":");
    if (splitPos !== -1) {
        const target = inputString.substr(0, splitPos);
        const body = inputString.substr(splitPos + 1);
        SendTargetedMessage(active_room_id, global_client_id, target, body);
    } else {
        window.log("Format violation. Use standard 'target_id:message' values inside parameters.", "warn");
    }
};