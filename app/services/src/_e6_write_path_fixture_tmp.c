struct active_chain; struct block_index;
int active_chain_set_tip(struct active_chain *, struct block_index *);
int e6_fixture(struct active_chain *c, struct block_index *b){ return active_chain_set_tip(c, b); }
