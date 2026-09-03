#include "game.h"
#include "ai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

Room g_rooms[MAX_ROOMS];
int g_room_count = 0;
int g_next_room_id = 1;

static void safe_copy(char *dst, size_t size, const char *src)
{
    if (!dst || size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    snprintf(dst, size, "%s", src);
}

static int starts_with(const char *s, const char *prefix)
{
    if (!s || !prefix || !*prefix) return 0;
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int count_terminal_punct(const char *s)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (p && *p) {
        if (*p == '!' || *p == '?' || *p == ';') {
            n++;
            p++;
            continue;
        }
        /* UTF-8: 。 = E3 80 82 */
        if (p[0] == 0xE3 && p[1] == 0x80 && p[2] == 0x82) {
            n++;
            p += 3;
            continue;
        }
        /* UTF-8: ！ = EF BC 81 */
        if (p[0] == 0xEF && p[1] == 0xBC && p[2] == 0x81) {
            n++;
            p += 3;
            continue;
        }
        /* UTF-8: ？ = EF BC 9F */
        if (p[0] == 0xEF && p[1] == 0xBC && p[2] == 0x9F) {
            n++;
            p += 3;
            continue;
        }
        /* UTF-8: ； = EF BC 9B */
        if (p[0] == 0xEF && p[1] == 0xBC && p[2] == 0x9B) {
            n++;
            p += 3;
            continue;
        }
        p++;
    }
    return n;
}

static int contains_conjunction(const char *s)
{
    static const char *words[] = {
        "但是", "可是", "然而", "而且", "并且", "因为", "所以", "如果", "那么",
        "虽然", "于是", "因此", "既然", "只要", "只有", "无论", "不管", "不仅",
        "不但", "还", "却", "但"
    };
    size_t i;
    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        /* "但" is deliberately checked separately to avoid matching 但是 twice. */
        if (strstr(s, words[i])) return 1;
    }
    return 0;
}

static int contains_kill_marker(const char *s)
{
    static const char *words[] = {
        "杀死", "击杀", "射杀", "斩首", "毒死", "勒死", "打死", "砍死", "捅死",
        "炸死", "致命", "重伤", "濒死", "处决", "消灭"
    };
    size_t i;
    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (strstr(s, words[i])) return 1;
    }
    return 0;
}

static int contains_rescue_marker(const char *s)
{
    static const char *words[] = {
        "治好", "恢复", "躲开", "挡住", "化解", "逃脱", "避开", "防御", "反击",
        "没死", "活下来", "挣脱", "闪避", "格挡"
    };
    size_t i;
    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (strstr(s, words[i])) return 1;
    }
    return 0;
}
static int contains_home_exit(const char *s)
{
    static const char *words[] = {
        "走出家门", "走出家", "离开家", "出了家门", "推开门", "出门", "离家"
    };
    size_t i;
    if (!s) return 0;
    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (strstr(s, words[i])) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Random determination model                                          */
/*                                                                     */
/* success_chance = BASE                                               */
/*   + (3 - difficulty)   * W_DIFFICULTY                               */
/*   + (plausibility - 3) * W_PLAUSIBLE                                */
/*   + (preparation - 3)  * W_PREPARED                                 */
/*   + operation weight                                                */
/*   + luck compensation (previous failure)                            */
/* clamped to [ROLL_MIN, ROLL_MAX]; d100 <= chance means success.      */
/* ------------------------------------------------------------------ */
#define ROLL_BASE        50
#define ROLL_W_DIFF      12
#define ROLL_W_PLAUSIBLE  8
#define ROLL_W_PREPARED   6
#define ROLL_LUCK_BONUS   8
#define ROLL_MIN          5
#define ROLL_MAX         95

static int operation_roll_weight(int operation)
{
    switch (operation) {
        case OP_CREATE:  return -5;   /* creating something new is harder */
        case OP_TWIST:   return -8;   /* twisting others' events is hardest */
        case OP_EXPLAIN: return  5;   /* explaining/completing is easier */
        default:         return  0;
    }
}

static int compute_roll_chance(int difficulty, int plausibility, int preparation,
                               int operation, int luck_bonus)
{
    int chance = ROLL_BASE;
    chance += (3 - difficulty) * ROLL_W_DIFF;
    chance += (plausibility - 3) * ROLL_W_PLAUSIBLE;
    chance += (preparation - 3) * ROLL_W_PREPARED;
    chance += operation_roll_weight(operation);
    chance += luck_bonus;
    if (chance < ROLL_MIN) chance = ROLL_MIN;
    if (chance > ROLL_MAX) chance = ROLL_MAX;
    return chance;
}

static int roll_d100(void)
{
    return rand() % 100 + 1;
}

/* Heuristic fallback: does this narration look uncertain enough to need a roll? */
static int heuristic_need_roll(const char *s, int operation)
{
    static const char *words[] = {
        "偷袭", "命中", "射", "刺", "砍", "扑向", "抢", "夺", "逃", "躲", "翻越",
        "撬", "砸", "点燃", "爆炸", "说服", "骗", "潜入", "破坏", "制造", "组装"
    };
    size_t i;
    if (!s) return 0;
    if (operation == OP_TWIST) return 1;
    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (strstr(s, words[i])) return 1;
    }
    return 0;
}

