require_rv64;
LOAD_TAG_CHECK(RS1 + insn.i_imm());
WRITE_RD(MMU.load_uint32(RS1 + insn.i_imm()));
