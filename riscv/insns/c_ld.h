require_extension('C');
require_rv64;
WRITE_RVC_RS2S(MMU.load_int64(RVC_RS1S + insn.rvc_ld_imm()));