static void resolve_victim_warnings(Room *room, int victim_id)
{
    int i;
    for (i = 0; i < room->warning_count; i++) {
        if (room->warnings[i].victim_id == victim_id && !room->warnings[i].resolved) {
            room->warnings[i].resolved = 1;
        }
    }
}

static int player_has_unresolved_warning(Room *room, int player_id)
{
    int i;
    for (i = 0; i < room->warning_count; i++) {
        if (room->warnings[i].victim_id == player_id && !room->warnings[i].resolved) {
            return 1;
        }
    }
    return 0;
}

static void apply_limit_state(Room *room, const char *content, const char *limit_keyword, int operation)
{
    if (room->limit.remaining > 0) {
        int accepted = 0;
        if (room->limit.keyword[0] && starts_with(content, room->limit.keyword)) {
            accepted = 1;
        }
        if (accepted) {
            if (room->limit.force_streak == 0) {
                /* First sentence accepted the limit: the following two must accept it too. */
                room->limit.remaining = 2;
                room->limit.force_streak = 1;
            } else {
                if (room->limit.remaining > 0) room->limit.remaining--;
                room->limit.force_streak++;
                if (room->limit.remaining == 0) {
                    room->limit.keyword[0] = '\0';
                }
            }
        } else {
            /* Not starting with the keyword is treated as a reasonable escape. */
            room->limit.remaining = 0;
            room->limit.force_streak = 0;
            room->limit.keyword[0] = '\0';
        }
    } else if (operation == OP_LIMIT) {
        safe_copy(room->limit.keyword, sizeof(room->limit.keyword),
                  limit_keyword && *limit_keyword ? limit_keyword : "可是");
        room->limit.remaining = 1;
        room->limit.force_streak = 0;
    }
}

static int player_by_id(Room *room, int id);
static void advance_one(Room *room);
static void advance_to_next_alive(Room *room);
static void ai_speak_current(Room *room);
static int process_one_pending_ai_action(Room *room)
{
    if (!room || room->status != ROOM_PLAYING || room->player_count == 0) return 0;
    if (!room->players[room->order[room->turn_index]].alive) {
        advance_to_next_alive(room);
        return 1;
    }
    if (room->players[room->order[room->turn_index]].is_ai) {
        ai_speak_current(room);
        advance_to_next_alive(room);
        return 1;
    }
    return 0;
}

