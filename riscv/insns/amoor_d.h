require_extension('A');
require_rv64;
LOAD_STORE_TAG_CHECK(RS1);
reg_t v = MMU.load_uint64(RS1);
tag_t t = MMU.tag_read(RS1);
MMU.store_uint64(RS1, RS2 | v);
MMU.tag_write(RS1, 0);
WRITE_REG(insn.rd(), v, t);
