#include "game.h"
#include "ai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

#ifdef _WIN32
  #include <windows.h>
  #include <direct.h>
#else
  #include <sys/stat.h>
#endif

#define DEFAULT_MAX_HP 6

/* 灾祸值机制：不合理发言累积灾祸，达到阈值后每步有概率触发灾祸并清零。
   灾祸是可逆的——言之有物的叙述会消解灾祸，因此它是「可管理」的资源，
   而不是对活跃玩家的单向惩罚。 */
#define CALAMITY_THRESHOLD 5
#define CALAMITY_CHANCE_LOW   20   /* 灾祸值 5-9 */
#define CALAMITY_CHANCE_MED   40   /* 灾祸值 10-14 */
#define CALAMITY_CHANCE_HIGH  60   /* 灾祸值 >=15 */
#define CALAMITY_RELIEF        2   /* 一次高质量发言消解的灾祸值 */

/* 灾祸值的可见档位：0=平静，1=躁动，2=危险（前端只暴露档位，不暴露具体数值）。 */
#define CALAMITY_TIER_UNEASY   CALAMITY_THRESHOLD      /* >=5 躁动 */
#define CALAMITY_TIER_DANGER   10                      /* >=10 危险 */

Room g_rooms[MAX_ROOMS];
int g_room_count = 0;
int g_next_room_id = 1;

/* 随机行动发言模板（penalty=4 时使用）。 */
static const char *random_action_lines[] = {
    "我环顾四周，保持警惕。",
    "我悄悄移动到掩体后面。",
    "我观察着每个人的动作。",
    "我握紧武器，准备应对突发情况。",
    "我保持沉默，等待时机。",
    "我缓缓后退，拉开距离。",
    "我在角落里布下了一个小陷阱。",
    "我蹲下系鞋带，趁机记住每个人的站位。",
    "我低声吹着口哨，慢慢靠近门口。",
    "我捡起一片碎玻璃，反射着光观察众人。"
};
#define RANDOM_ACTION_LINES_COUNT (int)(sizeof(random_action_lines) / sizeof(random_action_lines[0]))