static void ai_speak_current(Room *room)
{
    static const char *lines[] = {
        "我环顾四周，保持警惕。",
        "我悄悄移动到掩体后面。",
        "我观察着每个人的动作。",
        "我握紧武器，准备应对突发情况。",
        "我保持沉默，等待时机。",
        "我检查了一下周围的痕迹。",
        "我缓缓后退，拉开距离。",
        "我在角落里布下了一个小陷阱。",
        "我翻找身边的口袋，找到了一根绳子。",
        "我抬头观察天花板，寻找可以利用的结构。",
        "我开始在沙地上画图，假装在计算什么。",
        "我把桌上的杯子慢慢推向桌边。",
        "我假装看向窗外，实际上留意着每个人的反应。",
        "我拆下了一截金属管，握在手里。",
        "我蹲下身系鞋带，趁机记住每个人的站位。",
        "我低声吹着口哨，慢慢靠近门口。",
        "我捡起一片碎玻璃，反射着光观察众人。",
        "我靠着墙坐下，看起来很放松。",
        "我故意弄出一点声响，试探周围的反应。",
        "我在心里默数着每个人的呼吸节奏。"
    };
    Player *p;
    Narrative *n;
    char content[MAX_CONTENT];
    char players_text[512] = "";
    char recent_text[1024] = "";
    char constraint[256] = "";
    char ai_text[512] = "";
    int op = OP_NORMAL;
    int i;
    int start;
    int content_ok = 0;
    int attempt;

    if (!room || room->status != ROOM_PLAYING || room->player_count == 0) return;
    if (room->turn_index < 0 || room->turn_index >= room->player_count) return;
    p = &room->players[room->order[room->turn_index]];
    if (!p->is_ai) return;
    if (room->narrative_count >= MAX_NARRATIVES) return;

    for (i = 0; i < room->player_count; i++) {
        if (i) strncat(players_text, ", ", sizeof(players_text) - strlen(players_text) - 1);
        strncat(players_text, room->players[i].name,
                sizeof(players_text) - strlen(players_text) - 1);
    }

    start = room->narrative_count > 3 ? room->narrative_count - 3 : 0;
    for (i = start; i < room->narrative_count; i++) {
        char line[600];
        Player *np = &room->players[0];
        int pi = player_by_id(room, room->narratives[i].player_id);
        if (pi >= 0) np = &room->players[pi];
        snprintf(line, sizeof(line), "%s：%s\n", np->name, room->narratives[i].content);
        strncat(recent_text, line, sizeof(recent_text) - strlen(recent_text) - 1);
    }

    if (player_has_unresolved_warning(room, p->id)) {
        snprintf(constraint, sizeof(constraint), "你正处于濒死状态，请描述如何化解危机。");
    } else if (room->limit.remaining > 0 && room->limit.keyword[0]) {
        snprintf(constraint, sizeof(constraint),
                 "当前受限定词“%s”约束，你可以合理接受或尝试合理摆脱。", room->limit.keyword);
    }
if (!p->has_spoken_first) {
        if (constraint[0]) strncat(constraint, "；", sizeof(constraint) - strlen(constraint) - 1);
        strncat(constraint, "这是你的第一句话，请描述从家里出发的动作，例如“我走出了家门”。",
                sizeof(constraint) - strlen(constraint) - 1);
    }

    /* Choose an operation mode for the AI player. */
    if (!p->has_spoken_first) {
        op = OP_NORMAL;
    } else if (player_has_unresolved_warning(room, p->id)) {
        op = OP_EXPLAIN;
    } else if (room->limit.remaining > 0) {
        op = OP_EXPLAIN;
    } else {
        int r = rand() % 10;
        if (r < 4) op = OP_NORMAL;
        else if (r < 7) op = OP_CREATE;
        else if (r < 9) op = OP_TWIST;
        else op = OP_EXPLAIN;
    }

    if (constraint[0]) strncat(constraint, "；", sizeof(constraint) - strlen(constraint) - 1);
    strncat(constraint, "操作类型：", sizeof(constraint) - strlen(constraint) - 1);
    strncat(constraint, game_op_name(op), sizeof(constraint) - strlen(constraint) - 1);

    if (ai_is_configured()) {
        /* AI speech must pass review; otherwise regenerate, not fallback to template. */
        for (attempt = 0; attempt < 2 && !content_ok; attempt++) {
            int valid_reason = 1;
            int valid_predicate = 1;
            int valid_mode = 1;
            char vreason[256] = "";

            if (ai_try_generate_narration(room->name, players_text, p->name,
                                          recent_text, constraint, ai_text,
                                          sizeof(ai_text)) != 1 || !ai_text[0]) {
                continue;
            }
            snprintf(content, sizeof(content), "%s", ai_text);
            if (ai_validate_narration(room->name, players_text, p->name, content, op,
                                      !p->has_spoken_first,
                                      &valid_reason, &valid_predicate, &valid_mode,
                                      vreason, sizeof(vreason)) == 1 &&
                (!valid_reason || !valid_predicate || !valid_mode)) {
                continue;
            }
            content_ok = 1;
        }
    } else {
        if (ai_try_generate_narration(room->name, players_text, p->name,
                                      recent_text, constraint, ai_text,
                                      sizeof(ai_text)) == 1 && ai_text[0]) {
            snprintf(content, sizeof(content), "%s", ai_text);
            content_ok = 1;
        }
    }    if (!content_ok) {
        if (ai_is_configured()) {
            if (!p->has_spoken_first) snprintf(content, sizeof(content), "我走出了家门。");
            else snprintf(content, sizeof(content), "我保持沉默。");
        } else if (!p->has_spoken_first) {
            snprintf(content, sizeof(content), "我走出了家门。");
        } else if (player_has_unresolved_warning(room, p->id)) {
            snprintf(content, sizeof(content), "我躲开了这次攻击。");
        } else if (room->limit.remaining > 0 && room->limit.keyword[0]) {
            snprintf(content, sizeof(content), "%s我继续等待时机。", room->limit.keyword);
        } else {
            snprintf(content, sizeof(content), "%s", lines[rand() % (sizeof(lines) / sizeof(lines[0]))]);
        }
    }

    apply_limit_state(room, content, "", op);

    n = &room->narratives[room->narrative_count];
    memset(n, 0, sizeof(*n));
    n->id = room->narrative_count + 1;
    n->room_id = room->id;
    n->player_id = p->id;
    n->round = room->round;
    n->operation = op;
    safe_copy(n->content, sizeof(n->content), content);
    safe_copy(n->limit_keyword, sizeof(n->limit_keyword),
              room->limit.keyword[0] ? room->limit.keyword : "");
    n->target_id = 0;
    room->narrative_count++;
    p->has_spoken_first = 1;

    if (player_has_unresolved_warning(room, p->id)) {
        int rescued = 0;
        int ai_ok = 0;
        char ai_reason[256] = "";
        int wi;

        if (ai_is_configured()) {
            char warning_text[512] = "";
            for (wi = 0; wi < room->warning_count; wi++) {
                if (room->warnings[wi].victim_id == p->id && !room->warnings[wi].resolved) {
                    if (warning_text[0]) strncat(warning_text, "；", sizeof(warning_text) - strlen(warning_text) - 1);
                    strncat(warning_text, room->warnings[wi].reason,
                            sizeof(warning_text) - strlen(warning_text) - 1);
                }
            }
            ai_ok = ai_try_judge_rescue(room->name, players_text, p->name,
                                        content, warning_text,
                                        &rescued, ai_reason, sizeof(ai_reason));
        }

        if (ai_ok == 1 ? rescued : contains_rescue_marker(content)) {
            resolve_victim_warnings(room, p->id);
        }
    }
}

static void generate_token(char *out, size_t size)
{
    static const char alphabet[] = "0123456789abcdef";
    int i;
    if (!out || size == 0) return;
    for (i = 0; i < (int)size - 1; i++) {
        out[i] = alphabet[rand() % 16];
    }
    out[size - 1] = '\0';
}

static int player_name_used(Room *room, const char *name)
{
    int i;
    for (i = 0; i < room->player_count; i++) {
        if (strcmp(room->players[i].name, name) == 0) return 1;
    }
    return 0;
}

static int player_by_id(Room *room, int id)
{
    int i;
    for (i = 0; i < room->player_count; i++) {
        if (room->players[i].id == id) return i;
    }
    return -1;
}

