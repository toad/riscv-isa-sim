require_extension('A');
require_rv64;
LOAD_TAG_CHECK(RS1);
p->get_state()->load_reservation = RS1;
// old version
//WRITE_RD(MMU.load_int64(RS1));
// load datat and tag
reg_t address = RS1;
WRITE_REG(insn.rd(), MMU.load_int64(address), MMU.tag_read(address));
