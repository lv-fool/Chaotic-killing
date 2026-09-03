#ifndef AI_H
#define AI_H

#include <stddef.h>

/*
 * Try to use an OpenAI-compatible AI service to judge whether a narration
 * creates a near-death / fatal threat to any player.
 *
 * Returns:
 *   1 - AI successfully made a judgment (look at *creates_warning)
 *   0 - AI is not configured or call failed; caller should fall back to heuristics
 */
int ai_is_configured(void);
int ai_test_connection(char *out, size_t out_size);

/*
 * Runtime AI configuration (Phase 0.5).
 * ai_configure() stores the config and saves it to ./luansha_ai.conf.
 * ai_init() loads that file on startup; if no file is found, env vars are used.
 * The getters return the runtime config when available, otherwise env vars.
 */
void ai_init(void);
void ai_configure(const char *url, const char *model, const char *key);
const char *ai_get_url(void);
const char *ai_get_model(void);
const char *ai_get_key(void);

/*
 * Validate a human narration for reasonableness and single-predicate rule.
 * Returns 1 if AI made a judgment, 0 if unavailable/failed.
 */
int ai_try_judge_rescue(
    const char *room_name,
    const char *players_text,
    const char *narrator,
    const char *content,
    const char *warning_text,
    int *rescued,
    char *reason,
    size_t reason_size);

int ai_validate_narration(
    const char *room_name,
    const char *players_text,
    const char *narrator,
    const char *content,
    int operation,
    int is_first_sentence,
    int *valid_reason,
    int *valid_predicate,
    int *valid_mode,
    char *reason,
    size_t reason_size);

/*
 * Ask the reviewer whether this narration needs a random (dice) check,
 * and how hard it is. Returns 1 if the AI answered, 0 otherwise.
 *
 * need_roll    : 1 if a random determination is required
 * difficulty   : 1 (very easy) .. 5 (near impossible)
 * plausibility : 1 (far-fetched) .. 5 (very plausible)
 * preparation  : 1 (no setup at all) .. 5 (well prepared by earlier narration)
 */
int ai_judge_random_check(
    const char *room_name,
    const char *players_text,
    const char *narrator,
    const char *content,
    int operation,
    const char *recent_text,
    int *need_roll,
    int *difficulty,
    int *plausibility,
    int *preparation,
    char *reason,
    size_t reason_size);

int ai_try_judge_narration(
    const char *room_name,
    const char *players_text,
    const char *narrator,
    const char *content,
    int operation,
    const char *target_name,
    int *creates_warning,
    char *reason,
    size_t reason_size);

/*
 * Try to generate an AI player's one-sentence narration.
 * Returns 1 on success and fills out; returns 0 to let caller use fallback.
 */
int ai_try_generate_narration(
    const char *room_name,
    const char *players_text,
    const char *ai_name,
    const char *recent_text,
    const char *constraint,
    char *out,
    size_t out_size);

/*
 * Generate a context-appropriate lethal attack sentence for a player against
 * a target. Returns 1 on success and fills out, 0 otherwise.
 */
int ai_try_generate_lethal(
    const char *room_name,
    const char *players_text,
    const char *narrator,
    const char *target_name,
    const char *recent_text,
    char *out,
    size_t out_size);

#endif /* AI_H */