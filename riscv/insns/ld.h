require_rv64;

reg_t address = RS1 + insn.i_imm();
LOAD_TAG_CHECK(address);
WRITE_RD(MMU.load_int64(address));
