require_extension('A');
STORE_TAG_CHECK(RS1+insn.s_imm());
if (RS1 == p->get_state()->load_reservation)
{
  MMU.store_uint32(RS1, RS2);
  WRITE_RD(0);
}
else
  WRITE_RD(1);
