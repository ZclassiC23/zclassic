/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "conditions/condition_registry.h"

#define ZCL_CONDITION(name) void register_##name(void);
#include "conditions/condition_registry.def"
#undef ZCL_CONDITION

void condition_registry_register_all(void)
{
#define ZCL_CONDITION(name) register_##name();
#include "conditions/condition_registry.def"
#undef ZCL_CONDITION
}
