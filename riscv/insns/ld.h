require_rv64;

reg_t address = RS1 + insn.i_imm();
LOAD_TAG_CHECK(address);
WRITE_REG(insn.rd(), MMU.load_int64(address), MMU.tag_read(address));
