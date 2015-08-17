require_extension('A');
reg_t address = RS1;
tag_t mem_tag = MMU.tag_read(address);
LOAD_STORE_TAG_CHECK(mem_tag, address);
uint32_t v = MMU.load_int32(address);
MMU.store_uint32(address, std::max(uint32_t(RS2),v));
WRITE_RD((int32_t)v);
