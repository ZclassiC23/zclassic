/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Process ownership lease for a canonical node database pathname. */

#ifndef ZCL_DATABASE_OWNER_LEASE_H
#define ZCL_DATABASE_OWNER_LEASE_H

#include <stdbool.h>

struct node_db;

bool node_db_owner_lease_acquire(struct node_db *ndb);
bool node_db_owner_lease_rebind(struct node_db *ndb);
void node_db_owner_lease_release(struct node_db *ndb);

#endif
