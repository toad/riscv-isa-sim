require_extension('A');
LOAD_STORE_TAG_CHECK(RS1);
int32_t v = MMU.load_int32(RS1);
MMU.store_uint32(RS1, std::max(int32_t(RS2),v));
WRITE_RD(v);
