/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: bounded, explainable goal-to-symbol selection for ZCODE work. */

#include "services/zcode_goal_context_service.h"

#include "platform/time_compat.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZGOAL_HITS_PER_TOKEN 16

static bool zgoal_stopword(const char *word)
{
    static const char *const words[] = {
        "a", "add", "an", "and", "ensure", "fix", "make", "or",
        "repair", "the", "to", "with", "without",
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        if (strcmp(word, words[i]) == 0) return true;
    return false;
}

static void zgoal_stem(char *word)
{
    size_t n = strlen(word);
    if (n > 5 && strcmp(word + n - 3u, "ing") == 0)
        word[n - 3u] = '\0';
    else if (n > 4 && strcmp(word + n - 2u, "ed") == 0)
        word[n - 2u] = '\0';
    else if (n > 4 && word[n - 1u] == 's')
        word[n - 1u] = '\0';
}

static size_t zgoal_tokenize(const char *goal,
                             char out[ZCODE_GOAL_MAX_TOKENS]
                                     [ZCODE_GOAL_TOKEN_MAX + 1u],
                             bool *exhausted)
{
    size_t count = 0, used = 0;
    char word[ZCODE_GOAL_TOKEN_MAX + 1u];
    *exhausted = false;
    for (const unsigned char *p = (const unsigned char *)goal;; p++) {
        if (isalnum(*p) || *p == '_') {
            if (used < ZCODE_GOAL_TOKEN_MAX)
                word[used++] = (char)tolower(*p);
            else
                *exhausted = true;
            continue;
        }
        if (used != 0) {
            word[used] = '\0';
            zgoal_stem(word);
            bool duplicate = false;
            for (size_t i = 0; i < count; i++)
                if (strcmp(out[i], word) == 0) duplicate = true;
            if (!zgoal_stopword(word) && !duplicate && word[0]) {
                if (count < ZCODE_GOAL_MAX_TOKENS)
                    (void)snprintf(out[count++],
                                   ZCODE_GOAL_TOKEN_MAX + 1u, "%s", word);
                else
                    *exhausted = true;
            }
            used = 0;
        }
        if (*p == '\0') break;
    }
    return count;
}

static void zgoal_why(uint32_t mask, char out[64])
{
    out[0] = '\0';
    static const struct { uint32_t bit; const char *name; } fields[] = {
        { CI_SEARCH_MATCH_NAME, "name" },
        { CI_SEARCH_MATCH_SIGNATURE, "signature" },
        { CI_SEARCH_MATCH_PATH, "path" },
        { CI_SEARCH_MATCH_DOC, "documentation" },
    };
    size_t used = 0;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (!(mask & fields[i].bit)) continue;
        int n = snprintf(out + used, 64u - used, "%s%s",
                         used ? "+" : "", fields[i].name);
        if (n < 0 || (size_t)n >= 64u - used) break;
        used += (size_t)n;
    }
}

static bool zgoal_same(const struct zcode_goal_candidate *candidate,
                       const struct ci_symbol *symbol)
{
    return strcmp(candidate->symbol.name, symbol->name) == 0 &&
           strcmp(candidate->symbol.def_path, symbol->def_path) == 0 &&
           strcmp(candidate->symbol.decl_path, symbol->decl_path) == 0;
}

static int zgoal_candidate_cmp(const void *a, const void *b)
{
    const struct zcode_goal_candidate *ca = a, *cb = b;
    if (ca->score != cb->score) return ca->score > cb->score ? -1 : 1;
    int by_name = strcmp(ca->symbol.name, cb->symbol.name);
    if (by_name) return by_name;
    int by_def = strcmp(ca->symbol.def_path, cb->symbol.def_path);
    return by_def ? by_def : strcmp(ca->symbol.decl_path,
                                    cb->symbol.decl_path);
}

