#ifndef GAME_H
#define GAME_H

#include "json.h"

#define MAX_ROOMS 64
#define MAX_PLAYERS 7
#define MAX_NAME 64
#define MAX_CONTENT 512
#define MAX_NARRATIVES 2048
#define MAX_WARNINGS 256
#define TOKEN_LEN 16

enum RoomMode { MODE_AUTO = 0, MODE_GM = 1 };
enum RoomStatus { ROOM_WAITING = 0, ROOM_PLAYING = 1, ROOM_ENDED = 2 };
enum Operation { OP_NORMAL = 0, OP_CREATE = 1, OP_TWIST = 2, OP_EXPLAIN = 3, OP_LIMIT = 4, OP_DEATH = 5 };

typedef struct Player {
    int id;
    char name[MAX_NAME];
    char token[TOKEN_LEN + 1];
    int alive;
    int ready;
    int is_ai;
    int has_spoken_first;   /* 0 = hasn't spoken yet → first sentence requires "离家" */
    int last_roll_failed;   /* used as a small luck compensation weight */
    int hp;                 /* current hit points */
    int max_hp;             /* maximum hit points */
    int numeric_status;     /* 0=applied, 1=adjusted; only final result exposed */
} Player;

typedef struct Narrative {
    int id;
    int room_id;
    int player_id;
    int round;
    int operation;
    char content[MAX_CONTENT];
    char limit_keyword[64];
    int target_id;
    int damage;         /* 0 = no damage, 1-3 = damage dealt */
    int roll_used;      /* 1 if a random check was performed */
    int roll_value;     /* 1..100 dice result */
    int roll_chance;    /* success threshold after weights */
    int roll_success;   /* 1 success, 0 failure */
    char roll_note[128];
} Narrative;

typedef struct Warning {
    int id;
    int victim_id;
    int source_id;
    int narrative_id;
    int resolved;
    char reason[256];
} Warning;

typedef struct PendingJudgment {
    int player_id;
    int operation;
    int target_id;
    char content[MAX_CONTENT];
    int processed;
} PendingJudgment;

typedef struct LimitState {
    char keyword[64];
    int remaining;            /* >0 means subsequent sentences must accept the limit */
    int force_streak;         /* how many failed escape attempts have happened */
} LimitState;

typedef struct Room {
    int id;
    char name[MAX_NAME];
    int mode;
    int status;
    Player players[MAX_PLAYERS];
    int player_count;
    int order[MAX_PLAYERS];
    int turn_index;
    int round;
    Narrative narratives[MAX_NARRATIVES];
    int narrative_count;
    Warning warnings[MAX_WARNINGS];
    int warning_count;
    PendingJudgment pending_judgments[MAX_NARRATIVES];
    int pending_judgment_count;
    LimitState limit;
    int winner_id;
    int owner_id;
} Room;

/* Global game state. */
extern Room g_rooms[MAX_ROOMS];
extern int g_room_count;
extern int g_next_room_id;

/* Room lifecycle */
int game_create_room(const char *room_name, int mode, const char *player_name, char *token_out);
int game_join_room(int room_id, const char *player_name, char *token_out);
int game_add_ai(int room_id);
int game_set_ready(int room_id, const char *token, int ready);
int game_start(int room_id, const char *token, int fill_ai);

/* Gameplay */
int game_speak(int room_id, const char *token, int operation, const char *content,
               const char *limit_keyword, int target_id,
               char *error_msg, size_t error_size);
int game_declare_death(int room_id, const char *token, int target_id);
int game_gm_command(int room_id, const char *token, const char *action, int target_id);

/* Lookup helpers */
Room *game_find_room(int room_id);
Player *game_find_player(Room *room, const char *token);
int game_room_to_json(int room_id, const char *token, JsonBuf *out);
int game_room_to_json_ex(int room_id, const char *token, JsonBuf *out, int process_ai);
const char *game_op_name(int op);

/* Reset for tests */
void game_init(void);

#endif /* GAME_H */