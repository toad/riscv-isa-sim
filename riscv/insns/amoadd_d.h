require_extension('A');
require_rv64;
LOAD_STORE_TAG_CHECK(RS1);
reg_t v = MMU.load_uint64(RS1);
MMU.store_uint64(RS1, RS2 + v);
WRITE_RD(v);
