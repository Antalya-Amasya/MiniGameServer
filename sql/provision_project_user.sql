-- Run this once in MySQL Workbench on the saved local MySQL 8.0 connection.
-- Replace the password placeholder before executing. Do not commit the filled-in file.
-- The host pattern matches the current WSL subnet slice (172.31.224.0/24).
-- If the WSL address moves outside that range, add another host for the new address.

CREATE DATABASE IF NOT EXISTS minigame CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
CREATE DATABASE IF NOT EXISTS minigame_test CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE USER IF NOT EXISTS 'minigame_app'@'172.31.224.%' IDENTIFIED BY 'REPLACE_WITH_A_LONG_RANDOM_PASSWORD';

GRANT SELECT, INSERT, UPDATE, DELETE, CREATE, ALTER, INDEX, REFERENCES
  ON minigame.* TO 'minigame_app'@'172.31.224.%';

GRANT SELECT, INSERT, UPDATE, DELETE, CREATE, ALTER, DROP, INDEX, REFERENCES
  ON minigame_test.* TO 'minigame_app'@'172.31.224.%';

FLUSH PRIVILEGES;
