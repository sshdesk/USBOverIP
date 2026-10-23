'use strict';

import { initializeApp } from "https://www.gstatic.com/firebasejs/11.1.0/firebase-app.js";
import { 
    getFirestore, 
    collection, 
    doc, 
    addDoc, 
    setDoc, 
    getDocs, 
    deleteDoc, 
    onSnapshot, 
    query, 
    where,
    initializeFirestore,
    memoryLocalCache
} from "https://www.gstatic.com/firebasejs/11.1.0/firebase-firestore.js";

let global_room_ready_callback = null;
let global_peer_left_callback = null;
let global_sdp_received_callback = null;
let global_ice_received_callback = null;

let db = null;
let app_boot_time = Date.now();

export function InitializeFirebaseFramework(firebaseConfig) {
    const app = initializeApp(firebaseConfig);
    db = initializeFirestore(app, {
        localCache: memoryLocalCache() 
    });
    return true;
}

export async function GetActiveClientCount(roomId) {
    if (!db) return 0;
    const usersCollectionRef = collection(db, `rooms/${roomId}/users`);
    const snapshot = await getDocs(usersCollectionRef);
    let active_count = 0;
    const current_time = Date.now();

    snapshot.docs.forEach(docSnap => {
        const data = docSnap.data();
        let is_stale = true;
        if (data.lastSeen) {
            const diff = Math.abs(current_time - data.lastSeen);
            if (diff < 30000) { // 30 seconds threshold
                is_stale = false;
            }
        }
        if (!is_stale) {
            active_count++;
        }
    });
    return active_count;
}

export async function IsRoomEmpty(roomId) {
    const count = await GetActiveClientCount(roomId);
    if (window.log) window.log(`[Room Check] Room [${roomId}] current active client count: ${count}`);
    return (count === 0);
}

export async function VerifyAndReserveRoomCapacity(roomId) {
    if (!db) return false;
    const count = await GetActiveClientCount(roomId);
    return count < 2;
}

export async function SetUserPresence(roomId, clientId, isOnline) {
    if (!db) return;
    const userDocRef = doc(db, `rooms/${roomId}/users/${clientId}`);
    if (isOnline) {
        await setDoc(userDocRef, { status: "online", lastSeen: Date.now() });
    } else {
        await deleteDoc(userDocRef);
    }
}

export function RegisterOnRoomReadyEvent(callback) { global_room_ready_callback = callback; }
export function RegisterOnPeerLeftEvent(callback) { global_peer_left_callback = callback; }
export function RegisterOnSdpReceivedEvent(callback) { global_sdp_received_callback = callback; }
export function RegisterOnIceReceivedEvent(callback) { global_ice_received_callback = callback; }

export async function HandleGracefulExit(roomId, clientId) {
    if (!db) return;
    
    await addDoc(collection(db, `rooms/${roomId}/messages`), {
        senderId: clientId,
        targetId: "ALL",
        text: "SYSTEM_EVENT: Goodbye! I have closed my connection.",
        timestamp: Date.now()
    });

    await SetUserPresence(roomId, clientId, false);

    const usersSnapshot = await getDocs(collection(db, `rooms/${roomId}/users`));
    if (usersSnapshot.size === 0) {
        const msgs = await getDocs(collection(db, `rooms/${roomId}/messages`));
        for (const m of msgs.docs) await deleteDoc(m.ref);
        await deleteDoc(doc(db, `rooms/${roomId}`));
    }
}

export function ListenForTargetedMessages(roomId, myClientId) {
    if (!db) return;

    const messagesCollectionRef = collection(db, `rooms/${roomId}/messages`);
    const usersCollectionRef = collection(db, `rooms/${roomId}/users`);

    onSnapshot(usersCollectionRef, (snapshot) => {
        snapshot.docChanges().forEach((change) => {
            const userId = change.doc.id;
            if (userId === myClientId) return;
            
            if (change.type === "added") {
                if (window.log) window.log(`[ROOM PRESENCE] Peer entered room active pool: ${userId}`, "success");
            } else if (change.type === "removed") {
                if (window.log) window.log(`[ROOM PRESENCE] Peer left room active pool: ${userId}`, "error");
                if (global_peer_left_callback) global_peer_left_callback(userId);
            }
        });
    });

    const targetedQuery = query(
        messagesCollectionRef,
        where("targetId", "in", ["ALL", myClientId]),
        where("timestamp", ">", app_boot_time) 
    );

    onSnapshot(targetedQuery, (snapshot) => {
        snapshot.docChanges().forEach((change) => {
            if (change.type === "added") {
                const data = change.doc.data();
                if (data.senderId === myClientId) return;

                if (data.text === "SYSTEM_EVENT: ROOM_FULL_READY_FOR_WEBRTC") {
                    if (global_room_ready_callback) global_room_ready_callback(roomId);
                    return;
                }
                if (data.text === "SYSTEM_EVENT: Goodbye! I have closed my connection.") {
                    if (global_peer_left_callback) global_peer_left_callback(data.senderId);
                    return;
                }
                if (data.text.startsWith("SDP_OFFER:")) {
                    if (global_sdp_received_callback) global_sdp_received_callback(data.senderId, "offer", data.text.substr(10));
                    return;
                }
                if (data.text.startsWith("SDP_ANSWER:")) {
                    if (global_sdp_received_callback) global_sdp_received_callback(data.senderId, "answer", data.text.substr(11));
                    return;
                }
                if (data.text.startsWith("ICE:")) {
                    if (global_ice_received_callback) {
                        const parts = data.text.substr(4).split(":");
                        const mid = parts[0];
                        const mline = parseInt(parts[1], 10);
                        const cand_sdp = parts.slice(2).join(":");
                        global_ice_received_callback(data.senderId, mid, mline, cand_sdp);
                    }
                    return;
                }
            }
        });
    });

    getDocs(usersCollectionRef).then((usersSnap) => {
        if (usersSnap.size === 2) {
            SendTargetedMessage(roomId, myClientId, "ALL", "SYSTEM_EVENT: ROOM_FULL_READY_FOR_WEBRTC");
        }
    });
}

export function SendTargetedMessage(roomId, myClientId, targetId, textPayload) {
    if (!db) return;
    addDoc(collection(db, `rooms/${roomId}/messages`), {
        senderId: myClientId,
        targetId: targetId,
        text: textPayload,
        timestamp: Date.now()
    });
}