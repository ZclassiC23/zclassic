#ifndef ZCL_EVENT_AGENT_READINESS_H
#define ZCL_EVENT_AGENT_READINESS_H

#include <stdbool.h>

struct json_value;

void agent_push_readiness_json(struct json_value *out, const char *key,
                               bool serving, bool has_peers,
                               bool operator_needed,
                               bool validation_pack_ok, int gap,
                               int index_gap, int log_head_gap);

void agent_push_readiness_fields_json(struct json_value *out,
                                      bool serving, bool has_peers,
                                      bool operator_needed,
                                      bool validation_pack_ok, int gap,
                                      int index_gap, int log_head_gap);

void agent_push_readiness_contract_json(struct json_value *out,
                                        const char *key,
                                        bool serving, bool has_peers,
                                        bool operator_needed,
                                        bool validation_pack_ok, int gap,
                                        int index_gap, int log_head_gap);

#endif /* ZCL_EVENT_AGENT_READINESS_H */
