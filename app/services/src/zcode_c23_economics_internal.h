/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: private rendering constants for the pure C23 economics island. */

#ifndef ZCL_ZCODE_C23_ECONOMICS_INTERNAL_H
#define ZCL_ZCODE_C23_ECONOMICS_INTERNAL_H

#define ZCODE_C23_ECONOMICS_QUEUE_ORDER \
    "maturity_height,maturity_mtp,claim_root"
#define ZCODE_C23_ECONOMICS_CATEGORY_ORDER \
    "zero_root=0;else_first=(root[0]+1)%8;then=cyclic"
#define ZCODE_C23_ECONOMICS_CONCENTRATION_CAP \
    "per-recipient cap=min(epoch_capacity,max(1 ZC23,floor(epoch_capacity/100)))"

#endif /* ZCL_ZCODE_C23_ECONOMICS_INTERNAL_H */
