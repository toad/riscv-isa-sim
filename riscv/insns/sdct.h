require_rv64;
tag_t tag = MMU.tag_read(RS1 + insn.s_imm());
int csr = validate_csr(CSR_SD_TAG, true);
reg_t csr_content = p->get_csr(csr);

//printf("STORE DEBUG: tag: 0x%lx; csr 0x%lx\n", tag, csr_content);
if((csr_content >> tag) & 1) {
  //printf("Trap on a store!\n");
  throw trap_store_tag(RS1 + insn.s_imm());
} else {
  // store data
  MMU.store_uint64(RS1 + insn.s_imm(), RS2);
  // store tag
  MMU.tag_write(RS1 + insn.s_imm(), RS2_TAG);  
}
