/* Fixture copied into app/ by test_make_lint_gates.c. It must trip
 * check_raw_sqlite.sh because node.db DML through sqlite3_exec bypasses the
 * prepared AR_STEP_WRITE path. */
struct node_db {
    void *db;
};

extern int sqlite3_exec(void *db, const char *sql, void *cb, void *arg,
                        char **err);

void lint_fixture_raw_node_db_exec(struct node_db *ndb)
{
    sqlite3_exec(ndb->db, "DELETE FROM wallet_utxos", 0, 0, 0);
}
