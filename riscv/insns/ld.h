require_rv64;
//WRITE_RD(MMU.load_int64(RS1 + insn.i_imm())); //old functionality

reg_t address = RS1 + insn.i_imm();
WRITE_REG(insn.rd(), MMU.load_int64(address), MMU.tag_read(address));
