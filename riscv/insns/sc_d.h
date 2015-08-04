require_extension('A');
require_rv64;
if (RS1 == p->get_state()->load_reservation)
{
  // store data
  MMU.store_uint64(RS1, RS2);
  // store tag
  MMU.tag_write(RS1, RS2_TAG);
  WRITE_RD(0);
}
else
  WRITE_RD(1);
