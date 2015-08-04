//MMU.load_dummy( RS1 + insn.i_imm() );
MMU.load_dummy( (RS1 + insn.s_imm())>>3<<3 );
WRITE_RD( MMU.tag_read(RS1 + insn.i_imm()) );
