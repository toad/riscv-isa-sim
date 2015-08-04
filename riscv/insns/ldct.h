require_rv64;
tag_t tag = MMU.tag_read(RS1 + insn.i_imm());
int csr = validate_csr(CSR_LD_TAG, true);
reg_t csr_content = p->get_csr(csr);

if((csr_content >> tag) & 1) {
  throw trap_load_tag(RS1 + insn.i_imm());
} else {
  reg_t address = RS1 + insn.i_imm();
  WRITE_REG(insn.rd(), MMU.load_int64(address), MMU.tag_read(address));

}