static int next_player_id(Room *room)
{
    int max = 0;
    int i;
    for (i = 0; i < room->player_count; i++) {
        if (room->players[i].id > max) max = room->players[i].id;
    }
    return max + 1;
}

void game_init(void)
{
    memset(g_rooms, 0, sizeof(g_rooms));
    g_room_count = 0;
    g_next_room_id = 1;
    srand((unsigned)time(NULL));
}

Room *game_find_room(int room_id)
{
    int i;
    for (i = 0; i < g_room_count; i++) {
        if (g_rooms[i].id == room_id) return &g_rooms[i];
    }
    return NULL;
}

Player *game_find_player(Room *room, const char *token)
{
    int i;
    if (!room || !token) return NULL;
    for (i = 0; i < room->player_count; i++) {
        if (strcmp(room->players[i].token, token) == 0) return &room->players[i];
    }
    return NULL;
}

int game_create_room(const char *room_name, int mode, const char *player_name, char *token_out)
{
    Room *room;
    int idx;

    if (g_room_count >= MAX_ROOMS) return -1;
    idx = g_room_count++;

    room = &g_rooms[idx];
    memset(room, 0, sizeof(*room));
    room->id = g_next_room_id++;
    room->mode = mode ? MODE_GM : MODE_AUTO;
    room->status = ROOM_WAITING;
    safe_copy(room->name, sizeof(room->name), room_name && *room_name ? room_name : "乱杀房间");

    room->players[0].id = 1;
    safe_copy(room->players[0].name, sizeof(room->players[0].name),
              player_name && *player_name ? player_name : "玩家1");
    generate_token(room->players[0].token, sizeof(room->players[0].token));
    room->players[0].alive = 1;
    room->players[0].ready = 0;
    room->players[0].is_ai = 0;
    room->player_count = 1;
    room->owner_id = 1;
    room->round = 0;
    room->winner_id = 0;

    if (token_out) safe_copy(token_out, TOKEN_LEN + 1, room->players[0].token);
    return room->id;
}

int game_join_room(int room_id, const char *player_name, char *token_out)
{
    Room *room = game_find_room(room_id);
    Player *p;
    int idx;

    if (!room) return -1;
    if (room->status != ROOM_WAITING) return -2;
    if (room->player_count >= MAX_PLAYERS) return -3;
    if (!player_name || !*player_name) return -4;
    if (player_name_used(room, player_name)) return -5;

    idx = room->player_count++;
    p = &room->players[idx];
    memset(p, 0, sizeof(*p));
    p->id = next_player_id(room);
    safe_copy(p->name, sizeof(p->name), player_name);
    generate_token(p->token, sizeof(p->token));
    p->alive = 1;
    p->ready = 0;
    p->is_ai = 0;

    if (token_out) safe_copy(token_out, TOKEN_LEN + 1, p->token);
    return room->id;
}

int game_add_ai(int room_id)
{
    Room *room = game_find_room(room_id);
    Player *p;
    char name[64];
    int idx, ai_no = 1;

    if (!room) return -1;
    if (room->status != ROOM_WAITING) return -2;
    if (room->player_count >= MAX_PLAYERS) return -3;

    /* Find next AI number */
    while (1) {
        snprintf(name, sizeof(name), "AI-%d", ai_no);
        if (!player_name_used(room, name)) break;
        ai_no++;
    }

    idx = room->player_count++;
    p = &room->players[idx];
    memset(p, 0, sizeof(*p));
    p->id = next_player_id(room);
    safe_copy(p->name, sizeof(p->name), name);
    generate_token(p->token, sizeof(p->token));
    p->alive = 1;
    p->ready = 1;
    p->is_ai = 1;

    return room->id;
}

int game_set_ready(int room_id, const char *token, int ready)
{
    Room *room = game_find_room(room_id);
    Player *p = room ? game_find_player(room, token) : NULL;
    if (!room || !p) return -1;
    if (room->status != ROOM_WAITING) return -2;
    p->ready = ready ? 1 : 0;
    return 0;
}

static void advance_one(Room *room)
{
    if (room->player_count == 0) return;
    room->turn_index = (room->turn_index + 1) % room->player_count;
    if (room->turn_index == 0) room->round++;
}
static void advance_to_next_alive(Room *room)
{
    int guard = 0;
    if (room->player_count == 0) return;
    advance_one(room);
    while (room->players[room->order[room->turn_index]].alive == 0 &&
           guard++ < room->player_count) {
        advance_one(room);
    }
}

int game_start(int room_id, const char *token, int fill_ai)
{
    Room *room = game_find_room(room_id);
    Player *p = room ? game_find_player(room, token) : NULL;
    int i, j;

    if (!room || !p) return -1;
    if (room->status != ROOM_WAITING) return -2;
    if (p->id != room->owner_id) return -3;

    if (fill_ai) {
        while (room->player_count < 4) {
            int r = game_add_ai(room_id);
            if (r < 0) break;
        }
    }

    if (room->player_count < 4) return -4;
    if (room->player_count > MAX_PLAYERS) return -5;

    /* Ensure all humans ready (AI already ready) */
    for (i = 0; i < room->player_count; i++) {
        if (!room->players[i].is_ai && !room->players[i].ready) return -6;
    }

    /* Simple deterministic-ish shuffle */
    for (i = 0; i < room->player_count; i++) room->order[i] = i;
    for (i = room->player_count - 1; i > 0; i--) {
        j = rand() % (i + 1);
        {
            int t = room->order[i];
            room->order[i] = room->order[j];
            room->order[j] = t;
        }
    }

    room->status = ROOM_PLAYING;
    room->turn_index = 0;
    room->round = 1;
    room->winner_id = 0;
    room->limit.keyword[0] = '\0';
    room->limit.remaining = 0;
    room->limit.force_streak = 0;
    room->pending_judgment_count = 0;

    /* AI players will be processed lazily on state polls, so start stays fast. */
    return 0;
}

