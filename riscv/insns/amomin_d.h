require_extension('A');
require_rv64;
sreg_t v = MMU.load_int64(RS1);
tag_t t = MMU.tag_read(RS1);
MMU.store_uint64(RS1, std::min(sreg_t(RS2),v));
MMU.tag_write(RS1, 0);
WRITE_REG(insn.rd(), v, t);
