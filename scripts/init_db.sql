-- chat_system schema
-- Idempotent: re-running drops nothing, only creates missing objects.

CREATE DATABASE IF NOT EXISTS chat_system
  CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

USE chat_system;

CREATE TABLE IF NOT EXISTS users (
  id              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  username        VARCHAR(64)  NOT NULL,
  password_hash   VARCHAR(256) NOT NULL,    -- "iter$saltB64$hashB64"
  created_at      TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
  last_seen_at    TIMESTAMP    NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uq_users_username (username)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS sessions (
  token           CHAR(64)    NOT NULL,     -- 32-byte random hex-encoded
  user_id         BIGINT UNSIGNED NOT NULL,
  created_at      TIMESTAMP   NOT NULL DEFAULT CURRENT_TIMESTAMP,
  last_active_at  TIMESTAMP   NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (token),
  KEY idx_sessions_user (user_id),
  CONSTRAINT fk_sessions_user FOREIGN KEY (user_id) REFERENCES users(id)
    ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS rooms (
  id              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  name            VARCHAR(128) NOT NULL,
  created_at      TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uq_rooms_name (name)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS room_members (
  room_id         BIGINT UNSIGNED NOT NULL,
  user_id         BIGINT UNSIGNED NOT NULL,
  joined_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (room_id, user_id),
  KEY idx_room_members_user (user_id),
  CONSTRAINT fk_rm_room FOREIGN KEY (room_id) REFERENCES rooms(id) ON DELETE CASCADE,
  CONSTRAINT fk_rm_user FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- Messages: a single table backs both 1:1 DMs (room_id IS NULL) and rooms
-- (receiver_id IS NULL). Composite indexes target the two read patterns:
-- (a) DM history between two users; (b) recent room messages.
CREATE TABLE IF NOT EXISTS messages (
  id              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  sender_id       BIGINT UNSIGNED NOT NULL,
  receiver_id     BIGINT UNSIGNED NULL,
  room_id         BIGINT UNSIGNED NULL,
  content         TEXT NOT NULL,
  status          TINYINT UNSIGNED NOT NULL DEFAULT 0,  -- 0 sent, 1 delivered, 2 read
  created_at      TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (id),
  KEY idx_messages_dm (sender_id, receiver_id, id),
  KEY idx_messages_dm_rev (receiver_id, sender_id, id),
  KEY idx_messages_room (room_id, id),
  CONSTRAINT fk_msg_sender   FOREIGN KEY (sender_id)   REFERENCES users(id) ON DELETE CASCADE,
  CONSTRAINT fk_msg_receiver FOREIGN KEY (receiver_id) REFERENCES users(id) ON DELETE CASCADE,
  CONSTRAINT fk_msg_room     FOREIGN KEY (room_id)     REFERENCES rooms(id) ON DELETE CASCADE,
  CONSTRAINT chk_msg_target  CHECK ((receiver_id IS NULL) <> (room_id IS NULL))
) ENGINE=InnoDB;

-- Offline queue: undelivered DMs waiting for the recipient to come online.
-- Persisted separately so we don't have to scan messages on reconnect.
CREATE TABLE IF NOT EXISTS undelivered (
  message_id      BIGINT UNSIGNED NOT NULL,
  user_id         BIGINT UNSIGNED NOT NULL,
  PRIMARY KEY (user_id, message_id),
  CONSTRAINT fk_und_msg  FOREIGN KEY (message_id) REFERENCES messages(id) ON DELETE CASCADE,
  CONSTRAINT fk_und_user FOREIGN KEY (user_id)    REFERENCES users(id)    ON DELETE CASCADE
) ENGINE=InnoDB;
