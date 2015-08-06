require_extension('A');
LOAD_TAG_CHECK(RS1);
STORE_TAG_CHECK(RS2);
reg_t v = MMU.load_int32(RS1);
MMU.store_uint32(RS1, RS2 & v);
WRITE_RD(v);
