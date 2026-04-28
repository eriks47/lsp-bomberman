#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define TARGET_BROADCAST 254
#define TARGET_SERVER 255

#define CLIENT_ID_LEN 20
#define SERVER_ID_LEN 20
#define PROTOCOL_PLAYER_NAME_LEN 30

typedef struct {
    uint8_t msg_type;
    uint8_t sender_id;
    uint8_t target_id;
} msg_header_t;

typedef struct {
    char client_id[CLIENT_ID_LEN];
    char player_name[PROTOCOL_PLAYER_NAME_LEN];
} msg_hello_t;

typedef struct {
    char server_id[SERVER_ID_LEN];
    uint8_t game_status;
    uint8_t other_players_count;
} msg_welcome_t;

int send_all(int fd, const void* data, size_t size);
int recv_all(int fd, void* data, size_t size);

int send_header(int fd, uint8_t msg_type, uint8_t sender_id, uint8_t target_id);
int recv_header(int fd, msg_header_t* header);

int send_hello(int fd, const char* client_id, const char* player_name);
int recv_hello_payload(int fd, msg_hello_t* hello);

int send_welcome(int fd, uint8_t player_id, uint8_t game_status);
int recv_welcome_payload(int fd, msg_welcome_t* welcome);

#endif