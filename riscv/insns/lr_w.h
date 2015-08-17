require_extension('A');
LOAD_TAG_CHECK(RS1);
p->get_state()->load_reservation = RS1;
WRITE_RD(MMU.load_int32(RS1));