static bool zgoal_add(struct zcode_goal_selection *out,
                      const struct ci_search_hit *hit, const char *token)
{
    out->total_matches++;
    for (size_t i = 0; i < out->candidate_count; i++) {
        if (!zgoal_same(&out->candidates[i], &hit->symbol)) continue;
        out->candidates[i].match_mask |= hit->match_mask;
        if (hit->score + 25 > out->candidates[i].score) {
            out->candidates[i].score = hit->score + 25;
            (void)snprintf(out->candidates[i].matched_token,
                           sizeof(out->candidates[i].matched_token), "%s",
                           token);
        }
        zgoal_why(out->candidates[i].match_mask,
                  out->candidates[i].why);
        return true;
    }
    if (out->candidate_count >= ZCODE_GOAL_MAX_CANDIDATES) {
        out->dropped_candidates++;
        out->budget_exhausted = true;
        return true;
    }
    struct zcode_goal_candidate *candidate =
        &out->candidates[out->candidate_count++];
    candidate->symbol = hit->symbol;
    candidate->match_mask = hit->match_mask;
    candidate->score = hit->score;
    (void)snprintf(candidate->matched_token,
                   sizeof(candidate->matched_token), "%s", token);
    zgoal_why(candidate->match_mask, candidate->why);
    return codeindex_symbol_record_id(&candidate->symbol,
                                      candidate->symbol_id,
                                      sizeof(candidate->symbol_id)) >= 0;
}

static struct zcl_result zgoal_exact(
    struct codeindex *index, const char *exact,
    struct zcode_goal_selection *out)
{
    bool found = false;
    bool ok = strchr(exact, ':')
        ? codeindex_symbol_by_id(index, exact, &out->selected, &found)
        : codeindex_symbol(index, exact, &out->selected, &found);
    if (!ok || !found)
        return ZCL_ERR(-1, "exact context symbol is not indexed: %s", exact);
    if (codeindex_symbol_record_id(&out->selected,
                                   out->selected_symbol_id,
                                   sizeof(out->selected_symbol_id)) < 0)
        return ZCL_ERR(-1, "exact context symbol has no stable identity");
    (void)snprintf(out->why, sizeof(out->why), "exact_symbol_override");
    return ZCL_OK;
}

struct zcl_result zcode_goal_context_select(
    const char *workspace, const char *goal, const char *exact_symbol,
    struct zcode_goal_selection *out)
{
    if (!workspace || !goal || !goal[0] || !out || strlen(goal) > 4096)
        return ZCL_ERR(-1, "goal selection requires workspace and bounded goal");
    memset(out, 0, sizeof(*out));
    int64_t started = platform_time_monotonic_us();
    struct codeindex *index = codeindex_open(workspace);
    if (!index) return ZCL_ERR(-1, "code index could not open for goal selection");
    struct zcl_result result = ZCL_OK;
    if (exact_symbol && exact_symbol[0]) {
        result = zgoal_exact(index, exact_symbol, out);
    } else {
        out->token_count = zgoal_tokenize(goal, out->tokens,
                                          &out->budget_exhausted);
        for (size_t i = 0; i < out->token_count && result.ok; i++) {
            struct ci_search_hit hits[ZGOAL_HITS_PER_TOKEN];
            int count = codeindex_search_text(index, out->tokens[i], hits,
                                              ZGOAL_HITS_PER_TOKEN);
            if (count < 0) {
                result = ZCL_ERR(-1, "indexed goal search failed for '%s'",
                                 out->tokens[i]);
                break;
            }
            for (int j = 0; j < count; j++) {
                if (!zgoal_add(out, &hits[j], out->tokens[i])) {
                    result = ZCL_ERR(-1, "selected symbol identity failed");
                    break;
                }
            }
        }
        if (result.ok && out->candidate_count == 0)
            result = ZCL_ERR(-1, "goal did not match an indexed symbol");
        if (result.ok) {
            qsort(out->candidates, out->candidate_count,
                  sizeof(out->candidates[0]), zgoal_candidate_cmp);
            out->selected = out->candidates[0].symbol;
            (void)snprintf(out->selected_symbol_id,
                           sizeof(out->selected_symbol_id), "%s",
                           out->candidates[0].symbol_id);
            (void)snprintf(out->why, sizeof(out->why), "%s:%s",
                           out->candidates[0].matched_token,
                           out->candidates[0].why);
        }
    }
    codeindex_close(index);
    int64_t elapsed = platform_time_monotonic_us() - started;
    out->generation_us = elapsed > 0 ? (uint64_t)elapsed : 1u;
    if (!result.ok) memset(&out->selected, 0, sizeof(out->selected));
    return result;
}
