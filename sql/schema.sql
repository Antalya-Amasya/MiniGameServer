CREATE TABLE IF NOT EXISTS users (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  username VARCHAR(32) NOT NULL,
  password_hash VARCHAR(100) NOT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  last_login_at TIMESTAMP NULL DEFAULT NULL,
  PRIMARY KEY (id),
  UNIQUE KEY users_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS game_records (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  room_id INT NOT NULL,
  player_x_id BIGINT UNSIGNED NOT NULL,
  player_o_id BIGINT UNSIGNED NOT NULL,
  winner CHAR(1) NULL,
  result VARCHAR(32) NOT NULL,
  started_at TIMESTAMP NOT NULL,
  finished_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY game_once (room_id, started_at),
  KEY game_player_x (player_x_id),
  KEY game_player_o (player_o_id),
  KEY game_finished (finished_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
