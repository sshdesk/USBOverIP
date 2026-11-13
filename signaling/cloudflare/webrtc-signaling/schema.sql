CREATE TABLE IF NOT EXISTS clients (
    room_id TEXT NOT NULL,
    client_id TEXT NOT NULL,
    joined_at INTEGER NOT NULL,
    PRIMARY KEY (room_id, client_id)
);

CREATE TABLE IF NOT EXISTS messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    room_id TEXT NOT NULL,
    sender_id TEXT NOT NULL,
    target_id TEXT NOT NULL,
    text TEXT NOT NULL,
    timestamp INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_messages_poll 
ON messages (room_id, target_id, sender_id);
