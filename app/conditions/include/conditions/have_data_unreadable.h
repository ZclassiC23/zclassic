/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONDITIONS_HAVE_DATA_UNREADABLE_H
#define ZCL_CONDITIONS_HAVE_DATA_UNREADABLE_H

/* SYMPTOM: tip has not advanced for >= 60s and the tip+1 block is marked
 *   BLOCK_HAVE_DATA but block_index_have_data_readable() fails (file=nFile
 *   pos=nDataPos unreadable on disk).
 * REMEDY: clearing h — clear BLOCK_HAVE_DATA, set nFile=-1/nDataPos=0 and
 *   emit EV_BLOCK_REJECTED so the body is re-fetched.
 * WITNESSED: tip advanced to/past target, OR that block's data is now
 *   readable again (block_index_have_data_readable()).
 * COND_WARN; poll_secs=5 (backoff 30s, max_attempts 3). */
void register_have_data_unreadable(void);

#ifdef ZCL_TESTING
void have_data_unreadable_test_reset(void);
int have_data_unreadable_test_remedy_calls(void);
#endif

#endif /* ZCL_CONDITIONS_HAVE_DATA_UNREADABLE_H */
