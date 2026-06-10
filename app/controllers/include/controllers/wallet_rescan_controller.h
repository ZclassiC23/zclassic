/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONTROLLERS_WALLET_RESCAN_CONTROLLER_H
#define ZCL_CONTROLLERS_WALLET_RESCAN_CONTROLLER_H

#include "rpc/server.h"

void register_wallet_rescan_rpc_commands(struct rpc_table *t);

#endif
