require_rv64;
STORE_TAG_CHECK(RS1+insn.s_imm());
// store data
MMU.store_uint64(RS1 + insn.s_imm(), RS2);