int game_speak(int room_id, const char *token, int operation, const char *content,
               const char *limit_keyword, int target_id,
               char *error_msg, size_t error_size)
{
    Room *room = game_find_room(room_id);
    Player *p = room ? game_find_player(room, token) : NULL;
    Player *cp;
    Narrative *n;
    int roll_used = 0;
    int roll_value = 0;
    int roll_chance = 0;
    int roll_success = 1;
    char roll_note[128] = "";

    if (error_msg && error_size > 0) error_msg[0] = '\0';

    if (!room || !p) return -1;
    if (room->status != ROOM_PLAYING) return -2;
    if (!p->alive) {
        if (error_msg && error_size > 0) snprintf(error_msg, error_size, "死亡玩家不能再发言");
        return -12;
    }
    if (room->player_count == 0 || room->turn_index < 0 || room->turn_index >= room->player_count) return -3;
    cp = &room->players[room->order[room->turn_index]];
    if (cp->id != p->id) return -4;
    if (!content || !*content) return -5;
    if (strlen(content) >= MAX_CONTENT) return -6;

    /* Basic automatic checks */
    if (count_terminal_punct(content) > 1) return -8;
    {
        int allowed_conjunction_in_limit =
            room->limit.remaining > 0 &&
            room->limit.keyword[0] &&
            starts_with(content, room->limit.keyword);
        if (contains_conjunction(content) && operation != OP_LIMIT && !allowed_conjunction_in_limit) {
            return -9;
        }
    }
/* AI strengthened review: reasonableness + single predicate. */
    if (ai_is_configured()) {
        char players_text[512] = "";
        char ai_reason[256] = "";
        int valid_reason = 1;
        int valid_predicate = 1;
        int valid_mode = 1;
        int i;

        for (i = 0; i < room->player_count; i++) {
            if (i) strncat(players_text, ", ", sizeof(players_text) - strlen(players_text) - 1);
            strncat(players_text, room->players[i].name,
                    sizeof(players_text) - strlen(players_text) - 1);
        }

        if (ai_validate_narration(room->name, players_text, p->name, content, operation,
                                  !p->has_spoken_first,
                                  &valid_reason, &valid_predicate, &valid_mode,
                                  ai_reason, sizeof(ai_reason)) == 1) {
            if (!valid_reason || !valid_predicate || !valid_mode) {
                if (error_msg && error_size > 0) {
                    snprintf(error_msg, error_size, "%s",
                             ai_reason[0] ? ai_reason : "发言不合理或不符合单一谓语规则");
                }
                return -11;
            }
        }
        /* 首句离家设定：无论 AI 是否放行，第一句必须包含离家动作。 */
        if (!p->has_spoken_first && !contains_home_exit(content)) {
            if (error_msg && error_size > 0) {
                snprintf(error_msg, error_size, "第一句话应该类似“我走出了家门”");
            }
            return -11;
        }
    } else if (!p->has_spoken_first && !contains_home_exit(content)) {
        if (error_msg && error_size > 0) {
            snprintf(error_msg, error_size, "第一句话应该类似“我走出了家门”");
        }
        return -11;
    }

    /* Random determination: reviewer decides whether a dice check is needed. */
    {
        int need_roll = 0;
        int difficulty = 3;
        int plausibility = 3;
        int preparation = 3;
        char rc_reason[256] = "";
        char players_text2[512] = "";
        char recent_text2[1024] = "";
        int k;
        int start2;

        for (k = 0; k < room->player_count; k++) {
            if (k) strncat(players_text2, ", ", sizeof(players_text2) - strlen(players_text2) - 1);
            strncat(players_text2, room->players[k].name,
                    sizeof(players_text2) - strlen(players_text2) - 1);
        }
        start2 = room->narrative_count > 3 ? room->narrative_count - 3 : 0;
        for (k = start2; k < room->narrative_count; k++) {
            char line[600];
            int pi2 = player_by_id(room, room->narratives[k].player_id);
            snprintf(line, sizeof(line), "%s：%s\n",
                     pi2 >= 0 ? room->players[pi2].name : "系统",
                     room->narratives[k].content);
            strncat(recent_text2, line, sizeof(recent_text2) - strlen(recent_text2) - 1);
        }

        if (!p->has_spoken_first) {
            /* Traditional opening sentence always succeeds. */
            need_roll = 0;
        } else if (ai_is_configured()) {
            if (ai_judge_random_check(room->name, players_text2, p->name, content, operation,
                                      recent_text2, &need_roll, &difficulty, &plausibility,
                                      &preparation, rc_reason, sizeof(rc_reason)) != 1) {
                need_roll = heuristic_need_roll(content, operation);
            }
        } else {
            need_roll = heuristic_need_roll(content, operation);
        }

        if (need_roll) {
            int luck = p->last_roll_failed ? ROLL_LUCK_BONUS : 0;
            roll_chance = compute_roll_chance(difficulty, plausibility, preparation,
                                              operation, luck);
            roll_value = roll_d100();
            roll_success = roll_value <= roll_chance ? 1 : 0;
            roll_used = 1;
            p->last_roll_failed = roll_success ? 0 : 1;
            snprintf(roll_note, sizeof(roll_note),
                     "判定 %d/%d %s（难度%d 合理%d 铺垫%d）",
                     roll_value, roll_chance, roll_success ? "成功" : "失败",
                     difficulty, plausibility, preparation);
        }
    }

    /* Limit state machine */
    apply_limit_state(room, content, limit_keyword, operation);

    /* Record narrative */
    if (room->narrative_count >= MAX_NARRATIVES) return -10;
    n = &room->narratives[room->narrative_count];
    memset(n, 0, sizeof(*n));
    n->id = room->narrative_count + 1;
    n->room_id = room->id;
    n->player_id = p->id;
    n->round = room->round;
    n->operation = operation;
    safe_copy(n->content, sizeof(n->content), content);
    safe_copy(n->limit_keyword, sizeof(n->limit_keyword),
              room->limit.keyword[0] ? room->limit.keyword : (limit_keyword ? limit_keyword : ""));
    n->target_id = target_id;
    n->roll_used = roll_used;
    n->roll_value = roll_value;
    n->roll_chance = roll_chance;
    n->roll_success = roll_success;
    safe_copy(n->roll_note, sizeof(n->roll_note), roll_note);
    room->narrative_count++;
    p->has_spoken_first = 1;

    /* A failed random check means the attempted event does not take effect. */
    if (roll_used && !roll_success) {
        advance_to_next_alive(room);
        return 0;
    }

    /* Near-death warning:
   - If AI is configured, queue the judgment and let state polling process it.
   - Otherwise apply the fast keyword heuristic immediately. */
    {
        int warn_target_id = target_id;
        int i;

        /* If no explicit target, try to infer one from the narration text. */
        if (warn_target_id <= 0) {
            for (i = 0; i < room->player_count; i++) {
                if (room->players[i].id != p->id && strstr(content, room->players[i].name)) {
                    warn_target_id = room->players[i].id;
                    break;
                }
            }
        }

        if (ai_is_configured() && (operation == OP_TWIST || operation == OP_CREATE ||
                                   warn_target_id > 0 || contains_kill_marker(content))) {
            char players_text[512] = "";
            char ai_reason[256] = "";
            const char *target_name = "";
            int ai_danger = 0;
            int ai_ok = 0;
            int heuristic_danger = 0;
            int should_warn = 0;
            int ti;

            for (i = 0; i < room->player_count; i++) {
                if (i) strncat(players_text, ", ", sizeof(players_text) - strlen(players_text) - 1);
                strncat(players_text, room->players[i].name,
                        sizeof(players_text) - strlen(players_text) - 1);
            }
            if (warn_target_id > 0 && (ti = player_by_id(room, warn_target_id)) >= 0) {
                target_name = room->players[ti].name;
            }
            heuristic_danger = operation == OP_TWIST && warn_target_id > 0 &&
                               player_by_id(room, warn_target_id) >= 0 &&
                               contains_kill_marker(content);
            ai_ok = ai_try_judge_narration(room->name, players_text, p->name,
                                           content, operation, target_name,
                                           &ai_danger, ai_reason, sizeof(ai_reason));
            should_warn = ai_ok == 1 ? ai_danger : heuristic_danger;
            if (should_warn && warn_target_id > 0 && room->warning_count < MAX_WARNINGS) {
                Warning *w = &room->warnings[room->warning_count];
                memset(w, 0, sizeof(*w));
                w->id = room->warning_count + 1;
                w->victim_id = warn_target_id;
                w->source_id = p->id;
                w->narrative_id = n->id;
                w->resolved = 0;
                if (ai_ok == 1 && ai_reason[0]) {
                    snprintf(w->reason, sizeof(w->reason), "%s", ai_reason);
                } else {
                    snprintf(w->reason, sizeof(w->reason), "%s", content);
                }
                room->warning_count++;
            }
        } else {
            int heuristic_danger = operation == OP_TWIST && warn_target_id > 0 &&
                                   player_by_id(room, warn_target_id) >= 0 &&
                                   contains_kill_marker(content);
            if (heuristic_danger && warn_target_id > 0 && room->warning_count < MAX_WARNINGS) {
                Warning *w = &room->warnings[room->warning_count];
                memset(w, 0, sizeof(*w));
                w->id = room->warning_count + 1;
                w->victim_id = warn_target_id;
                w->source_id = p->id;
                w->narrative_id = n->id;
                w->resolved = 0;
                snprintf(w->reason, sizeof(w->reason), "%s", content);
                room->warning_count++;
            }
        }
    }

    /* Simple healing/escaping text resolves this player's own near-death warnings. */
    /* Near-death warning resolution: AI review, fallback heuristic. */
    if (player_has_unresolved_warning(room, p->id)) {
        int rescued = 0;
        int ai_ok = 0;
        char ai_reason[256] = "";
        char players_text[512] = "";
        int i;

        for (i = 0; i < room->player_count; i++) {
            if (i) strncat(players_text, ", ", sizeof(players_text) - strlen(players_text) - 1);
            strncat(players_text, room->players[i].name,
                    sizeof(players_text) - strlen(players_text) - 1);
        }

        if (ai_is_configured()) {
            char warning_text[512] = "";
            for (i = 0; i < room->warning_count; i++) {
                if (room->warnings[i].victim_id == p->id && !room->warnings[i].resolved) {
                    if (warning_text[0]) strncat(warning_text, "；", sizeof(warning_text) - strlen(warning_text) - 1);
                    strncat(warning_text, room->warnings[i].reason,
                            sizeof(warning_text) - strlen(warning_text) - 1);
                }
            }
            ai_ok = ai_try_judge_rescue(room->name, players_text, p->name,
                                        content, warning_text,
                                        &rescued, ai_reason, sizeof(ai_reason));
        }

        if (ai_ok == 1 ? rescued : contains_rescue_marker(content)) {
            resolve_victim_warnings(room, p->id);
        }
    }

    /* Advance to the next living player. AI turns are processed lazily on state polls. */
    advance_to_next_alive(room);

    return 0;
}

