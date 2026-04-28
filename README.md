# Online Bomberman

To build the project run: make clean, then make.

To start the server use: ./build/server/server <port> <map_file>. Example: ./build/server/server 12345 maps/test.map

To start a client use: ./build/client/client <host> <port> <player_name>. Example: ./build/client/client 127.0.0.1 12345 Player1

You need at least two clients to start the game. After connecting, players must type /ready. When all players are ready, the game starts automatically.

Controls are simple. w a s d to move. /bomb to place a bomb on your current cell. /ready to mark yourself ready. /sync to resync the state with the server. /lobby to return to lobby after game ends. /players to show players. /map to print the map. /ping to test connection. /quit to exit.

Game rules are basic. Players move on a grid map. They can place bombs which explode after a timer. Explosion spreads in four directions and kills players, destroys soft blocks and can trigger other bombs. Hard blocks cannot be destroyed. Bonuses can increase speed, bomb radius, bomb count or timer.

The round ends when only one player remains alive, when all players die or when the time limit is reached. After that, the server sends the winner and round statistics including kills, destroyed blocks and collected bonuses.

Client renders the map in the terminal. Players are shown as numbers, bombs as B, explosions as *, walls and bonuses are shown directly from the map.