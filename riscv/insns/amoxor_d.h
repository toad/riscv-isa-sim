require_extension('A');
require_rv64;
reg_t v = MMU.load_uint64(RS1);
tag_t t = MMU.tag_read(RS1);
MMU.tag_write(RS1, 0);
MMU.store_uint64(RS1, RS2 ^ v);
WRITE_REG(insn.rd(), v, t);