static void finish_if_one_alive(Room *room)
{
    int alive_count = 0;
    int last = 0;
    int i;
    for (i = 0; i < room->player_count; i++) {
        if (room->players[i].alive) {
            alive_count++;
            last = room->players[i].id;
        }
    }
    if (alive_count <= 1 && room->status == ROOM_PLAYING) {
        room->status = ROOM_ENDED;
        room->winner_id = alive_count == 1 ? last : 0;
    }
}

static void announce_death(Room *room, int victim_id)
{
    int vi = player_by_id(room, victim_id);
    Narrative *n;
    if (room->narrative_count >= MAX_NARRATIVES) return;
    resolve_victim_warnings(room, victim_id);
    n = &room->narratives[room->narrative_count];
    memset(n, 0, sizeof(*n));
    n->id = room->narrative_count + 1;
    n->room_id = room->id;
    n->player_id = 0;
    n->round = room->round;
    n->operation = OP_DEATH;
    if (vi >= 0) {
        char vname[MAX_NAME];
        safe_copy(vname, sizeof(vname), room->players[vi].name);
        snprintf(n->content, sizeof(n->content), "玩家%s 死亡宣告", vname);
    } else {
        snprintf(n->content, sizeof(n->content), "死亡宣告");
    }
    n->target_id = 0;
    room->narrative_count++;
}

