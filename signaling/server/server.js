const http = require('http');
const url = require('url');

const PORT = 8080;
let rooms = {}; // In-memory store: { room_id: { clients: [], messages: [] } }

const server = http.createServer((req, res) => {
    const parsedUrl = url.parse(req.url, true);
    const pathname = parsedUrl.pathname;

    console.log(`[HTTP Incoming] Method: ${req.method}, Path: ${pathname}, Query:`, JSON.stringify(parsedUrl.query));

    const sendJson = (statusCode, data) => {
        console.log(`[HTTP Response] Status: ${statusCode}, Body:`, JSON.stringify(data));
        res.writeHead(statusCode, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(data));
    };

    let body = '';
    req.on('data', chunk => { body += chunk; });
    req.on('end', () => {
        let jsonBody = {};
        if (body) {
            try { 
                jsonBody = JSON.parse(body); 
                console.log(`[HTTP Body Parsed]:`, JSON.stringify(jsonBody));
            } catch (e) {
                console.log(`[HTTP Body Parse Error]:`, body);
            }
        }

        // 1. POST /prod/join
        if (req.method === 'POST' && pathname === '/prod/join') {
            const { room_id, client_id } = jsonBody;
            if (!room_id || !client_id) return sendJson(400, { error: "Missing room_id or client_id" });

            if (!rooms[room_id]) {
                rooms[room_id] = { clients: [], messages: [] };
                console.log(`[ROOM CREATED] Room ID: ${room_id}`);
            }

            if (!rooms[room_id].clients.includes(client_id)) {
                rooms[room_id].clients.push(client_id);
                console.log(`[CLIENT JOINED] Client ${client_id} added to Room ${room_id}. Current clients:`, rooms[room_id].clients);
            }

            const clients = rooms[room_id].clients;
            const otherClients = clients.filter(id => id !== client_id);

            const is_caller = (otherClients.length > 0);
            const remote_peer = otherClients[0] || "";

            console.log(`[JOIN RESULT] Room: ${room_id}, Client: ${client_id}, IsCaller: ${is_caller}, RemotePeer Assigned: ${remote_peer}`);
            return sendJson(200, { is_caller, remote_peer_id: remote_peer });
        }

        // 2. POST /prod/messages
        if (req.method === 'POST' && pathname === '/prod/messages') {
            const { room_id, sender_id, target_id, text } = jsonBody;
            if (!room_id || !sender_id) return sendJson(400, { error: "Missing fields" });

            if (!rooms[room_id]) rooms[room_id] = { clients: [], messages: [] };
            
            const messageEntry = { sender_id, target_id: target_id || "ALL", text, timestamp: Date.now() };
            rooms[room_id].messages.push(messageEntry);

            const preview = typeof text === 'string' ? text : JSON.stringify(text);
            console.log(`[MSG STORED] Room: ${room_id} | From: ${sender_id} | Target: ${target_id || "ALL"} | Payload Preview: ${preview.substring(0, 60)}...`);
            return sendJson(200, { status: "sent" });
        }

        // 3. GET /prod/messages
        if (req.method === 'GET' && pathname === '/prod/messages') {
            const { room_id, client_id } = parsedUrl.query;
            if (!room_id || !rooms[room_id]) {
                console.log(`[POLL LOG] Room ${room_id} not found or empty for client ${client_id}`);
                return sendJson(200, []);
            }

            const messages = rooms[room_id].messages.filter(m => 
                m.sender_id !== client_id && (m.target_id === client_id || m.target_id === "ALL")
            );

            rooms[room_id].messages = rooms[room_id].messages.filter(m => 
                !(m.sender_id !== client_id && (m.target_id === client_id || m.target_id === "ALL"))
            );

            if (messages.length > 0) {
                console.log(`[POLL DISPATCH] Delivering and consuming ${messages.length} message(s) for client ${client_id} in room ${room_id}`);
            }

            return sendJson(200, messages);
        }

        // 4. POST / DELETE /prod/leave
        if ((req.method === 'POST' || req.method === 'DELETE') && pathname === '/prod/leave') {
            const { room_id, client_id } = jsonBody;
            if (!room_id || !client_id) return sendJson(400, { error: "Missing room_id or client_id" });

            if (rooms[room_id] && rooms[room_id].clients) {
                rooms[room_id].clients = rooms[room_id].clients.filter(id => id !== client_id);
                console.log(`[CLIENT LEFT] Client ${client_id} removed from Room ${room_id}. Remaining clients:`, rooms[room_id].clients);

                if (rooms[room_id].clients.length === 0) {
                    delete rooms[room_id];
                    console.log(`[ROOM DELETED] Room ID: ${room_id} is empty and has been removed.`);
                }
            }

            return sendJson(200, { status: "success" });
        }

        // 5. POST / DELETE /prod/purgeStale (Iterates and removes stale messages/empty rooms)
        if ((req.method === 'POST' || req.method === 'DELETE') && pathname === '/prod/purgeStale') {
            const { room_id } = jsonBody;
            const now = Date.now();
            const STALE_THRESHOLD_MS = 1 * 60 * 1000; // 1 minutes threshold

            let cleanedCount = 0;

            if (room_id && rooms[room_id]) {
                // Purge specific room's stale messages
                const initial = rooms[room_id].messages.length;
                rooms[room_id].messages = rooms[room_id].messages.filter(m => (now - m.timestamp) < STALE_THRESHOLD_MS);
                cleanedCount = initial - rooms[room_id].messages.length;
                console.log(`[PURGE STALE] Room ${room_id}: removed ${cleanedCount} old message(s).`);
            } else {
                // Global sweep across all rooms if no specific room_id provided
                for (const rId of Object.keys(rooms)) {
                    const room = rooms[rId];
                    if (room.messages) {
                        room.messages = room.messages.filter(m => (now - m.timestamp) < STALE_THRESHOLD_MS);
                    }
                    if (!room.clients || room.clients.length === 0) {
                        delete rooms[rId];
                        cleanedCount++;
                        console.log(`[PURGE STALE] Removed empty room: ${rId}`);
                    }
                }
            }

            return sendJson(200, { status: "purge_stale_success", cleaned_count: cleanedCount });
        }

        console.log(`[404 NOT FOUND] Path: ${pathname}`);
        sendJson(404, { error: "Not Found" });
    });
});

server.listen(PORT, () => {
    console.log(`Node.js full-logging local signaling mock server running at http://localhost:${PORT}/prod`);
});