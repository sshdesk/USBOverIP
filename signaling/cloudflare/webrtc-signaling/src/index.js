export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const pathname = url.pathname;

    const corsHeaders = {
      "Access-Control-Allow-Origin": "*",
      "Access-Control-Allow-Methods": "GET, POST, DELETE, OPTIONS",
      "Access-Control-Allow-Headers": "Content-Type",
    };

    if (request.method === "OPTIONS") {
      return new Response(null, { status: 204, headers: corsHeaders });
    }

    const sendJson = (status, data) =>
      new Response(JSON.stringify(data), {
        status,
        headers: { "Content-Type": "application/json", ...corsHeaders },
      });

    let jsonBody = {};
    if (request.method === "POST" || request.method === "DELETE") {
      try {
        const raw = await request.text();
        if (raw) jsonBody = JSON.parse(raw);
      } catch (e) {
        return sendJson(400, { error: "Invalid JSON body" });
      }
    }

    // 1. POST /prod/join
    if (request.method === "POST" && pathname === "/prod/join") {
      const { room_id, client_id } = jsonBody;
      if (!room_id || !client_id) {
        return sendJson(400, { error: "Missing room_id or client_id" });
      }

      await env.DB.prepare(
        `INSERT INTO clients (room_id, client_id, joined_at)
         VALUES (?, ?, ?)
         ON CONFLICT(room_id, client_id) DO UPDATE SET joined_at = excluded.joined_at`
      )
        .bind(room_id, client_id, Date.now())
        .run();

      const { results } = await env.DB.prepare(
        `SELECT client_id FROM clients WHERE room_id = ? ORDER BY joined_at ASC`
      )
        .bind(room_id)
        .all();

      const otherClients = results
        .map((r) => r.client_id)
        .filter((id) => id !== client_id);

      const is_caller = otherClients.length > 0;
      const remote_peer_id = otherClients[0] || "";

      return sendJson(200, { is_caller, remote_peer_id });
    }

    // 2. POST /prod/messages
    if (request.method === "POST" && pathname === "/prod/messages") {
      const { room_id, sender_id, target_id, text } = jsonBody;
      if (!room_id || !sender_id) {
        return sendJson(400, { error: "Missing fields" });
      }

      const serializedText = JSON.stringify(text ?? "");

      await env.DB.prepare(
        `INSERT INTO messages (room_id, sender_id, target_id, text, timestamp)
         VALUES (?, ?, ?, ?, ?)`
      )
        .bind(room_id, sender_id, target_id || "ALL", serializedText, Date.now())
        .run();

      return sendJson(200, { status: "sent" });
    }

    // 3. GET /prod/messages (Atomic fetch + consume)
    if (request.method === "GET" && pathname === "/prod/messages") {
      const room_id = url.searchParams.get("room_id");
      const client_id = url.searchParams.get("client_id");

      if (!room_id || !client_id) {
        return sendJson(200, []);
      }

      const { results } = await env.DB.prepare(
        `DELETE FROM messages
         WHERE room_id = ?
           AND sender_id != ?
           AND (target_id = ? OR target_id = 'ALL')
         RETURNING sender_id, target_id, text, timestamp`
      )
        .bind(room_id, client_id, client_id)
        .all();

      const formatted = (results || [])
        .sort((a, b) => a.timestamp - b.timestamp)
        .map((row) => {
          let parsedText = row.text;
          try {
            parsedText = JSON.parse(row.text);
          } catch (_) {}
          return {
            sender_id: row.sender_id,
            target_id: row.target_id,
            text: parsedText,
            timestamp: row.timestamp,
          };
        });

      return sendJson(200, formatted);
    }

    // 4. POST / DELETE /prod/leave
    if ((request.method === "POST" || request.method === "DELETE") && pathname === "/prod/leave") {
      const { room_id, client_id } = jsonBody;
      if (!room_id || !client_id) {
        return sendJson(400, { error: "Missing room_id or client_id" });
      }

      await env.DB.prepare(
        `DELETE FROM clients WHERE room_id = ? AND client_id = ?`
      )
        .bind(room_id, client_id)
        .run();

      const { results } = await env.DB.prepare(
        `SELECT COUNT(*) AS count FROM clients WHERE room_id = ?`
      )
        .bind(room_id)
        .all();

      if (results[0]?.count === 0) {
        await env.DB.prepare(`DELETE FROM messages WHERE room_id = ?`)
          .bind(room_id)
          .run();
      }

      return sendJson(200, { status: "success" });
    }

    // 5. POST / DELETE /prod/purgeStale
    if ((request.method === "POST" || request.method === "DELETE") && pathname === "/prod/purgeStale") {
      const { room_id } = jsonBody;
      const cutoff = Date.now() - 60 * 1000;
      let cleanedMessages = 0;
      let cleanedClients = 0;

      if (room_id) {
        const msgRes = await env.DB.prepare(
          `DELETE FROM messages WHERE room_id = ? AND timestamp <= ?`
        )
          .bind(room_id, cutoff)
          .run();

        const clientRes = await env.DB.prepare(
          `DELETE FROM clients WHERE room_id = ? AND joined_at <= ?`
        )
          .bind(room_id, cutoff)
          .run();

        cleanedMessages = msgRes.meta?.changes || 0;
        cleanedClients = clientRes.meta?.changes || 0;
      } else {
        const msgRes = await env.DB.prepare(
          `DELETE FROM messages WHERE timestamp <= ?`
        )
          .bind(cutoff)
          .run();

        const clientRes = await env.DB.prepare(
          `DELETE FROM clients WHERE joined_at <= ?`
        )
          .bind(cutoff)
          .run();

        cleanedMessages = msgRes.meta?.changes || 0;
        cleanedClients = clientRes.meta?.changes || 0;
      }

      return sendJson(200, {
        status: "purge_stale_success",
        cleaned_messages: cleanedMessages,
        cleaned_clients: cleanedClients,
        cleaned_count: cleanedMessages + cleanedClients,
      });
    }

    // 6. POST /prod/heartbeat
    if (request.method === "POST" && pathname === "/prod/heartbeat") {
      const { room_id, client_id } = jsonBody;
      if (!room_id || !client_id) {
        return sendJson(400, { error: "Missing room_id or client_id" });
      }

      const result = await env.DB.prepare(
        `UPDATE clients SET joined_at = ? WHERE room_id = ? AND client_id = ?`
      )
        .bind(Date.now(), room_id, client_id)
        .run();

      return sendJson(200, {
        status: "ok",
        updated: result.meta?.changes || 0,
      });
    }

    return sendJson(404, { error: "Not Found" });
  },

  async scheduled(event, env, ctx) {
    const cutoff = Date.now() - 60 * 1000;
    await env.DB.prepare(`DELETE FROM messages WHERE timestamp <= ?`)
      .bind(cutoff)
      .run();
    await env.DB.prepare(`DELETE FROM clients WHERE joined_at <= ?`)
      .bind(cutoff)
      .run();
  },
};