int game_declare_death(int room_id, const char *token, int target_id)
{
    Room *room = game_find_room(room_id);
    Player *p = room ? game_find_player(room, token) : NULL;
    int ti;
    int i;
    int found_unresolved = 0;

    if (!room || !p) return -1;
    if (room->status != ROOM_PLAYING) return -2;

    if (target_id == p->id) {
        if (!p->alive) return -7;
        p->alive = 0;
        announce_death(room, p->id);
        finish_if_one_alive(room);
        if (room->status == ROOM_PLAYING) advance_to_next_alive(room);
        return 0;
    }

    /* Damage source can only declare on its own turn. */
    if (room->player_count == 0 ||
        room->players[room->order[room->turn_index]].id != p->id) {
        return -3;
    }

    ti = player_by_id(room, target_id);
    if (ti < 0) return -4;

    for (i = 0; i < room->warning_count; i++) {
        if (room->warnings[i].source_id == p->id &&
            room->warnings[i].victim_id == target_id &&
            !room->warnings[i].resolved) {
            found_unresolved = 1;
            break;
        }
    }
    if (!found_unresolved) return -5;
    if (!room->players[ti].alive) return -6;

    room->players[ti].alive = 0;
    announce_death(room, target_id);
    finish_if_one_alive(room);
    if (room->status == ROOM_PLAYING) advance_to_next_alive(room);
    return 0;
}

int game_gm_command(int room_id, const char *token, const char *action, int target_id)
{
    Room *room = game_find_room(room_id);
    Player *p = room ? game_find_player(room, token) : NULL;
    int ti;

    if (!room || !p) return -1;
    if (room->mode != MODE_GM) return -2;
    if (p->id != room->owner_id) return -3;

    ti = player_by_id(room, target_id);
    if (ti < 0) return -4;

    if (strcmp(action, "force_death") == 0) {
        if (!room->players[ti].alive) return -5;
        room->players[ti].alive = 0;
        announce_death(room, target_id);
        finish_if_one_alive(room);
        return 0;
    }
    if (strcmp(action, "revive") == 0) {
        if (room->players[ti].alive) return -5;
        room->players[ti].alive = 1;
        return 0;
    }
    if (strcmp(action, "resolve_warnings") == 0) {
        resolve_victim_warnings(room, target_id);
        return 0;
    }
    if (strcmp(action, "set_turn") == 0) {
        int i;
        for (i = 0; i < room->player_count; i++) {
            if (room->order[i] == ti) {
                room->turn_index = i;
                return 0;
            }
        }
        return -5;
    }

    return -6;
}

const char *game_op_name(int op)
{
    switch (op) {
        case OP_CREATE: return "创造";
        case OP_TWIST: return "扭曲";
        case OP_EXPLAIN: return "解释/补充";
        case OP_LIMIT: return "限定";
        case OP_DEATH: return "死亡宣告";
        default: return "陈述";
    }
}

