import { APIGatewayProxyHandler } from 'aws-lambda';

// In-memory rooms configuration stays warm during active container execution
let rooms: Record<string, { clients: string[]; messages: any[] }> = {};

export const handler: APIGatewayProxyHandler = async (event) => {
  const { path, httpMethod, queryStringParameters } = event;

  const headers = {
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Headers': 'Content-Type',
    'Access-Control-Allow-Methods': 'GET, POST, OPTIONS, DELETE',
    'Content-Type': 'application/json'
  };

  if (httpMethod === 'OPTIONS') {
    return { statusCode: 204, headers, body: '' };
  }

  let body: any = {};
  if (event.body) {
    try { body = JSON.parse(event.body); } catch (e) {}
  }

  // 1. POST /join
  if (httpMethod === 'POST' && path === '/join') {
    const { room_id, client_id } = body;
    if (!room_id || !client_id) {
      return { statusCode: 400, headers, body: JSON.stringify({ error: "Missing room_id or client_id" }) };
    }

    if (!rooms[room_id]) rooms[room_id] = { clients: [], messages: [] };
    if (!rooms[room_id].clients.includes(client_id)) rooms[room_id].clients.push(client_id);

    const otherClients = rooms[room_id].clients.filter(id => id !== client_id);
    return {
      statusCode: 200,
      headers,
      body: JSON.stringify({ is_caller: otherClients.length > 0, remote_peer_id: otherClients })
    };
  }

  // 2. POST /messages
  if (httpMethod === 'POST' && path === '/messages') {
    const { room_id, sender_id, target_id, text } = body;
    if (!room_id || !sender_id) {
      return { statusCode: 400, headers, body: JSON.stringify({ error: "Missing fields" }) };
    }

    if (!rooms[room_id]) rooms[room_id] = { clients: [], messages: [] };
    rooms[room_id].messages.push({ sender_id, target_id: target_id || "ALL", text, timestamp: Date.now() });

    return { statusCode: 200, headers, body: JSON.stringify({ status: "sent" }) };
  }

  // 3. GET /messages
  if (httpMethod === 'GET' && path === '/messages') {
    const room_id = queryStringParameters?.room_id;
    const client_id = queryStringParameters?.client_id;

    if (!room_id || !rooms[room_id]) {
      return { statusCode: 200, headers, body: JSON.stringify([]) };
    }

    const messages = rooms[room_id].messages.filter(m => 
      m.sender_id !== client_id && (m.target_id === client_id || m.target_id === "ALL")
    );

    rooms[room_id].messages = rooms[room_id].messages.filter(m => 
      !(m.sender_id !== client_id && (m.target_id === client_id || m.target_id === "ALL"))
    );

    return { statusCode: 200, headers, body: JSON.stringify(messages) };
  }

  return { statusCode: 404, headers, body: JSON.stringify({ error: "Not Found" }) };
};

