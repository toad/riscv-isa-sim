require_extension('A');
require_rv64;
reg_t address = RS1;
tag_t mem_tag = MMU.tag_read(address);
LOAD_STORE_TAG_CHECK(mem_tag, address);
reg_t v = MMU.load_uint64(address);
MMU.store_uint64(address, RS2 ^ v);
WRITE_RD(v);
