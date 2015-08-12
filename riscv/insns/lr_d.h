require_extension('A');
require_rv64;
LOAD_TAG_CHECK(RS1);
p->get_state()->load_reservation = RS1;
WRITE_RD(MMU.load_int64(RS1));
