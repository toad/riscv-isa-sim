require_rv64;
LOAD_TAG_CHECK(RS1+insn.i_imm());

reg_t address = RS1 + insn.i_imm();
WRITE_REG(insn.rd(), MMU.load_int64(address), MMU.tag_read(address));
