# Online Bomberman

The project is intended to be built and run on Linux or WSL Ubuntu. To install the required tools, run sudo apt update and then sudo apt install build-essential make git. To build the project, run make clean and then make. After a successful build, the server executable will be located at build/server/server, and the client executable will be located at build/client/client. The build/ folder should not be committed to git.

To start the game, first run the server with a port and a map file. For example: ./build/server/server 12345 maps/test.map. After that, start clients in separate terminals. For example: ./build/client/client 127.0.0.1 12345 Player1 and ./build/client/client 127.0.0.1 12345 Player2. The server supports up to 8 connected players.

After connecting, each client enters the lobby. The available client commands are /ready, /ping, /quit, /players, /map, w, a, s, d, and /bomb. The /ready command marks the player as ready. The /ping command checks the connection with the server. The /quit command leaves the game. The /players command prints the current player list. The /map command prints the current map. The keys w, a, s, and d move the player up, left, down, and right. The /bomb command places a bomb on the current player cell.

The game starts when at least 2 players are connected and all active players have used the /ready command. When the game starts, the server sends the game status and the map to all clients. Players can then move around the map and place bombs.

The map is loaded from a text file. The first line contains the map settings: number of rows, number of columns, player speed, explosion danger time in ticks, default bomb radius, and bomb timer in ticks. For example, 6 9 4 5 1 60 means that the map has 6 rows and 9 columns, the player speed is 4, the explosion danger time is 5 ticks, the default bomb radius is 1, and the bomb timer is 60 ticks. Since the server updates the game 20 times per second, 60 ticks are about 3 seconds.

Map cells use simple symbols. A dot means an empty cell. H means a hard block that cannot be destroyed. S means a soft block that can be destroyed by an explosion. Digits from 1 to 8 mean player starting positions. A means a speed bonus. R means a bomb radius bonus. T means a bomb timer bonus. N means a bomb count bonus. B means a bomb.

The server is the only source of truth. Clients do not decide whether a movement is valid, whether a bomb explodes, whether a player dies, or who wins. Clients only send action attempts, such as movement attempts, bomb placement attempts, readiness messages, ping messages, and leave messages. The server checks the rules and sends the final result to all clients.

The main protocol messages currently used by the project include HELLO, WELCOME, SET_READY, SET_STATUS, MAP, MOVE_ATTEMPT, MOVED, BOMB_ATTEMPT, BOMB, EXPLOSION_START, EXPLOSION_END, DEATH, BLOCK_DESTROYED, BONUS_RETRIEVED, WINNER, PING, PONG, LEAVE, ERROR, and DISCONNECT.

The server updates the game at 20 ticks per second. One tick is approximately 50 milliseconds. On each tick, the server accepts messages from clients, updates bomb timers, starts explosions when bomb timers reach zero, ends explosions after their danger time, destroys soft blocks, checks player deaths, and checks whether the game has a winner or a draw.

The currently implemented features include TCP server and client communication, player connection using HELLO and WELCOME, lobby support, ready status, game start after all players are ready, map loading from a file, map broadcasting to clients, movement on a grid, collision checks with walls, blocks, bombs, and other players, bomb placement, bomb timers, cross-shaped explosions, stopping explosions on hard blocks, destroying soft blocks, chain bomb detonation, player death in explosions, bonus pickup, winner detection, and draw detection when all players die.

There are still some limitations. After a round ends, a new round currently requires restarting the server. SYNC_BOARD and SYNC_REQUEST are not implemented yet. BONUS_AVAILABLE is not sent separately because bonuses are currently visible through the map. Map selection by the room creator is not implemented yet because the map is selected when the server starts. Player speed is stored as a value, but movement is currently step-based through terminal commands. There is no round time limit yet. There is no final statistics screen for kills, destroyed blocks, or collected bonuses. The client prints explosion events but does not fully draw the explosion zone on the map.

A simple test flow is the following. Start the server with ./build/server/server 12345 maps/test.map. Start the first client with ./build/client/client 127.0.0.1 12345 Player1. Start the second client with ./build/client/client 127.0.0.1 12345 Player2. In both clients, type /ready. After the game starts, use w, a, s, and d to move, and use /bomb to place a bomb.
