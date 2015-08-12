require_rv64;

reg_t address = RS1 + insn.s_imm();
STORE_TAG_CHECK(address);

// store data
MMU.store_uint64(address, RS2);
// store tag
MMU.tag_write(address, RS2_TAG);  