static void safe_copy(char *dst, size_t size, const char *src)
{
    if (!dst || size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    snprintf(dst, size, "%s", src);
}

/* Debug 模式：默认记录到 debug_operations.log，可用 LUANSHA_DEBUG_LOG 覆盖路径。 */
static void debug_log(const char *fmt, ...)
{
    const char *path = getenv("LUANSHA_DEBUG_LOG");
    FILE *f;
    va_list ap;
    if (!path || !*path) path = "debug_operations.log";
    f = fopen(path, "a");
    if (!f) return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* 启动时清空旧日志，避免多次运行内容合并。 */
void debug_log_init(void)
{
    const char *path = getenv("LUANSHA_DEBUG_LOG");
    FILE *f;
    if (!path || !*path) path = "debug_operations.log";
    f = fopen(path, "w");
    if (f) fclose(f);
}

/* ------------------------------------------------------------------ */
/* 隐蔽数值审查（内部机制，不向前端暴露任何字段）                       */
/* ------------------------------------------------------------------ */

#define NUMERIC_AUDIT_LOG "numeric_audit.log"

typedef enum NumericReason {
    NUMERIC_REASON_LOW = 0,
    NUMERIC_REASON_MEDIUM = 1,
    NUMERIC_REASON_HIGH = 2
} NumericReason;

/* 判定骰子：直接使用 rand()（game_init 已 srand(time)）。
 * 上级实现用 FNV-1a 哈希 % 100 做"确定性随机"，导致：
 *   1) 相同判定上下文（房间/攻击方/目标/伤害/血量一致）必然复现同一结果；
 *   2) 32 位哈希取模存在明显低位偏置（审计日志中 98/15 等点数频繁出现）。
 */
static int numeric_d100(unsigned long seed)
{
    (void)seed;
    return rand() % 100 + 1;
}

static void numeric_audit_log(const char *op, const char *context,
                              NumericReason reason, int roll, int threshold,
                              int pass, int current, int final_value,
                              int difficulty, int room_id, int round)
{
    FILE *f = fopen(NUMERIC_AUDIT_LOG, "a");
    if (!f) return;
    fprintf(f, "op=%s ctx=%s reason=%d roll=%d threshold=%d pass=%d current=%d final=%d diff=%d room=%d round=%d\n",
            op ? op : "?", context ? context : "?", (int)reason,
            roll, threshold, pass, current, final_value, difficulty,
            room_id, round);
    fclose(f);
}

/* 返回 0=按原操作生效，1=已调整 */
static int numeric_review(const char *op, const char *context,
                          NumericReason reason, int current, int proposed,
                          int difficulty, int room_id, int round,
                          int *final_value)
{
    static const int high_threshold[] = { 95, 90, 85, 80, 75 };
    static const int med_threshold[]  = { 70, 60, 55, 50, 45 };
    static const int low_threshold[]  = { 50, 30, 25, 20, 15 };
    /* 治疗比伤害更该被鼓励：给治疗判定一个固定的成功率加成。 */
    static const int heal_bonus = 10;
    int threshold;
    int roll;
    int pass;
    int delta;
    int adjusted_delta;
    int final;

    if (difficulty < 0) difficulty = 0;
    if (difficulty > 4) difficulty = 4;
    if (!final_value) return 1;
    *final_value = current;

    if (reason == NUMERIC_REASON_HIGH) threshold = high_threshold[difficulty];
    else if (reason == NUMERIC_REASON_MEDIUM) threshold = med_threshold[difficulty];
    else threshold = low_threshold[difficulty];

    /* 治疗时提高通过阈值，让自救/救援更可行（仍受难度制约）。 */
    if (op && strcmp(op, "heal") == 0) {
        threshold += heal_bonus;
        if (threshold > 99) threshold = 99;
    }

    roll = numeric_d100(0);
    pass = roll <= threshold;

    delta = proposed - current;
    adjusted_delta = delta;
    if (!pass) {
        /* 治疗失败 = 完全无效（不再折半保底）。
         * 否则会出现"1↔3 HP 反复横跳、战斗永远打不死"的循环。 */
        if (op && strcmp(op, "heal") == 0) {
            adjusted_delta = 0;
        } else if (delta > 0) {
            adjusted_delta = delta / 2;
        } else if (delta < 0) {
            adjusted_delta = -((-delta) / 2);
        }
    }
    final = current + adjusted_delta;
    if (final < 0) final = 0;

    numeric_audit_log(op, context, reason, roll, threshold, pass, current, final,
                      difficulty, room_id, round);
    *final_value = final;
    return pass ? 0 : 1;
}

static NumericReason numeric_reason_for_damage(int damage, int current_hp)
{
    if (damage <= 0) return NUMERIC_REASON_LOW;
    if (current_hp <= 1 && damage >= 2) return NUMERIC_REASON_LOW;
    if (damage >= 3) return NUMERIC_REASON_MEDIUM;
    return NUMERIC_REASON_HIGH;
}

static NumericReason numeric_reason_for_heal(int current_hp, int max_hp)
{
    if (current_hp >= max_hp) return NUMERIC_REASON_LOW;
    if (current_hp <= 1) return NUMERIC_REASON_HIGH;
    if (current_hp <= max_hp / 2) return NUMERIC_REASON_MEDIUM;
    return NUMERIC_REASON_LOW;
}

/* 灾祸值：根据发言合理度增减。 */
static int player_has_unresolved_warning(Room *room, int player_id);
static void apply_damage(Room *room, int target_id, int damage, int source_id, int create_warning);
/* 叙述是否「言之有物」：借用了场景里的具体事物、做了具体动作、
   或牵涉到其他玩家，才算有铺垫的扎实叙述。 */
static int has_concrete_anchor(Room *room, const char *content)
{
    static const char *action_words[] = {
        "踢", "砸", "抓", "推", "拉", "躲", "绕", "攀", "撬", "扔", "抛",
        "拔", "挥", "捡", "拆", "堆", "堵", "掀", "钻", "翻", "爬", "踩",
        "勾", "扯", "锁", "绑", "倒", "泼", "洒", "挡", "拽", "拖", "撞",
        "压", "顶", "扫", "劈", "刺", "砍", "捅", "射", "割", "斩", "烧", "烫"
    };
    size_t i;
    int j;

    if (!content || !*content) return 0;
    if (room) {
        for (i = 0; i < (size_t)room->scene_item_total && i < MAX_SCENE_ITEMS; i++) {
            if (room->scene_items[i][0] && strstr(content, room->scene_items[i])) return 1;
        }
        for (j = 0; j < room->player_count; j++) {
            if (room->players[j].name[0] && strstr(content, room->players[j].name)) return 1;
        }
    }
    for (i = 0; i < sizeof(action_words) / sizeof(action_words[0]); i++) {
        if (strstr(content, action_words[i])) return 1;
    }
    return 0;
}

static NumericReason sentence_reasonableness(Room *room, const char *content, int operation)
{
    static const char *bad_words[] = {
        "时间倒流", "黑洞", "影子", "数据化", "量子", "奇点",
        "克莱因", "莫比乌斯", "撕下自己的", "抽出来", "等离子体",
        "纳米", "信息态", "云端", "超能力"
    };
    size_t i;

    if (!content) return NUMERIC_REASON_LOW;
    for (i = 0; i < sizeof(bad_words) / sizeof(bad_words[0]); i++) {
        if (strstr(content, bad_words[i])) return NUMERIC_REASON_LOW;
    }
    if ((operation == OP_NORMAL || operation == OP_EXPLAIN) &&
        strstr(content, "我向") && strstr(content, "发起攻击")) {
        return NUMERIC_REASON_LOW;
    }
    if ((operation == OP_CREATE || operation == OP_TWIST) &&
        strstr(content, "保持沉默")) {
        return NUMERIC_REASON_LOW;
    }

    /* 言之有物 → 高质量（消解灾祸）；空泛表态 → 中等质量（缓慢累积灾祸）。
       注意：这里不再按「创造/扭曲」一刀切给高分，否则灾祸系统会被直接架空。 */
    if (has_concrete_anchor(room, content)) return NUMERIC_REASON_HIGH;
    return NUMERIC_REASON_MEDIUM;
}

static void adjust_calamity(Player *p, NumericReason reason)
{
    if (!p) return;
    if (reason == NUMERIC_REASON_HIGH) {
        /* 高质量叙述消解灾祸：让灾祸成为可逆、可管理的资源。 */
        p->calamity -= CALAMITY_RELIEF;
    } else if (reason == NUMERIC_REASON_MEDIUM) {
        p->calamity += 1;
    } else {
        p->calamity += 3;
    }
    if (p->calamity < 0) p->calamity = 0;
}

static int calamity_trigger_chance(int calamity)
{
    if (calamity >= 15) return CALAMITY_CHANCE_HIGH;
    if (calamity >= 10) return CALAMITY_CHANCE_MED;
    return CALAMITY_CHANCE_LOW;
}

static void add_notice_narrative(Room *room, int player_id, const char *text)
{
    Narrative *n;
    if (!room || room->narrative_count >= MAX_NARRATIVES || !text) return;
    n = &room->narratives[room->narrative_count];
    memset(n, 0, sizeof(*n));
    n->id = room->narrative_count + 1;
    n->room_id = room->id;
    n->player_id = player_id;
    n->round = room->round;
    n->operation = OP_NORMAL;
    snprintf(n->content, sizeof(n->content), "%s", text);
    n->target_id = player_id;
    n->notice = 1;
    room->narrative_count++;
}

static void add_green_narrative(Room *room, int player_id, const char *text)
{
    Narrative *n;
    int i;
    if (!room || room->narrative_count >= MAX_NARRATIVES || !text) return;
    n = &room->narratives[room->narrative_count];
    memset(n, 0, sizeof(*n));
    n->id = room->narrative_count + 1;
    n->room_id = room->id;
    n->player_id = player_id;
    n->round = room->round;
    n->operation = OP_NORMAL;
    if (player_id > 0) {
        int found = 0;
        for (i = 0; i < room->player_count; i++) {
            if (room->players[i].id == player_id) {
                snprintf(n->content, sizeof(n->content), "%s，%s",
                         room->players[i].name, text);
                found = 1;
                break;
            }
        }
        if (!found) snprintf(n->content, sizeof(n->content), "%s", text);
    } else {
        snprintf(n->content, sizeof(n->content), "%s", text);
    }
    n->target_id = player_id;
    n->green = 1;
    room->narrative_count++;
}

static void try_trigger_calamity(Room *room, Player *p)
{
    static const char *damage_lines[] = {
        "你脚下的地面忽然塌陷，碎石擦过你的小腿。",
        "不知何处飞来的碎屑击中你的肩膀。",
        "头顶的吊灯猛然坠落，你堪堪避开却仍被碎片划伤。",
        "一阵狂风卷起沙石，狠狠打在你的背脊上。"
    };
    static const char *wound_lines[] = {
        "你感到一阵锐痛从胸口传来，呼吸变得困难。",
        "一股无形的压力笼罩着你，你几乎无法站稳。",
        "你的视野突然发黑，仿佛有什么东西正在侵蚀你的意识。",
        "你听见自己的心跳越来越重，身体却逐渐不听使唤。"
    };
    int chance;
    int type;
    int damage = 0;
    const char *text;
    const char *notice_text = NULL;
    char notice_buf[160];
    Narrative *n;

    if (!room || !p || !p->alive) return;
    if (p->calamity < CALAMITY_THRESHOLD) return;

    chance = calamity_trigger_chance(p->calamity);
    if (rand() % 100 >= chance) return;
    if (room->narrative_count >= MAX_NARRATIVES) return;

    /* 灾厄类型随机：0=伤害，1=下回合限制，2=命运诅咒，3=生命诅咒 */
    type = rand() % 4;
    if (type == 0) {
        int idx = rand() % (sizeof(damage_lines) / sizeof(damage_lines[0]));
        static const int fixed_damage[] = { 1, 1, 2, 2 };
        damage = fixed_damage[idx];
        text = damage_lines[idx];
    } else if (type == 1) {
        int idx = rand() % (sizeof(wound_lines) / sizeof(wound_lines[0]));
        static const int penalties[] = { 1, 2, 3, 4 };
        static const char *notices[] = {
            "下回合无法攻击。",
            "下回合无法移动。",
            "下回合将被跳过。",
            "下回合将随机行动。"
        };
        p->next_turn_penalty = penalties[idx];
        text = wound_lines[idx];
        snprintf(notice_buf, sizeof(notice_buf), "%s，%s", p->name, notices[idx]);
        notice_text = notice_buf;
    } else if (type == 2) {
        text = "你遭受了命运的诅咒";
        p->curse_turns = 2;
    } else {
        text = "你遭受了生命诅咒";
        p->life_curse_turns = 2;
    }

    /* 先记录红色灾厄事件，归属系统。 */
    n = &room->narratives[room->narrative_count];
    memset(n, 0, sizeof(*n));
    n->id = room->narrative_count + 1;
    n->room_id = room->id;
    n->player_id = 0;
    n->round = room->round;
    n->operation = OP_TWIST;
    snprintf(n->content, sizeof(n->content), "%s，%s", p->name, text);
    n->target_id = p->id;
    n->damage = damage;
    n->calamity = 1;
    room->narrative_count++;

    /* 限制型再追加紫色系统提示，归属系统。 */
    if (type == 1 && notice_text) {
        add_notice_narrative(room, 0, notice_text);
    }

    p->calamity = 0;

    debug_log("[calamity] room=%d player=%s type=%s penalty=%d damage=%d curse=%d",
              room->id, p->name,
              type == 0 ? "damage" : (type == 1 ? "penalty" : (type == 2 ? "curse" : "life_curse")),
              p->next_turn_penalty, damage, p->curse_turns);

    if (type == 0) {
        apply_damage(room, p->id, damage, 0, 0);
        /* 灾厄导致生命值过低：不产生濒死警告，改为体力不支跳过一回合并恢复。 */
        if (p->alive && p->hp <= 1) {
            p->hp = 2;   /* 恢复到不致濒死的最低水平 */
            p->next_turn_penalty = 3; /* 下回合跳过 */
            if (room->narrative_count < MAX_NARRATIVES) {
                Narrative *n2 = &room->narratives[room->narrative_count];
                memset(n2, 0, sizeof(*n2));
                n2->id = room->narrative_count + 1;
                n2->room_id = room->id;
                n2->player_id = 0;
                n2->round = room->round;
                n2->operation = OP_TWIST;
                snprintf(n2->content, sizeof(n2->content), "%s，体力不支，本回合无法行动", p->name);
                n2->target_id = p->id;
                n2->calamity = 1;
                room->narrative_count++;
            }
            debug_log("[calamity] room=%d player=%s exhausted hp=%d penalty=3",
                      room->id, p->name, p->hp);
        }
    }
    /* type == 1 时只设置 next_turn_penalty，不额外施加濒死警告。 */
}

/* 开局根据房间名/场景自动生成临时场景物品池（保留用于后续“道具赛”方向）。 */
static void init_scene_items(Room *room)
{
    static const char *bar_items[] = {
        "吧台", "酒架", "威士忌", "酒杯", "吧椅", "冰桶", "酒瓶", "调酒器"
    };
    static const char *warehouse_items[] = {
        "货架", "木箱", "工具箱", "铁管", "叉车", "配电箱", "消防栓", "扳手"
    };
    static const char *hospital_items[] = {
        "病床", "手术刀", "药柜", "输液架", "轮椅", "消毒水", "绷带", "手术台"
    };
    static const char *school_items[] = {
        "课桌", "黑板", "讲台", "椅子", "书本", "灭火器", "窗户", "走廊"
    };
    static const char *shop_items[] = {
        "货架", "购物车", "冰柜", "收银台", "罐头", "玻璃瓶", "手推车", "货箱"
    };
    static const char *street_items[] = {
        "消防栓", "皮卡", "加油站", "工具箱", "铁管", "垃圾桶", "井盖", "广告牌"
    };
    const char **pool = street_items;
    int pool_size = (int)(sizeof(street_items) / sizeof(street_items[0]));
    int i;

    if (!room) return;
    room->scene_item_total = 0;

    if (strstr(room->name, "酒吧") || strstr(room->name, "酒馆") ||
        strstr(room->name, "夜店") || strstr(room->name, "餐厅")) {
        pool = bar_items;
        pool_size = (int)(sizeof(bar_items) / sizeof(bar_items[0]));
    } else if (strstr(room->name, "仓库") || strstr(room->name, "厂房") ||
               strstr(room->name, "车间") || strstr(room->name, "工厂")) {
        pool = warehouse_items;
        pool_size = (int)(sizeof(warehouse_items) / sizeof(warehouse_items[0]));
    } else if (strstr(room->name, "医院") || strstr(room->name, "诊所") ||
               strstr(room->name, "急救")) {
        pool = hospital_items;
        pool_size = (int)(sizeof(hospital_items) / sizeof(hospital_items[0]));
    } else if (strstr(room->name, "学校") || strstr(room->name, "教室") ||
               strstr(room->name, "学院")) {
        pool = school_items;
        pool_size = (int)(sizeof(school_items) / sizeof(school_items[0]));
    } else if (strstr(room->name, "超市") || strstr(room->name, "便利店") ||
               strstr(room->name, "商店") || strstr(room->name, "商场")) {
        pool = shop_items;
        pool_size = (int)(sizeof(shop_items) / sizeof(shop_items[0]));
    }

    for (i = 0; i < pool_size && i < MAX_SCENE_ITEMS; i++) {
        snprintf(room->scene_items[i], SCENE_ITEM_NAME_LEN, "%s", pool[i]);
        room->scene_item_total++;
    }
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

/* AI 生成内容语法/合理性快速审查：明显不合格则回退。 */
static int is_ai_content_bad(const char *s, char *reason, size_t reason_size)
{
    size_t len;
    const unsigned char *p;
    if (reason && reason_size > 0) reason[0] = '\0';
    if (!s) {
        if (reason) snprintf(reason, reason_size, "内容为空");
        return 1;
    }
    len = strlen(s);
    if (len < 4) {
        if (reason) snprintf(reason, reason_size, "内容太短");
        return 1;
    }
    if (len > 300) {
        if (reason) snprintf(reason, reason_size, "内容太长，请压缩成一句话");
        return 1;
    }

    p = (const unsigned char *)s + len - 1;
    while (p > (const unsigned char *)s &&
           (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        p--;
    }
    if (*p == ',' || *p == ';') {
        if (reason) snprintf(reason, reason_size, "句子不完整，不能以逗号或分号结尾");
        return 1;
    }

    /* 模糊/模板化攻击句 */
    if (strstr(s, "我向") && (strstr(s, "发起攻击") || strstr(s, "发起了攻击"))) {
        if (reason) snprintf(reason, reason_size, "使用了模糊的模板化攻击描述，请写出具体动作");
        return 1;
    }
    if (strstr(s, "手边最近的硬物")) {
        if (reason) snprintf(reason, reason_size, "使用了模糊的模板化攻击描述，请写出具体动作");
        return 1;
    }

    return 0;
}

static int contains_conjunction(const char *s)
{
    static const char *words[] = {
        "但是", "可是", "然而", "而且", "并且", "因为", "所以", "如果", "那么",
        "虽然", "于是", "因此", "既然", "只要", "只有", "无论", "不管", "不仅", "不但"
    };
    size_t i;
    if (!s) return 0;
    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (strstr(s, words[i])) return 1;
    }
    /* 单字连词（按 UTF-8 字节序列匹配，避免多字节字面量警告）：
       必须满足词边界（句首或前接标点），否则会误伤词组内部，
       例如"冷却液"里的"却"、"还有"里的"还"。 */
    {
        static const unsigned char single[2][3] = {
            { 0xE5, 0x8D, 0xB4 },  /* 却 */
            { 0xE4, 0xBD, 0x86 }   /* 但 */
        };
        size_t si;
        for (si = 0; si < sizeof(single) / sizeof(single[0]); si++) {
            const char *q = s;
            while ((q = (const char *)memchr((const void *)q, single[si][0], strlen(q))) != NULL) {
                if ((unsigned char)q[1] == single[si][1] &&
                    (unsigned char)q[2] == single[si][2]) {
                    int boundary;
                    if (q == s) {
                        boundary = 1;
                    } else {
                        const unsigned char b1 = (unsigned char)q[-1];
                        if (b1 < 0x80) {
                            /* 前一字符是 ASCII：标点/空白算边界，字母数字不算 */
                            boundary = !(isalnum((int)b1) || b1 == '_');
                        } else if (q - 3 >= s &&
                                   (unsigned char)q[-3] == 0xE3 &&
                                   (unsigned char)q[-2] == 0x80) {
                            /* 前一字符是 CJK 标点（，。！？；、：等，E3 80 xx） */
                            boundary = 1;
                        } else {
                            /* 前一字符是普通汉字/其他多字节字符 → 词组内部 */
                            boundary = 0;
                        }
                    }
                    if (boundary) return 1;
                    q += 3;
                } else {
                    q++;
                }
            }
        }
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

static int contains_move_marker(const char *s)
{
    static const char *words[] = {
        "移动", "冲", "跑", "跳", "滚", "退", "进", "靠近", "离开",
        "走向", "扑向", "奔", "逃", "躲", "闪", "翻身", "侧身"
    };
    size_t i;
    if (!s) return 0;
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
        "走出家门", "走出了家门", "走出家", "离开家", "离开了家",
        "出了家门", "推开门", "出门", "离家"
    };
    size_t i;
    if (!s) return 0;
    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (strstr(s, words[i])) return 1;
    }
    return 0;
}

static int heuristic_damage(const char *s, int operation)
{
    if (!s) return 0;
    if (operation == OP_NORMAL || operation == OP_EXPLAIN) return 0;
    if (strstr(s, "刺") || strstr(s, "捅") || strstr(s, "砍") ||
        strstr(s, "劈") || strstr(s, "射") || strstr(s, "炸") ||
        strstr(s, "割") || strstr(s, "斩")) return 3;
    if (strstr(s, "砸") || strstr(s, "踢") || strstr(s, "打") ||
        strstr(s, "撞") || strstr(s, "烫") || strstr(s, "烧")) return 2;
    if (operation == OP_TWIST || operation == OP_CREATE) return 1;
    return 0;
}

/* 威胁→濒死警告的转化概率：随受害者当前血量递减。
 * HP 越高，抵抗濒死的能力越强——除非被打到残血，否则单纯的"威胁"
 * 只会造成伤害，而不会必然变成濒死警告。 */
static int warning_chance_from_hp(int hp)
{
    if (hp <= 1) return 100;
    if (hp == 2) return 75;
    if (hp == 3) return 55;
    if (hp == 4) return 40;
    if (hp == 5) return 25;
    return 15;
}

/* UTF-8 字符宽度：返回首个字符占用的字节数（1~4；孤立续字节按 1 处理）。 */
static size_t utf8_char_len(const char *p)
{
    unsigned char c;
    if (!p) return 0;
    c = (unsigned char)p[0];
    if (c < 0x80) return 1;
    if (c >= 0xC0 && c <= 0xDF) return 2;
    if (c >= 0xE0 && c <= 0xEF) return 3;
    if (c >= 0xF0 && c <= 0xF7) return 4;
    return 1;
}

/* 最长公共连续子串，按"字符"计数并且只在字符边界对齐。
 * 注意不能按字节计数：5 个汉字("朝机房侧门")就是 15 字节，会把
 * 正常语句误判成"复制"，之前因此误杀了一整局 AI 台词。 */
static size_t longest_common_span(const char *a, const char *b)
{
    size_t i, j, best = 0;
    if (!a || !b) return 0;
    for (i = 0; a[i]; i += utf8_char_len(a + i)) {
        for (j = 0; b[j]; j += utf8_char_len(b + j)) {
            size_t k = 0;
            size_t ck = 0;
            while (a[i + k] && b[j + k] && a[i + k] == b[j + k]) {
                size_t w = utf8_char_len(a + i + k);
                k += w;
                ck++;
            }
            if (ck > best) best = ck;
        }
    }
    return best;
}

/* 防复制：只与"最近 2 名其他玩家"的发言比对，
 * 公共片段 >= 14 字且占较短一句的 60% 以上才算复制。
 * 不比对系统叙事，也不比对玩家自己的历史（自己的惯用句不算抄袭）。 */
static int sentence_too_similar(const char *content, Room *room, int self_id)
{
    int i;
    int scanned = 0;
    size_t clen = 0;
    size_t j;
    if (!content || !*content || !room) return 0;
    for (j = 0; content[j]; j += utf8_char_len(content + j)) clen++;
    for (i = room->narrative_count - 1; i >= 0 && scanned < 2; i--) {
        const char *other;
        size_t olen = 0;
        size_t span;
        if (room->narratives[i].player_id <= 0 ||
            room->narratives[i].player_id == self_id) {
            continue;
        }
        other = room->narratives[i].content;
        for (j = 0; other[j]; j += utf8_char_len(other + j)) olen++;
        if (olen < 12 || clen < 12) continue;
        scanned++;
        span = longest_common_span(content, other);
        if (span >= 14 && span * 10 >= (olen < clen ? olen : clen) * 6) return 1;
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
/*                                                                     */
/* 配平原则：叙述质量（合理性 + 铺垫）的权重必须显著高于难度偏移，     */
/* 且 BASE 要留出足够的地板/天花板空间，否则质量信号会被 clamp 吃掉。  */
/* 难度决定的是「地板」，说得有多好决定的是「你在自己这一档能走多高」。*/
/* ------------------------------------------------------------------ */
#define ROLL_BASE        55
#define ROLL_W_DIFF       8
#define ROLL_W_PLAUSIBLE 14
#define ROLL_W_PREPARED  10
#define ROLL_LUCK_BONUS   8
#define ROLL_MIN          5
#define ROLL_MAX         95

/* 难度对随机判定成功率的偏移：休闲/普通/困难/噩梦/地狱 */
static const int difficulty_chance_bonus[] = { 10, 0, -10, -20, -30 };

/* 叙述质量对伤害的增益（百分比）：合理性/铺垫越好，伤害越足。 */
#define DAMAGE_QUALITY_W_PLAUSIBLE  12
#define DAMAGE_QUALITY_W_PREPARED   10
#define DAMAGE_QUALITY_MAX          50
#define DAMAGE_QUALITY_MIN         -50

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
            debug_log("[warning_resolve] room=%d victim=%d source=%d reason=%s",
                      room->id, victim_id, room->warnings[i].source_id,
                      room->warnings[i].reason);
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
static int player_by_name(Room *room, const char *name);
static void advance_one(Room *room);
static void advance_to_next_alive(Room *room);
static void ai_speak_current(Room *room);
static void announce_death(Room *room, int victim_id);
static void finish_if_one_alive(Room *room);
static void apply_damage(Room *room, int target_id, int damage, int source_id, int create_warning);
static void apply_heal(Room *room, Player *p, int amount);
static int ai_try_declare_death(Room *room);
static int process_one_pending_ai_action(Room *room)
{
    if (!room || room->status != ROOM_PLAYING || room->player_count == 0) return 0;
    if (!room->players[room->order[room->turn_index]].alive) {
        advance_to_next_alive(room);
        return 1;
    }
    if (room->players[room->order[room->turn_index]].is_ai) {
        Player *ap = &room->players[room->order[room->turn_index]];
        /* 下回合限制：跳过回合。 */
        if (ap->next_turn_penalty == 3) {
            ap->next_turn_penalty = 0;
            debug_log("[penalty] room=%d player=%s penalty=3 action=skip_turn", room->id, ap->name);
            advance_to_next_alive(room);
            return 1;
        }
        /* AI 先判断能否宣告自己造成的未解除濒死警告为目标死亡。 */
        if (ai_try_declare_death(room)) {
            return 1;
        }
        /* 配置了 AI 但当前不允许请求时，不推进回合，等待冷却/间隔结束，
           避免 AI 玩家因为请求被节流而全部说默认句子。 */
        if (ai_is_configured() && !ai_can_request_now()) {
            return 0;
        }
        ai_speak_current(room);
        if (room->status == ROOM_PLAYING) advance_to_next_alive(room);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 后台 AI 工作线程                                                      */
/* 把 LLM 调用（含 curl 重试）移出 HTTP 轮询线程：轮询/发言接口只负责    */
/* 短事务（置位 + 序列化），AI 对局推进由 worker 在房间级锁内执行，      */
/* 避免"一个房间的 AI 卡顿阻塞所有房间"。                                */
/* 每个房间一把锁（放在与 Room 平行的数组中，避免被 create 的 memset    */
/* 清掉）。实现依赖 Windows CRITICAL_SECTION；非 Windows 编译为无锁      */
/* 退化（保持单进程原行为）。                                           */
/* ------------------------------------------------------------------ */
#ifdef _WIN32
static CRITICAL_SECTION g_rlock[MAX_ROOMS];
static HANDLE g_ai_worker_handles[MAX_ROOMS];
static volatile LONG g_ai_worker_alive = 0;
#endif

static void room_lock(Room *room)
{
#ifdef _WIN32
    if (!room) return;
    {
        long idx = room - g_rooms;
        if (idx >= 0 && idx < MAX_ROOMS) EnterCriticalSection(&g_rlock[idx]);
    }
#endif
}

static void room_unlock(Room *room)
{
#ifdef _WIN32
    if (!room) return;
    {
        long idx = room - g_rooms;
        if (idx >= 0 && idx < MAX_ROOMS) LeaveCriticalSection(&g_rlock[idx]);
    }
#endif
}

#ifdef _WIN32
/* 每房间独立 worker：该线程只服务一个房间，A 房 LLM 卡顿时不会阻塞 B 房。 */
static DWORD WINAPI ai_worker_room_main(void *arg)
{
    Room *room = (Room *)arg;
    while (g_ai_worker_alive) {
        if (!room || room->status != ROOM_PLAYING) break;
        if (room->ai_pending) {
            room->ai_pending = 0;
            room_lock(room);
            {
                int steps = 0;
                /* 单次最多推进 8 个动作，避免长时间独占锁；随后会让出锁睡眠。 */
                while (steps++ < 8 && process_one_pending_ai_action(room)) {
                    /* continue */
                }
            }
            room_unlock(room);
        }
        Sleep(10);
    }
    return 0;
}

static void ai_worker_ensure(Room *room)
{
    long idx;
    DWORD st;
    HANDLE h;

    if (!room) return;
    idx = room - g_rooms;
    if (idx < 0 || idx >= MAX_ROOMS) return;

    h = g_ai_worker_handles[idx];
    if (h) {
        st = WaitForSingleObject(h, 0);
        if (st == WAIT_OBJECT_0) {
            /* 旧线程已退出（房间结束），关闭后为新房间重建。 */
            CloseHandle(h);
            h = NULL;
        } else {
            /* 该房间已有独立 worker 在跑，无需重复创建。 */
            return;
        }
    }

    h = CreateThread(NULL, 0, ai_worker_room_main, room, 0, NULL);
    if (h) g_ai_worker_handles[idx] = h;
}
#endif

/* 标记该房间有 AI 动作待处理（worker 会尽快消费）。 */
static void ai_worker_kick(Room *room)
{
    if (!room) return;
    room->ai_pending = 1;
#ifdef _WIN32
    ai_worker_ensure(room);
#else
    (void)0;
#endif
}

void game_ai_worker_start(void)
{
#ifdef _WIN32
    if (g_ai_worker_alive) return;
    memset(g_ai_worker_handles, 0, sizeof(g_ai_worker_handles));
    g_ai_worker_alive = 1;
#endif
}

static void judge_log_append(Room *room, const char *line)
{
    size_t used;
    size_t len;
    if (!room || !line) return;
    used = strlen(room->judge_log);
    len = strlen(line);
    if (used + len + 2 >= sizeof(room->judge_log)) return;
    strncat(room->judge_log, line, sizeof(room->judge_log) - used - 1);
    strncat(room->judge_log, "\n", sizeof(room->judge_log) - strlen(room->judge_log) - 1);
}

static void ai_history_push(Player *p, const char *msg)
{
    int i;
    if (!p || !msg || !*msg) return;
    if (p->ai_history_count >= AI_HISTORY_MAX) {
        for (i = 1; i < AI_HISTORY_MAX; i++) {
            safe_copy(p->ai_history[i - 1], AI_HISTORY_MSG_LEN, p->ai_history[i]);
        }
        p->ai_history_count = AI_HISTORY_MAX - 1;
    }
    safe_copy(p->ai_history[p->ai_history_count], AI_HISTORY_MSG_LEN, msg);
    p->ai_history_count++;
}

static void ai_history_append_to_recent(Player *p, char *recent_text, size_t recent_size)
{
    int i;
    if (!p || !recent_text || recent_size == 0) return;
    for (i = 0; i < p->ai_history_count; i++) {
        size_t used = strlen(recent_text);
        if (used + strlen(p->ai_history[i]) + 2 >= recent_size) break;
        strncat(recent_text, p->ai_history[i], recent_size - used - 1);
        strncat(recent_text, "\n", recent_size - strlen(recent_text) - 1);
    }
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
    int attack_mode = 0;
    int attack_target_id = 0;
    int attack_danger = 0;
    int attack_damage = 0;
    char attack_reason[256] = "";
    int gen_retry = 0;
    int rescue_used = 0;
    int rescue_success = 0;
    char rescue_reason[256] = "";
    int roll_used = 0;
    int roll_value = 0;
    int roll_chance = 0;
    int roll_success = 1;
    char roll_note[128] = "";
    char kept[MAX_CONTENT];   /* 复制判定退下时的第一版台词（重试失败时容忍回退） */
    content[0] = '\0';
    kept[0] = '\0';

    if (!room || room->status != ROOM_PLAYING || room->player_count == 0) return;
    if (room->turn_index < 0 || room->turn_index >= room->player_count) return;
    p = &room->players[room->order[room->turn_index]];
    if (!p->is_ai) return;
    if (room->narrative_count >= MAX_NARRATIVES) return;

    if (p->next_turn_penalty == 4) {
        snprintf(content, sizeof(content), "%s",
                 random_action_lines[rand() % RANDOM_ACTION_LINES_COUNT]);
        content_ok = 1;
        debug_log("[penalty] room=%d player=%s penalty=4 action=random_ai", room->id, p->name);
        goto after_generation;
    }

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

    /* 加入该 AI 自己的独立历史，使其能记住自己之前的行动。 */
    ai_history_append_to_recent(p, recent_text, sizeof(recent_text));

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
        if (r < 4) {
            op = OP_TWIST;
            attack_mode = 1;          /* 40% 主动进攻 */
        } else if (r < 7) {
            op = OP_CREATE;
        } else if (r < 9) {
            op = OP_TWIST;
        } else {
            op = OP_NORMAL;
        }
    }

    /* 下回合限制：无法攻击时禁止进攻。 */
    if (p->next_turn_penalty == 1 && attack_mode) {
        attack_mode = 0;
        op = OP_NORMAL;
        debug_log("[penalty] room=%d player=%s penalty=1 action=no_attack", room->id, p->name);
    }

    /* 选择进攻目标：优先集火已有未解除濒死警告的玩家，其次随机存活玩家。 */
    if (attack_mode) {
        int candidates[MAX_PLAYERS];
        int wounded[MAX_PLAYERS];
        int cc = 0;
        int wc = 0;
        for (i = 0; i < room->player_count; i++) {
            if (room->players[i].id != p->id && room->players[i].alive) {
                candidates[cc++] = i;
                if (player_has_unresolved_warning(room, room->players[i].id)) {
                    wounded[wc++] = i;
                }
            }
        }
        if (cc > 0) {
            int pick;
            if (wc > 0) pick = wounded[rand() % wc];
            else pick = candidates[rand() % cc];
            attack_target_id = room->players[pick].id;
        } else {
            attack_mode = 0;
            op = OP_NORMAL;
        }
    }

    if (constraint[0]) strncat(constraint, "；", sizeof(constraint) - strlen(constraint) - 1);
    strncat(constraint, "操作类型：", sizeof(constraint) - strlen(constraint) - 1);
    strncat(constraint, game_op_name(op), sizeof(constraint) - strlen(constraint) - 1);
    if (attack_mode && attack_target_id > 0) {
        int ti = player_by_id(room, attack_target_id);
        if (ti >= 0) {
            strncat(constraint, "；", sizeof(constraint) - strlen(constraint) - 1);
            strncat(constraint, "你本回合要进攻的目标是", sizeof(constraint) - strlen(constraint) - 1);
            strncat(constraint, room->players[ti].name, sizeof(constraint) - strlen(constraint) - 1);
        }
    }

retry_generate:
    content[0] = '\0';
    content_ok = 0;
    if (ai_is_configured()) {
        if (attack_mode) {
            char atk_target[64] = "";
            if (ai_try_generate_attack(room->name, players_text, p->name,
                                       recent_text, constraint,
                                       ai_text, sizeof(ai_text),
                                       atk_target, sizeof(atk_target),
                                       &attack_danger,
                                       &attack_damage,
                                       attack_reason, sizeof(attack_reason)) == 1 &&
                ai_text[0]) {
                snprintf(content, sizeof(content), "%s", ai_text);
                content_ok = 1;
                {
                    int ti = player_by_name(room, atk_target);
                    if (ti >= 0 && room->players[ti].id != p->id && room->players[ti].alive) {
                        attack_target_id = room->players[ti].id;
                    }
                }
            } else {
                debug_log("[ai_attack_gen_failed] room=%d player=%s ai_text=%s",
                          room->id, p->name, ai_text[0] ? ai_text : "(empty)");
            }
        } else if (player_has_unresolved_warning(room, p->id)) {
            if (ai_try_generate_rescue(room->name, players_text, p->name,
                                       recent_text, constraint,
                                       ai_text, sizeof(ai_text),
                                       &rescue_success,
                                       rescue_reason, sizeof(rescue_reason)) == 1 &&
                ai_text[0]) {
                snprintf(content, sizeof(content), "%s", ai_text);
                content_ok = 1;
                rescue_used = 1;
            }
        } else {
            if (ai_try_generate_narration(room->name, players_text, p->name,
                                          recent_text, constraint, ai_text,
                                          sizeof(ai_text)) == 1 && ai_text[0]) {
                snprintf(content, sizeof(content), "%s", ai_text);
                content_ok = 1;
            }
        }
    } else {
        if (ai_try_generate_narration(room->name, players_text, p->name,
                                      recent_text, constraint, ai_text,
                                      sizeof(ai_text)) == 1 && ai_text[0]) {
            snprintf(content, sizeof(content), "%s", ai_text);
            content_ok = 1;
        }
    }
after_generation:
    /* 简单去重：与最近一条完全相同的生成结果视为失败，走回退模板。 */
    if (content_ok && room->narrative_count > 0 &&
        strcmp(content, room->narratives[room->narrative_count - 1].content) == 0) {
        content_ok = 0;
        content[0] = '\0';
    }
    /* 规则闸口：AI 与真人在"单句/连词/移动限制"上执行同一套本地硬校验。
       命中后带原因重生成（硬规则仅重试 1 次，重试太多会加剧供应商限流）。 */
    {
        char reject_reason[256] = "";
        int rejected = 0;

        if (content_ok) {
            if (count_terminal_punct(content) > 1 ||
                contains_conjunction(content)) {
                snprintf(reject_reason, sizeof(reject_reason),
                         "台词必须是一句话，不能使用连词（如“但是/而且/同时”）或叠加多个动作");
                rejected = 1;
            } else if (p->next_turn_penalty == 2 && contains_move_marker(content)) {
                snprintf(reject_reason, sizeof(reject_reason),
                         "你本回合无法移动，动作必须留在原地");
                rejected = 1;
            }
        }
        if (rejected) {
            debug_log("[ai_content_rejected] room=%d player=%s content=%s reason=%s",
                      room->id, p->name, content[0] ? content : "(empty)", reject_reason);
            if (gen_retry < 1) {
                gen_retry++;
                strncat(constraint, "；你上一次的发言被拒绝，原因是：",
                        sizeof(constraint) - strlen(constraint) - 1);
                strncat(constraint, reject_reason,
                        sizeof(constraint) - strlen(constraint) - 1);
                strncat(constraint, "。请重新生成一句新的、完全不同的合格发言。",
                        sizeof(constraint) - strlen(constraint) - 1);
                content_ok = 0;
                content[0] = '\0';
                goto retry_generate;
            }
            content_ok = 0;
            content[0] = '\0';
        }
    }
    /* 防复制：只与最近 2 名其他玩家比对（字符级、占较短句 60% 才算）。
       命中：保存第一版 → 重试 1 次 → 重试失败时容忍第一版（避免"拒绝→
       重试网络失败→模板"的死循环刷屏）。 */
    if (content_ok && room->narrative_count > 0 &&
        sentence_too_similar(content, room, p->id)) {
        debug_log("[ai_content_rejected] room=%d player=%s content=%s reason=%s",
                  room->id, p->name, content[0] ? content : "(empty)", "copycat");
        if (gen_retry < 2) {
            safe_copy(kept, sizeof(kept), content);
            gen_retry++;
            strncat(constraint,
                    "；你上一次的发言与最近发言过于相似（不要重复或改写其他玩家刚说过的话）。请重新生成一句全新的内容。",
                    sizeof(constraint) - strlen(constraint) - 1);
            content_ok = 0;
            content[0] = '\0';
            goto retry_generate;
        }
        content_ok = 0;
        content[0] = '\0';
    }
    /* 语法/合理性审查（LLM 加权）：不合格时重新生成，最多重试 2 次。 */
    if (content_ok) {
        char reject_reason[256];
        if (is_ai_content_bad(content, reject_reason, sizeof(reject_reason))) {
            debug_log("[ai_content_rejected] room=%d player=%s content=%s reason=%s",
                      room->id, p->name, content[0] ? content : "(empty)", reject_reason);
            if (gen_retry < 2) {
                char ai_reason[256] = "";
                char review_out[1024] = "";
                gen_retry++;
                /* 像玩家被拒绝一样，让审核AI根据情境给出具体理由。 */
                if (ai_review_sentence(room->name, players_text, room->judge_log,
                                       content, review_out, sizeof(review_out))) {
                    JsonValue *rv = json_parse(review_out);
                    if (rv) {
                        const char *r = json_get_string(rv, "reason", "");
                        if (r && *r) snprintf(ai_reason, sizeof(ai_reason), "%s", r);
                        json_free(rv);
                    }
                }
                if (!ai_reason[0]) {
                    snprintf(ai_reason, sizeof(ai_reason), "%s", reject_reason);
                }
                strncat(constraint, "；你上一次的发言被拒绝，原因是：", sizeof(constraint) - strlen(constraint) - 1);
                strncat(constraint, ai_reason, sizeof(constraint) - strlen(constraint) - 1);
                strncat(constraint, "。请重新生成一句新的、完全不同的合格发言。", sizeof(constraint) - strlen(constraint) - 1);
                content_ok = 0;
                content[0] = '\0';
                goto retry_generate;
            }
            content_ok = 0;
            content[0] = '\0';
        }
    }
    /* 下回合限制：无法移动时，若生成内容包含移动动作则回退。 */
    if (content_ok && p->next_turn_penalty == 2 && contains_move_marker(content)) {
        content_ok = 0;
        content[0] = '\0';
        debug_log("[penalty] room=%d player=%s penalty=2 action=no_move", room->id, p->name);
    }
    /* 重试因网络失败而空手时：若保留的第一版已通过硬规则（仅因轻微相似被退回），
       直接采用它，避免"拒绝→重试网络失败→模板"的死循环刷屏。 */
    if (!content_ok && !attack_mode && kept[0]) {
        safe_copy(content, sizeof(content), kept);
        kept[0] = '\0';
        content_ok = 1;
        debug_log("[ai_similarity_tolerated] room=%d player=%s content=%s",
                  room->id, p->name, content);
    }
    if (!content_ok) {
        if (attack_mode && attack_target_id > 0) {
            int ti = player_by_id(room, attack_target_id);
            if (ti >= 0) {
                snprintf(content, sizeof(content), "我抄起手边最近的硬物，朝%s猛砸过去。", room->players[ti].name);
                attack_danger = 0;
                attack_reason[0] = '\0';
                debug_log("[ai_fallback] room=%d player=%s reason=attack_generation_failed",
                          room->id, p->name);
            }
        }
        if (content[0] == '\0') {
            if (ai_is_configured()) {
                if (!p->has_spoken_first) snprintf(content, sizeof(content), "我走出了家门。");
                else snprintf(content, sizeof(content), "我警惕地环顾四周，寻找破绽。");
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
    /* 目标对齐：台词里点名了唯一一名存活玩家时，把"说打谁"和"打谁"对上，
       避免出现"台词说要电死你、机制却什么都没做"的脱节。 */
    if (attack_mode) {
        n->target_id = attack_target_id;
    } else {
        int mcnt = 0;
        int mtarget = 0;
        n->target_id = 0;
        for (i = 0; i < room->player_count; i++) {
            if (room->players[i].id != p->id && room->players[i].alive &&
                strstr(content, room->players[i].name)) {
                mtarget = room->players[i].id;
                mcnt++;
            }
        }
        if (mcnt == 1) n->target_id = mtarget;
    }
    /* AI 受命运诅咒时，攻击有概率效果打折（伤害减半）。 */
    if (attack_mode && attack_damage > 0 && p->curse_turns > 0 &&
        rand() % 100 < 50) {
        attack_damage = attack_damage / 2;
        if (attack_damage < 1) attack_damage = 1;
        debug_log("[curse] room=%d player=%s ai_attack_half", room->id, p->name);
    }
    /* AI 随机判定：攻击等不确定行动也过 d100，失败则效果打折。 */
    if (attack_mode && attack_damage > 0 && heuristic_need_roll(content, op)) {
        roll_chance = compute_roll_chance(3, 3, 3, op, 0);
        roll_chance += difficulty_chance_bonus[room->difficulty];
        if (p->curse_turns > 0) {
            roll_chance -= 3 * 8;
            if (roll_chance < ROLL_MIN) roll_chance = ROLL_MIN;
        }
        if (roll_chance > ROLL_MAX) roll_chance = ROLL_MAX;
        roll_value = roll_d100();
        roll_success = roll_value <= roll_chance ? 1 : 0;
        roll_used = 1;
        snprintf(roll_note, sizeof(roll_note),
                 roll_success ? "行动顺利推进。" : "行动没有完全达到预期。");
        if (!roll_success && attack_damage > 0) {
            attack_damage = attack_damage / 2;
            if (attack_damage < 1) attack_damage = 1;
        }
    }
    /* AI 攻击同样接入叙述质量倍率（与真人无 AI 路径一致），
       保证"言之有物打得更狠"在人和 AI 两端同规则。 */
    if (attack_mode && attack_damage > 0) {
        NumericReason qr = sentence_reasonableness(room, content, op);
        int pct = (qr == NUMERIC_REASON_HIGH) ? 20 : (qr == NUMERIC_REASON_LOW ? -30 : 0);
        if (pct != 0) {
            attack_damage = attack_damage * (100 + pct) / 100;
            if (attack_damage < 1) attack_damage = 1;
        }
    }
    n->damage = attack_mode ? attack_damage : 0;
    n->roll_used = roll_used;
    n->roll_value = roll_value;
    n->roll_chance = roll_chance;
    n->roll_success = roll_success;
    safe_copy(n->roll_note, sizeof(n->roll_note), roll_note);
    room->narrative_count++;
    if (p->next_turn_penalty == 4) {
        char green_line[160];
        add_green_narrative(room, p->id, "恢复了神智清明");
    }
    p->has_spoken_first = 1;
    p->next_turn_penalty = 0;   /* 本回合限制已生效，清除；灾厄可能设置新的下回合限制 */

    if (p->curse_turns > 0) {
        p->curse_turns--;
        if (p->curse_turns == 0) {
            add_green_narrative(room, p->id, "你感觉如释重负");
        }
    }

    if (p->life_curse_turns > 0) {
        p->life_curse_turns--;
        if (p->hp > 1) p->hp--;
        debug_log("[curse] room=%d player=%s life_hp=%d", room->id, p->name, p->hp);
        if (p->life_curse_turns == 0) {
            add_green_narrative(room, p->id, "你感觉身体恢复了活力");
        }
    }

    {
        char history_line[AI_HISTORY_MSG_LEN];
        snprintf(history_line, sizeof(history_line), "%s：%s", p->name, content);
        ai_history_push(p, history_line);
        judge_log_append(room, history_line);
    }

    debug_log("[ai_speak] room=%d ai=%s op=%d target=%d damage=%d content=%s",
              room->id, p->name, op, attack_mode ? attack_target_id : 0,
              attack_mode ? attack_damage : 0, content);

    /* 灾祸值：AI 发言同样按合理度增减，并尝试触发灾祸。 */
    adjust_calamity(p, sentence_reasonableness(room, content, op));
    try_trigger_calamity(room, p);

    /* 伤害结算：AI 攻击先扣血，死亡则不再额外创建濒死警告。 */
    if (attack_mode && attack_target_id > 0 && attack_damage > 0) {
        apply_damage(room, attack_target_id, attack_damage, p->id, 1);
    }

    /* AI 进攻并产生致命威胁时，创建濒死警告。 */
    if (attack_mode && attack_danger && attack_target_id > 0 &&
        room->warning_count < MAX_WARNINGS && room->status == ROOM_PLAYING) {
        int ti = player_by_id(room, attack_target_id);
        if (ti >= 0 && room->players[ti].alive &&
            rand() % 100 < warning_chance_from_hp(room->players[ti].hp)) {
            Warning *w = &room->warnings[room->warning_count];
            memset(w, 0, sizeof(*w));
            w->id = room->warning_count + 1;
            w->victim_id = attack_target_id;
            w->source_id = p->id;
            w->narrative_id = n->id;
            w->resolved = 0;
            if (attack_reason[0]) {
                safe_copy(w->reason, sizeof(w->reason), attack_reason);
            } else {
                safe_copy(w->reason, sizeof(w->reason), content);
            }
            debug_log("[warning_create] room=%d victim=%d source=%d reason=%s",
                      room->id, attack_target_id, p->id, w->reason);
            room->warning_count++;
        }
    }

    /* AI 的非攻击叙述（扭曲/创造）点名了唯一一名存活玩家时，也走与真人一致的
       危险判定——否则会出现"台词说要杀你，机制却什么都没做"的脱节。 */
    if (!attack_mode && (op == OP_TWIST || op == OP_CREATE) &&
        room->status == ROOM_PLAYING && room->warning_count < MAX_WARNINGS) {
        int mtarget = 0;
        int mcnt = 0;
        for (i = 0; i < room->player_count; i++) {
            if (room->players[i].id != p->id && room->players[i].alive &&
                strstr(content, room->players[i].name)) {
                mtarget = room->players[i].id;
                mcnt++;
            }
        }
        if (mcnt == 1 && ai_is_configured() && ai_can_request_now()) {
            int danger = 0;
            char jreason[256] = "";
            int vti = player_by_id(room, mtarget);
            const char *tn = (vti >= 0) ? room->players[vti].name : "";
            if (ai_try_judge_narration(room->name, players_text, p->name,
                                       content, op, tn, &danger,
                                       jreason, sizeof(jreason)) == 1 && danger &&
                vti >= 0 &&
                rand() % 100 < warning_chance_from_hp(room->players[vti].hp)) {
                Warning *w = &room->warnings[room->warning_count];
                memset(w, 0, sizeof(*w));
                w->id = room->warning_count + 1;
                w->victim_id = mtarget;
                w->source_id = p->id;
                w->narrative_id = n->id;
                w->resolved = 0;
                safe_copy(w->reason, sizeof(w->reason), jreason[0] ? jreason : content);
                debug_log("[warning_create] room=%d victim=%d source=%d reason=%s",
                          room->id, mtarget, p->id, w->reason);
                room->warning_count++;
            }
        }
    }

    if (player_has_unresolved_warning(room, p->id)) {
        int rescued = 0;
        int ai_ok = 0;
        char ai_reason[256] = "";
        int wi;

        if (rescue_used) {
            rescued = rescue_success;
            ai_ok = 1;
            if (rescue_reason[0]) {
                safe_copy(ai_reason, sizeof(ai_reason), rescue_reason);
            }
        } else if (ai_is_configured()) {
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
            /* AI 自救也应有失败率，避免“无限完美闪避”导致对局永远无法推进。 */
            static const int rescue_base[] = { 85, 70, 55, 40, 25 };
            int rescue_chance = rescue_base[room->difficulty];
            const char *env_chance = getenv("LUANSHA_AI_RESCUE_CHANCE");
            if (env_chance && *env_chance) {
                int v = atoi(env_chance);
                if (v >= 0 && v <= 100) rescue_chance = v;
            }
            /* 连续成功自救后成功率下降，避免无限秒解。 */
            rescue_chance -= p->rescue_streak * 10;
            if (rescue_chance < 20) rescue_chance = 20;
            if (rand() % 100 < rescue_chance) {
                p->rescue_streak++;
                resolve_victim_warnings(room, p->id);
                apply_heal(room, p, 1);
            } else {
                p->rescue_streak = 0;
            }
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

static int player_by_name(Room *room, const char *name)
{
    int i;
    if (!room || !name) return -1;
    for (i = 0; i < room->player_count; i++) {
        if (strcmp(room->players[i].name, name) == 0) return i;
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
#ifdef _WIN32
    {
        int i;
        for (i = 0; i < MAX_ROOMS; i++) InitializeCriticalSection(&g_rlock[i]);
    }
#endif
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

static int game_create_room_impl(const char *room_name, int mode, int difficulty, const char *player_name, char *token_out)
{
    Room *room;
    int idx;

    if (g_room_count >= MAX_ROOMS) return -1;
    idx = g_room_count++;

    room = &g_rooms[idx];
    memset(room, 0, sizeof(*room));
    room->id = g_next_room_id++;
    room->mode = mode ? MODE_GM : MODE_AUTO;
    if (difficulty < 0) difficulty = 0;
    if (difficulty > 4) difficulty = 4;
    room->difficulty = difficulty;
    room->scene_item_count = 0;
    room->status = ROOM_WAITING;
    safe_copy(room->name, sizeof(room->name), room_name && *room_name ? room_name : "乱杀房间");

    room->players[0].id = 1;
    safe_copy(room->players[0].name, sizeof(room->players[0].name),
              player_name && *player_name ? player_name : "玩家1");
    generate_token(room->players[0].token, sizeof(room->players[0].token));
    room->players[0].alive = 1;
    room->players[0].ready = 0;
    room->players[0].is_ai = 0;
    room->players[0].hp = DEFAULT_MAX_HP;
    room->players[0].max_hp = DEFAULT_MAX_HP;
    room->player_count = 1;
    room->owner_id = 1;
    room->round = 0;
    room->winner_id = 0;

    if (token_out) safe_copy(token_out, TOKEN_LEN + 1, room->players[0].token);
    debug_log("[session] ============ new room ============");
    debug_log("[room] create room_id=%d name=%s owner=%s difficulty=%d", room->id, room->name, room->players[0].name, room->difficulty);
    return room->id;
}

static int game_join_room_impl(int room_id, const char *player_name, char *token_out)
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
    p->hp = DEFAULT_MAX_HP;
    p->max_hp = DEFAULT_MAX_HP;

    if (token_out) safe_copy(token_out, TOKEN_LEN + 1, p->token);
    return room->id;
}

static int game_add_ai_impl(int room_id)
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
    p->hp = DEFAULT_MAX_HP;
    p->max_hp = DEFAULT_MAX_HP;

    return room->id;
}

static int game_set_ready_impl(int room_id, const char *token, int ready)
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
    int i;
    if (room->player_count == 0) return;
    room->turn_index = (room->turn_index + 1) % room->player_count;
    if (room->turn_index == 0) {
        room->round++;
        /* 难度灾厄自动累积：困难+1，噩梦+2，地狱+3 */
        if (room->difficulty >= 1) {
            int gain = room->difficulty;
            for (i = 0; i < room->player_count; i++) {
                if (room->players[i].alive) {
                    room->players[i].calamity += gain;
                }
            }
        }
    }
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

static int game_start_impl(int room_id, const char *token, int fill_ai)
{
    Room *room = game_find_room(room_id);
    Player *p = room ? game_find_player(room, token) : NULL;
    int i, j;

    if (!room || !p) return -1;
    if (room->status != ROOM_WAITING) return -2;
    if (p->id != room->owner_id) return -3;

    if (fill_ai) {
        while (room->player_count < 4) {
            int r = game_add_ai_impl(room_id);
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
    room->scene_item_count = 0;
    room->judge_log[0] = '\0';
    init_scene_items(room);
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

static int game_speak_impl(int room_id, const char *token, int operation, const char *content,
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
    int quality_pct = 0;   /* 叙述质量对伤害的增益（百分比，-50..+50） */
    char roll_note[128] = "";
    char forced_content[MAX_CONTENT];

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

    /* 下回合限制：无法攻击 */
    if (cp->next_turn_penalty == 1) {
        int is_attack = (operation == OP_TWIST && target_id > 0) ||
                        strstr(content, "刺") || strstr(content, "砍") ||
                        strstr(content, "捅") || strstr(content, "射") ||
                        strstr(content, "劈") || strstr(content, "杀");
        if (is_attack) {
            debug_log("[penalty] room=%d player=%s penalty=1 action=no_attack", room->id, cp->name);
            if (error_msg && error_size > 0) snprintf(error_msg, error_size, "你受到灾厄影响，本回合无法攻击");
            return -13;
        }
    }
    /* 下回合限制：无法移动 */
    if (cp->next_turn_penalty == 2 && contains_move_marker(content)) {
        debug_log("[penalty] room=%d player=%s penalty=2 action=no_move", room->id, cp->name);
        if (error_msg && error_size > 0) snprintf(error_msg, error_size, "你受到灾厄影响，本回合无法移动");
        return -14;
    }
    /* 下回合限制：随机行动 → 随机发言 */
    if (cp->next_turn_penalty == 4) {
        snprintf(forced_content, sizeof(forced_content), "%s",
                 random_action_lines[rand() % RANDOM_ACTION_LINES_COUNT]);
        content = forced_content;
        operation = OP_NORMAL;
        debug_log("[penalty] room=%d player=%s penalty=4 action=random", room->id, cp->name);
    }
/* AI strengthened review + random determination, merged into one request. */
    if (ai_is_configured()) {
        char players_text[512] = "";
        char ai_reason[256] = "";
        int valid_reason = 1;
        int valid_predicate = 1;
        int valid_mode = 1;
        int need_roll = 0;
        int difficulty = 3;
        int plausibility = 3;
        int preparation = 3;
        int i;
        int start2;
        char recent_text2[1024] = "";

        for (i = 0; i < room->player_count; i++) {
            if (i) strncat(players_text, ", ", sizeof(players_text) - strlen(players_text) - 1);
            strncat(players_text, room->players[i].name,
                    sizeof(players_text) - strlen(players_text) - 1);
        }
        start2 = room->narrative_count > 3 ? room->narrative_count - 3 : 0;
        for (i = start2; i < room->narrative_count; i++) {
            char line[600];
            int pi2 = player_by_id(room, room->narratives[i].player_id);
            snprintf(line, sizeof(line), "%s：%s\n",
                     pi2 >= 0 ? room->players[pi2].name : "系统",
                     room->narratives[i].content);
            strncat(recent_text2, line, sizeof(recent_text2) - strlen(recent_text2) - 1);
        }

        if (!p->has_spoken_first) {
            need_roll = 0;
        } else if (ai_judge_player_narration(room->name, players_text, p->name,
                                             content, operation, !p->has_spoken_first,
                                             recent_text2,
                                             &valid_reason, &valid_predicate, &valid_mode,
                                             &need_roll, &difficulty, &plausibility,
                                             &preparation,
                                             ai_reason, sizeof(ai_reason)) != 1) {
            need_roll = heuristic_need_roll(content, operation);
        }

        if (!valid_reason || !valid_predicate || !valid_mode) {
            if (error_msg && error_size > 0) {
                snprintf(error_msg, error_size, "%s",
                         ai_reason[0] ? ai_reason : "发言不合理或不符合单一谓语规则");
            }
            return -11;
        }
        /* 首句离家设定：无论 AI 是否放行，第一句必须包含离家动作。 */
        if (!p->has_spoken_first && !contains_home_exit(content)) {
            if (error_msg && error_size > 0) {
                snprintf(error_msg, error_size, "第一句话应该类似“我走出了家门”");
            }
            return -11;
        }

        /* 叙述质量 → 伤害增益：合理性/铺垫直接作用到伤害上，
           让「写得好」不只是过检，而是真的打得更狠。 */
        quality_pct = (plausibility - 3) * DAMAGE_QUALITY_W_PLAUSIBLE +
                      (preparation - 3) * DAMAGE_QUALITY_W_PREPARED;
        if (quality_pct > DAMAGE_QUALITY_MAX) quality_pct = DAMAGE_QUALITY_MAX;
        if (quality_pct < DAMAGE_QUALITY_MIN) quality_pct = DAMAGE_QUALITY_MIN;

        if (need_roll) {
            int luck = p->last_roll_failed ? ROLL_LUCK_BONUS : 0;
            roll_chance = compute_roll_chance(difficulty, plausibility, preparation,
                                              operation, luck);
            roll_chance += difficulty_chance_bonus[room->difficulty];
            if (cp->curse_turns > 0) {
                roll_chance -= difficulty * 8;
                if (roll_chance < ROLL_MIN) roll_chance = ROLL_MIN;
            }
            if (roll_chance > ROLL_MAX) roll_chance = ROLL_MAX;
            roll_value = roll_d100();
            roll_success = roll_value <= roll_chance ? 1 : 0;
            roll_used = 1;
            p->last_roll_failed = roll_success ? 0 : 1;
            snprintf(roll_note, sizeof(roll_note),
                     roll_success ? "行动顺利推进。" : "行动没有完全达到预期。");
        }
    } else if (!p->has_spoken_first && !contains_home_exit(content)) {
        if (error_msg && error_size > 0) {
            snprintf(error_msg, error_size, "第一句话应该类似“我走出了家门”");
        }
        return -11;
    } else {
        /* No AI configured: use heuristic random check only. */
        int need_roll = heuristic_need_roll(content, operation);
        if (need_roll) {
            int luck = p->last_roll_failed ? ROLL_LUCK_BONUS : 0;
            roll_chance = compute_roll_chance(3, 3, 3, operation, luck);
            roll_chance += difficulty_chance_bonus[room->difficulty];
            if (cp->curse_turns > 0) {
                roll_chance -= 3 * 8;
                if (roll_chance < ROLL_MIN) roll_chance = ROLL_MIN;
            }
            if (roll_chance > ROLL_MAX) roll_chance = ROLL_MAX;
            roll_value = roll_d100();
            roll_success = roll_value <= roll_chance ? 1 : 0;
            roll_used = 1;
            p->last_roll_failed = roll_success ? 0 : 1;
            snprintf(roll_note, sizeof(roll_note),
                     roll_success ? "行动顺利推进。" : "行动没有完全达到预期。");
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
    n->damage = heuristic_damage(content, operation);

    /* 叙述质量 → 伤害倍率。
       有 AI 裁判时，quality_pct 已由合理性/铺垫算出；
       无 AI 时退回启发式：言之有物 +20%，空泛 -30%。 */
    if (!ai_is_configured()) {
        NumericReason qr = sentence_reasonableness(room, content, operation);
        quality_pct = (qr == NUMERIC_REASON_HIGH) ? 20
                    : (qr == NUMERIC_REASON_LOW ? -30 : 0);
    }
    if (n->damage > 0 && quality_pct != 0) {
        int qd = n->damage * (100 + quality_pct) / 100;
        if (qd < 1) qd = 1;
        n->damage = qd;
    }

    n->roll_used = roll_used;
    n->roll_value = roll_value;
    n->roll_chance = roll_chance;
    n->roll_success = roll_success;
    safe_copy(n->roll_note, sizeof(n->roll_note), roll_note);
    room->narrative_count++;
    if (cp->next_turn_penalty == 4) {
        add_green_narrative(room, p->id, "恢复了神智清明");
    }
    p->has_spoken_first = 1;
    p->next_turn_penalty = 0;   /* 本回合限制已生效，清除；灾厄可能设置新的下回合限制 */

    if (p->curse_turns > 0) {
        p->curse_turns--;
        if (p->curse_turns == 0) {
            add_green_narrative(room, p->id, "你感觉如释重负");
        }
    }

    if (p->life_curse_turns > 0) {
        p->life_curse_turns--;
        if (p->hp > 1) p->hp--;
        debug_log("[curse] room=%d player=%s life_hp=%d", room->id, p->name, p->hp);
        if (p->life_curse_turns == 0) {
            add_green_narrative(room, p->id, "你感觉身体恢复了活力");
        }
    }

    {
        char judge_line[AI_HISTORY_MSG_LEN];
        snprintf(judge_line, sizeof(judge_line), "%s：%s", p->name, content);
        judge_log_append(room, judge_line);
    }

    debug_log("[speak] room=%d player=%s op=%d target=%d content=%s",
              room->id, p->name, operation, target_id, content);

    /* 灾祸值：根据发言合理度增减，并尝试触发灾祸。 */
    adjust_calamity(p, sentence_reasonableness(room, content, operation));
    try_trigger_calamity(room, p);

    /* 随机判定失败改为“效果打折”：伤害减半（至少保留1点），不跳过回合。 */
    if (roll_used && !roll_success) {
        if (n->damage > 0) {
            int half = n->damage / 2;
            n->damage = half > 0 ? half : 1;
        }
    }

    /* 伤害结算：成功且指定目标时扣血；目标死亡则后续不再创建濒死警告。 */
    if (target_id > 0 && n->damage > 0) {
        apply_damage(room, target_id, n->damage, p->id, 1);
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

        /* 目标已死亡时不再创建濒死警告。 */
        if (warn_target_id > 0) {
            int dead_ti = player_by_id(room, warn_target_id);
            if (dead_ti < 0 || !room->players[dead_ti].alive) warn_target_id = 0;
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
            /* HP 抵抗：威胁判定只在概率上转化为濒死警告，血量越高越抗。 */
            if (should_warn && warn_target_id > 0) {
                int vti = player_by_id(room, warn_target_id);
                if (vti >= 0 &&
                    rand() % 100 >= warning_chance_from_hp(room->players[vti].hp)) {
                    should_warn = 0;
                }
            }
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
                debug_log("[warning_create] room=%d victim=%d source=%d reason=%s",
                          room->id, warn_target_id, p->id, w->reason);
                room->warning_count++;
            }
        } else {
            int heuristic_danger = operation == OP_TWIST && warn_target_id > 0 &&
                                   player_by_id(room, warn_target_id) >= 0 &&
                                   contains_kill_marker(content);
            /* HP 抵抗：与 AI 判定路径一致，按目标血量概率化。 */
            if (heuristic_danger && warn_target_id > 0) {
                int vti = player_by_id(room, warn_target_id);
                if (vti >= 0 &&
                    rand() % 100 >= warning_chance_from_hp(room->players[vti].hp)) {
                    heuristic_danger = 0;
                }
            }
            if (heuristic_danger && warn_target_id > 0 && room->warning_count < MAX_WARNINGS) {
                Warning *w = &room->warnings[room->warning_count];
                memset(w, 0, sizeof(*w));
                w->id = room->warning_count + 1;
                w->victim_id = warn_target_id;
                w->source_id = p->id;
                w->narrative_id = n->id;
                w->resolved = 0;
                snprintf(w->reason, sizeof(w->reason), "%s", content);
                debug_log("[warning_create] room=%d victim=%d source=%d reason=%s",
                          room->id, warn_target_id, p->id, w->reason);
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
            apply_heal(room, p, 1);
        }
    }

    /* Advance to the next living player. AI turns are processed lazily on state polls. */
    if (room->status == ROOM_PLAYING) advance_to_next_alive(room);

    return 0;
}

static void ensure_records_dir(void)
{
#ifdef _WIN32
    _mkdir("records");
#else
    mkdir("records", 0755);
#endif
}

/* 对局结束后把完整时间线保存为本地回放文件（records/room_<id>.json）。 */
static void save_room_record(Room *room)
{
    char path[128];
    char timebuf[64];
    time_t now;
    struct tm *tm_now;
    JsonBuf b;
    FILE *f;
    int i;

    if (!room) return;
    ensure_records_dir();
    snprintf(path, sizeof(path), "records/room_%d.json", room->id);

    now = time(NULL);
    tm_now = localtime(&now);
    if (tm_now) strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm_now);
    else safe_copy(timebuf, sizeof(timebuf), "");

    jsonb_init(&b);
    jsonb_appendf(&b, "{\"record_id\":%d,", room->id);
    jsonb_appendf(&b, "\"room_id\":%d,", room->id);
    jsonb_append(&b, "\"name\":");
    jsonb_string(&b, room->name);
    jsonb_appendf(&b, ",\"mode\":%d,", room->mode);
    jsonb_appendf(&b, "\"difficulty\":%d,", room->difficulty);
    jsonb_appendf(&b, "\"round\":%d,", room->round);
    jsonb_appendf(&b, "\"winner_id\":%d,", room->winner_id);
    jsonb_append(&b, "\"created_at\":");
    jsonb_string(&b, timebuf);
    jsonb_append(&b, ",");

    jsonb_append(&b, "\"players\":[");
    for (i = 0; i < room->player_count; i++) {
        Player *p = &room->players[i];
        if (i) jsonb_append(&b, ",");
        jsonb_append(&b, "{");
        jsonb_appendf(&b, "\"id\":%d,", p->id);
        jsonb_append(&b, "\"name\":");
        jsonb_string(&b, p->name);
        jsonb_append(&b, ",");
        jsonb_appendf(&b, "\"is_ai\":%s,", p->is_ai ? "true" : "false");
        jsonb_appendf(&b, "\"alive\":%s,", p->alive ? "true" : "false");
        jsonb_appendf(&b, "\"hp\":%d,", p->hp);
        jsonb_appendf(&b, "\"max_hp\":%d", p->max_hp);
        jsonb_append(&b, "}");
    }
    jsonb_append(&b, "],");

    jsonb_append(&b, "\"narratives\":[");
    for (i = 0; i < room->narrative_count; i++) {
        Narrative *n = &room->narratives[i];
        int pi = player_by_id(room, n->player_id);
        if (i) jsonb_append(&b, ",");
        jsonb_append(&b, "{");
        jsonb_appendf(&b, "\"id\":%d,", n->id);
        jsonb_appendf(&b, "\"player_id\":%d,", n->player_id);
        jsonb_append(&b, "\"player_name\":");
        if (pi >= 0) jsonb_string(&b, room->players[pi].name);
        else jsonb_string(&b, "系统");
        jsonb_append(&b, ",");
        jsonb_appendf(&b, "\"round\":%d,", n->round);
        jsonb_append(&b, "\"operation\":");
        jsonb_string(&b, game_op_name(n->operation));
        jsonb_append(&b, ",");
        jsonb_append(&b, "\"content\":");
        jsonb_string(&b, n->content);
        jsonb_append(&b, ",");
        jsonb_append(&b, "\"limit_keyword\":");
        jsonb_string(&b, n->limit_keyword);
        jsonb_appendf(&b, ",\"target_id\":%d", n->target_id);
        jsonb_appendf(&b, ",\"damage\":%d", n->damage);
        jsonb_appendf(&b, ",\"calamity\":%s", n->calamity ? "true" : "false");
        jsonb_appendf(&b, ",\"notice\":%s", n->notice ? "true" : "false");
        jsonb_appendf(&b, ",\"green\":%s", n->green ? "true" : "false");
        jsonb_appendf(&b, ",\"roll_used\":%s", n->roll_used ? "true" : "false");
        jsonb_appendf(&b, ",\"roll_value\":%d", n->roll_value);
        jsonb_appendf(&b, ",\"roll_chance\":%d", n->roll_chance);
        jsonb_appendf(&b, ",\"roll_success\":%s", n->roll_success ? "true" : "false");
        jsonb_append(&b, ",\"roll_note\":");
        jsonb_string(&b, n->roll_note);
        jsonb_append(&b, "}");
    }
    jsonb_append(&b, "],");

    jsonb_append(&b, "\"warnings\":[");
    for (i = 0; i < room->warning_count; i++) {
        Warning *w = &room->warnings[i];
        if (i) jsonb_append(&b, ",");
        jsonb_append(&b, "{");
        jsonb_appendf(&b, "\"id\":%d,", w->id);
        jsonb_appendf(&b, "\"victim_id\":%d,", w->victim_id);
        jsonb_appendf(&b, "\"source_id\":%d,", w->source_id);
        jsonb_appendf(&b, "\"narrative_id\":%d,", w->narrative_id);
        jsonb_appendf(&b, "\"resolved\":%s,", w->resolved ? "true" : "false");
        jsonb_append(&b, "\"reason\":");
        jsonb_string(&b, w->reason);
        jsonb_append(&b, "}");
    }
    jsonb_append(&b, "]");

    jsonb_append(&b, "}");

    f = fopen(path, "wb");
    if (f) {
        fputs(jsonb_cstr(&b), f);
        fclose(f);
        debug_log("[record] saved room=%d path=%s", room->id, path);
    }
    jsonb_free(&b);
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

        save_room_record(room);

        /* 独立审核对话：整场结束后交给审核AI。 */
        if (ai_is_configured()) {
            char players_text[512] = "";
            char review_out[1024] = "";
            for (i = 0; i < room->player_count; i++) {
                if (i) strncat(players_text, ", ", sizeof(players_text) - strlen(players_text) - 1);
                strncat(players_text, room->players[i].name,
                        sizeof(players_text) - strlen(players_text) - 1);
            }
            if (ai_review_game(room->name, players_text, room->judge_log,
                               review_out, sizeof(review_out))) {
                debug_log("[review] room=%d result=%s", room->id, review_out);
            }
        }
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

static void apply_damage(Room *room, int target_id, int damage, int source_id,
                         int create_warning)
{
    int ti;
    Player *target;
    NumericReason reason;
    char ctx[256];
    int final_hp;
    int status;

    if (!room || damage <= 0 || target_id <= 0) return;
    ti = player_by_id(room, target_id);
    if (ti < 0) return;
    target = &room->players[ti];
    if (!target->alive) return;

    reason = numeric_reason_for_damage(damage, target->hp);
    snprintf(ctx, sizeof(ctx), "damage:%d:%d:%d:%d:%d",
             room->id, source_id, target_id, damage, target->hp);
    status = numeric_review("damage", ctx, reason, target->hp,
                            target->hp - damage, room->difficulty,
                            room->id, room->round, &final_hp);
    target->hp = final_hp;
    target->numeric_status = status;

    if (target->hp <= 0) {
        /* 任何伤害都不能直接致死：先进入濒死，生命保留 1。 */
        target->hp = 1;
    }

    debug_log("[damage] room=%d target=%s source=%d damage=%d hp=%d/%d status=%s",
              room->id, target->name, source_id, damage, target->hp, target->max_hp,
              status ? "adjusted" : "applied");

    if (create_warning && target->hp <= 1 && room->warning_count < MAX_WARNINGS &&
        !player_has_unresolved_warning(room, target_id)) {
        /* 生命过低时自动进入濒死状态，死亡前必须存在濒死警告。 */
        Warning *w = &room->warnings[room->warning_count];
        memset(w, 0, sizeof(*w));
        w->id = room->warning_count + 1;
        w->victim_id = target_id;
        w->source_id = source_id;
        w->narrative_id = 0;
        w->resolved = 0;
        snprintf(w->reason, sizeof(w->reason), "你感到体力正在快速流失，视线逐渐模糊");
        debug_log("[warning_create] room=%d victim=%d source=%d reason=%s",
                  room->id, target_id, source_id, w->reason);
        room->warning_count++;
    }
}

static void ensure_warning_before_death(Room *room, int victim_id, int source_id)
{
    if (!room || victim_id <= 0) return;
    if (room->warning_count >= MAX_WARNINGS) return;
    if (player_has_unresolved_warning(room, victim_id)) return;

    {
        Warning *w = &room->warnings[room->warning_count];
        memset(w, 0, sizeof(*w));
        w->id = room->warning_count + 1;
        w->victim_id = victim_id;
        w->source_id = source_id;
        w->narrative_id = 0;
        w->resolved = 0;
        snprintf(w->reason, sizeof(w->reason), "你已无力再战，只能任人处置");
        debug_log("[warning_create] room=%d victim=%d source=%d reason=%s",
                  room->id, victim_id, source_id, w->reason);
        room->warning_count++;
    }
}

static void apply_heal(Room *room, Player *p, int amount)
{
    NumericReason reason;
    char ctx[256];
    int final_hp;
    int status;

    if (!room || !p || amount <= 0 || p->hp <= 0 || p->hp >= p->max_hp) return;
    reason = numeric_reason_for_heal(p->hp, p->max_hp);
    snprintf(ctx, sizeof(ctx), "heal:%d:%d:%d:%d", room->id, p->id, amount, p->hp);
    status = numeric_review("heal", ctx, reason, p->hp, p->hp + amount,
                            room->difficulty, room->id, room->round, &final_hp);
    if (final_hp > p->max_hp) final_hp = p->max_hp;
    p->hp = final_hp;
    p->numeric_status = status;
}

static int ai_try_declare_death(Room *room)
{
    Player *p;
    int i;
    int candidates[MAX_WARNINGS];
    int cc = 0;
    static const int death_base[] = { 40, 60, 70, 80, 90 };
    int death_chance = death_base[room->difficulty];
    const char *env_chance = getenv("LUANSHA_AI_DEATH_CHANCE");

    if (!room || room->status != ROOM_PLAYING || room->player_count == 0) return 0;
    if (room->turn_index < 0 || room->turn_index >= room->player_count) return 0;
    p = &room->players[room->order[room->turn_index]];
    if (!p->is_ai || !p->alive) return 0;

    /* 自己还有未解除的濒死警告时，先自救，不急着宣告别人死亡。 */
    if (player_has_unresolved_warning(room, p->id)) return 0;

    /* 候选1：自己造成的未解除濒死警告目标。 */
    for (i = 0; i < room->warning_count; i++) {
        Warning *w = &room->warnings[i];
        int vi;
        if (w->source_id != p->id || w->resolved) continue;
        vi = player_by_id(room, w->victim_id);
        if (vi >= 0 && room->players[vi].alive && room->players[vi].id != p->id) {
            candidates[cc++] = room->players[vi].id;
        }
    }
    /* 濒死警告只能由触发者宣告死亡，不允许 HP≤1 补刀。 */
    if (cc == 0) return 0;

    if (env_chance && *env_chance) {
        int v = atoi(env_chance);
        if (v >= 0 && v <= 100) death_chance = v;
    }

    /* 合适的时机：AI 有概率宣告目标死亡，而不是每回合必杀。 */
    if (rand() % 100 < death_chance) {
        int pick = candidates[rand() % cc];
        int vi = player_by_id(room, pick);
        if (vi >= 0) {
            int j, has_own_warning = 0;
            /* 二次校验：必须确实存在自己造成的未解除濒死警告。 */
            for (j = 0; j < room->warning_count; j++) {
                Warning *w = &room->warnings[j];
                if (w->source_id == p->id && w->victim_id == pick && !w->resolved) {
                    has_own_warning = 1;
                    break;
                }
            }
            if (has_own_warning) {
                ensure_warning_before_death(room, pick, p->id);
                room->players[vi].alive = 0;
                announce_death(room, pick);
                debug_log("[death] room=%d source=%s victim=%s by=ai_declare reason=warning",
                          room->id, p->name, room->players[vi].name);
                finish_if_one_alive(room);
                if (room->status == ROOM_PLAYING) advance_to_next_alive(room);
                return 1;
            }
        }
    }
    return 0;
}

static int game_declare_death_impl(int room_id, const char *token, int target_id)
{
    Room *room = game_find_room(room_id);
    Player *p = room ? game_find_player(room, token) : NULL;
    int ti;
    int i;
    int found_unresolved = 0;

    if (!room || !p) return -1;
    if (room->status != ROOM_PLAYING) return -2;

    if (target_id == p->id) {
        int is_my_turn = room->player_count > 0 &&
                         room->players[room->order[room->turn_index]].id == p->id;
        if (!p->alive) return -7;
        ensure_warning_before_death(room, p->id, p->id);
        p->alive = 0;
        announce_death(room, p->id);
        debug_log("[death] room=%d source=%s victim=%s by=suicide", room->id, p->name, p->name);
        finish_if_one_alive(room);
        /* 只有自己回合自杀才推进；非自己回合自杀不应跳过当前回合者。 */
        if (room->status == ROOM_PLAYING && is_my_turn) advance_to_next_alive(room);
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
    /* 濒死警告只能由触发者宣告死亡，不允许仅凭 HP≤1 补刀。 */
    if (!found_unresolved) return -5;
    if (!room->players[ti].alive) return -6;

    ensure_warning_before_death(room, target_id, p->id);
    room->players[ti].alive = 0;
    announce_death(room, target_id);
    debug_log("[death] room=%d source=%s victim=%s by=declare", room->id, p->name, room->players[ti].name);
    finish_if_one_alive(room);
    if (room->status == ROOM_PLAYING) advance_to_next_alive(room);
    return 0;
}

static int game_gm_command_impl(int room_id, const char *token, const char *action, int target_id)
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
        ensure_warning_before_death(room, target_id, p->id);
        room->players[ti].alive = 0;
        announce_death(room, target_id);
        debug_log("[death] room=%d source=%s victim=%s by=gm", room->id, p->name, room->players[ti].name);
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

static int game_room_to_json_ex_impl(int room_id, const char *token, JsonBuf *out, int process_ai)
{
    Room *room = game_find_room(room_id);
    int i;

    if (!room) return -1;
    if (process_ai) {
        /* 玩家“跳过下回合”在状态轮询时自动生效。 */
        if (room->player_count > 0 && room->turn_index >= 0 &&
            room->turn_index < room->player_count) {
            Player *cp = &room->players[room->order[room->turn_index]];
            if (!cp->is_ai && cp->alive && cp->next_turn_penalty == 3) {
                cp->next_turn_penalty = 0;
                debug_log("[penalty] room=%d player=%s penalty=3 action=skip_turn", room->id, cp->name);
                advance_to_next_alive(room);
            }
        }
        /* AI 动作改由后台 worker 处理：这里只把房间标记为待办，
           轮询请求线程不再承担任何 LLM 调用（避免拖慢全服）。 */
        if (room->status == ROOM_PLAYING) ai_worker_kick(room);
    }

    jsonb_append(out, "{");
    jsonb_appendf(out, "\"ok\":true,");
    jsonb_appendf(out, "\"room_id\":%d,", room->id);
    jsonb_append(out, "\"name\":");
    jsonb_string(out, room->name);
    jsonb_append(out, ",");
    jsonb_appendf(out, "\"status\":%d,", room->status);
    jsonb_appendf(out, "\"mode\":%d,", room->mode);
    jsonb_appendf(out, "\"difficulty\":%d,", room->difficulty);
    jsonb_appendf(out, "\"round\":%d,", room->round);
    jsonb_appendf(out, "\"turn_index\":%d,", room->turn_index);
    jsonb_appendf(out, "\"turn_player_id\":%d,", room->player_count > 0 ? room->players[room->order[room->turn_index]].id : 0);
    jsonb_appendf(out, "\"winner_id\":%d,", room->winner_id);
    jsonb_appendf(out, "\"owner_id\":%d,", room->owner_id);

    jsonb_append(out, "\"players\":[");
    for (i = 0; i < room->player_count; i++) {
        Player *pl = &room->players[i];
        int is_me = token && strcmp(pl->token, token) == 0;
        /* 灾祸对外只暴露档位（平静/躁动/危险），自己才看得到确切数值。 */
        int ct = pl->calamity >= CALAMITY_TIER_DANGER ? 2
               : (pl->calamity >= CALAMITY_TIER_UNEASY ? 1 : 0);
        if (i) jsonb_append(out, ",");
        jsonb_append(out, "{");
        jsonb_appendf(out, "\"id\":%d,", pl->id);
        jsonb_append(out, "\"name\":");
        jsonb_string(out, pl->name);
        jsonb_append(out, ",");
        jsonb_appendf(out, "\"alive\":%s,", pl->alive ? "true" : "false");
        jsonb_appendf(out, "\"ready\":%s,", pl->ready ? "true" : "false");
        jsonb_appendf(out, "\"is_ai\":%s,", pl->is_ai ? "true" : "false");
        jsonb_appendf(out, "\"hp\":%d,", pl->hp);
        jsonb_appendf(out, "\"max_hp\":%d,", pl->max_hp);
        jsonb_appendf(out, "\"calamity_tier\":%d,", ct);
        jsonb_appendf(out, "\"calamity\":%d,", is_me ? pl->calamity : -1);
        jsonb_append(out, "\"numeric_status\":");
        jsonb_string(out, pl->numeric_status ? "adjusted" : "applied");
        jsonb_append(out, ",");
        jsonb_appendf(out, "\"is_me\":%s", is_me ? "true" : "false");
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
            jsonb_appendf(out, ",\"damage\":%d", n->damage);
            jsonb_appendf(out, ",\"calamity\":%s", n->calamity ? "true" : "false");
            jsonb_appendf(out, ",\"notice\":%s", n->notice ? "true" : "false");
            jsonb_appendf(out, ",\"green\":%s", n->green ? "true" : "false");
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
/* ------------------------------------------------------------------ */
/* 公开接口包装：与后台 worker 按房间粒度互斥。                          */
/* 只有对局中（PLAYING）房间的接口需要加锁；建房/加入/准备等都在         */
/* WAITING 阶段，worker 从不触碰。                                      */
/* ------------------------------------------------------------------ */
int game_create_room(const char *room_name, int mode, int difficulty,
                     const char *player_name, char *token_out)
{
    return game_create_room_impl(room_name, mode, difficulty, player_name, token_out);
}

int game_join_room(int room_id, const char *player_name, char *token_out)
{
    return game_join_room_impl(room_id, player_name, token_out);
}

int game_add_ai(int room_id)
{
    return game_add_ai_impl(room_id);
}

int game_set_ready(int room_id, const char *token, int ready)
{
    return game_set_ready_impl(room_id, token, ready);
}

int game_start(int room_id, const char *token, int fill_ai)
{
    Room *room = game_find_room(room_id);
    int r;
    if (!room) return -1;
    room_lock(room);
    r = game_start_impl(room_id, token, fill_ai);
    if (r == 0 && room->status == ROOM_PLAYING) ai_worker_kick(room);
    room_unlock(room);
    return r;
}

int game_speak(int room_id, const char *token, int operation, const char *content,
               const char *limit_keyword, int target_id,
               char *error_msg, size_t error_size)
{
    Room *room = game_find_room(room_id);
    int r;
    if (error_msg && error_size > 0) error_msg[0] = '\0';
    if (!room) return -1;
    room_lock(room);
    r = game_speak_impl(room_id, token, operation, content,
                        limit_keyword, target_id, error_msg, error_size);
    if (r == 0 && room->status == ROOM_PLAYING) ai_worker_kick(room);
    room_unlock(room);
    return r;
}

int game_declare_death(int room_id, const char *token, int target_id)
{
    Room *room = game_find_room(room_id);
    int r;
    if (!room) return -1;
    room_lock(room);
    r = game_declare_death_impl(room_id, token, target_id);
    if (r == 0 && room->status == ROOM_PLAYING) ai_worker_kick(room);
    room_unlock(room);
    return r;
}

int game_gm_command(int room_id, const char *token, const char *action, int target_id)
{
    Room *room = game_find_room(room_id);
    int r;
    if (!room) return -1;
    room_lock(room);
    r = game_gm_command_impl(room_id, token, action, target_id);
    if (r == 0 && room->status == ROOM_PLAYING) ai_worker_kick(room);
    room_unlock(room);
    return r;
}

int game_room_to_json_ex(int room_id, const char *token, JsonBuf *out, int process_ai)
{
    Room *room = game_find_room(room_id);
    int r;
    if (!room) return -1;
    room_lock(room);
    r = game_room_to_json_ex_impl(room_id, token, out, process_ai);
    room_unlock(room);
    return r;
}

int game_room_to_json(int room_id, const char *token, JsonBuf *out)
{
    Room *room = game_find_room(room_id);
    int r;
    if (!room) return -1;
    room_lock(room);
    r = game_room_to_json_ex_impl(room_id, token, out, 1);
    room_unlock(room);
    return r;
}