int game_room_to_json_ex(int room_id, const char *token, JsonBuf *out, int process_ai)
{
    Room *room = game_find_room(room_id);
    int i;

    if (!room) return -1;
    if (process_ai) {
        /* Process at most one pending AI turn or one AI danger judgment per poll. */
        process_one_pending_ai_action(room);
    }

    jsonb_append(out, "{");
    jsonb_appendf(out, "\"ok\":true,");
    jsonb_appendf(out, "\"room_id\":%d,", room->id);
    jsonb_append(out, "\"name\":");
    jsonb_string(out, room->name);
    jsonb_append(out, ",");
    jsonb_appendf(out, "\"status\":%d,", room->status);
    jsonb_appendf(out, "\"mode\":%d,", room->mode);
    jsonb_appendf(out, "\"round\":%d,", room->round);
    jsonb_appendf(out, "\"turn_index\":%d,", room->turn_index);
    jsonb_appendf(out, "\"turn_player_id\":%d,", room->player_count > 0 ? room->players[room->order[room->turn_index]].id : 0);
    jsonb_appendf(out, "\"winner_id\":%d,", room->winner_id);
    jsonb_appendf(out, "\"owner_id\":%d,", room->owner_id);

    jsonb_append(out, "\"players\":[");
    for (i = 0; i < room->player_count; i++) {
        Player *pl = &room->players[i];
        if (i) jsonb_append(out, ",");
        jsonb_append(out, "{");
        jsonb_appendf(out, "\"id\":%d,", pl->id);
        jsonb_append(out, "\"name\":");
        jsonb_string(out, pl->name);
        jsonb_append(out, ",");
        jsonb_appendf(out, "\"alive\":%s,", pl->alive ? "true" : "false");
        jsonb_appendf(out, "\"ready\":%s,", pl->ready ? "true" : "false");
        jsonb_appendf(out, "\"is_ai\":%s,", pl->is_ai ? "true" : "false");
        jsonb_appendf(out, "\"is_me\":%s", token && strcmp(pl->token, token) == 0 ? "true" : "false");
        jsonb_append(out, "}");
    }
    jsonb_append(out, "],");

    /* Only expose relevant subset of narratives. */
    jsonb_append(out, "\"narratives\":[");
    {
        int start = room->narrative_count > 100 ? room->narrative_count - 100 : 0;
        for (i = start; i < room->narrative_count; i++) {
            Narrative *n = &room->narratives[i];
            int pi = player_by_id(room, n->player_id);
            if (i > start) jsonb_append(out, ",");
            jsonb_append(out, "{");
            jsonb_appendf(out, "\"id\":%d,", n->id);
            jsonb_appendf(out, "\"player_id\":%d,", n->player_id);
            jsonb_append(out, "\"player_name\":");
            if (pi >= 0) {
                jsonb_string(out, room->players[pi].name);
            } else {
                jsonb_string(out, "系统");
            }
            jsonb_append(out, ",");
            jsonb_appendf(out, "\"round\":%d,", n->round);
            jsonb_append(out, "\"operation\":");
            jsonb_string(out, game_op_name(n->operation));
            jsonb_append(out, ",");
            jsonb_append(out, "\"content\":");
            jsonb_string(out, n->content);
            jsonb_append(out, ",");
            jsonb_append(out, "\"limit_keyword\":");
            jsonb_string(out, n->limit_keyword);
            jsonb_appendf(out, ",\"target_id\":%d", n->target_id);
            jsonb_appendf(out, ",\"roll_used\":%s", n->roll_used ? "true" : "false");
            jsonb_appendf(out, ",\"roll_value\":%d", n->roll_value);
            jsonb_appendf(out, ",\"roll_chance\":%d", n->roll_chance);
            jsonb_appendf(out, ",\"roll_success\":%s", n->roll_success ? "true" : "false");
            jsonb_append(out, ",\"roll_note\":");
            jsonb_string(out, n->roll_note);
            jsonb_append(out, "}");
        }
    }
    jsonb_append(out, "],");

    jsonb_append(out, "\"warnings\":[");
    for (i = 0; i < room->warning_count; i++) {
        Warning *w = &room->warnings[i];
        if (i) jsonb_append(out, ",");
        jsonb_append(out, "{");
        jsonb_appendf(out, "\"id\":%d,", w->id);
        jsonb_appendf(out, "\"victim_id\":%d,", w->victim_id);
        jsonb_appendf(out, "\"source_id\":%d,", w->source_id);
        jsonb_appendf(out, "\"narrative_id\":%d,", w->narrative_id);
        jsonb_appendf(out, "\"resolved\":%s,", w->resolved ? "true" : "false");
        jsonb_append(out, "\"reason\":");
        jsonb_string(out, w->reason);
        jsonb_append(out, "}");
    }
    jsonb_append(out, "],");

    jsonb_append(out, "\"limit\":{");
    jsonb_append(out, "\"keyword\":");
    jsonb_string(out, room->limit.keyword);
    jsonb_appendf(out, ",\"remaining\":%d,\"force_streak\":%d", room->limit.remaining, room->limit.force_streak);
    jsonb_append(out, "}");

    jsonb_append(out, "}");
    return 0;
}
int game_room_to_json(int room_id, const char *token, JsonBuf *out)
{
    return game_room_to_json_ex(room_id, token, out, 1);
}