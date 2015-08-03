// See LICENSE for license details.

#include "processor.h"
#include "extension.h"
#include "common.h"
#include "config.h"
#include "sim.h"
#include "htif.h"
#include "disasm.h"
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <assert.h>
#include <limits.h>
#include <stdexcept>
#include <algorithm>

#undef STATE
#define STATE state

processor_t::processor_t(const char* isa, sim_t* sim, uint32_t id)
  : sim(sim), ext(NULL), disassembler(new disassembler_t),
    id(id), run(false), debug(false)
{
  parse_isa_string(isa);

  mmu = new mmu_t(sim->mem, sim->tagmem, sim->memsz);
  mmu->set_processor(this);

  reset(true);

  #define DECLARE_INSN(name, match, mask) REGISTER_INSN(this, name, match, mask)
  #include "encoding.h"
  #undef DECLARE_INSN
  build_opcode_map();
}

processor_t::~processor_t()
{
#ifdef RISCV_ENABLE_HISTOGRAM
  if (histogram_enabled)
  {
    fprintf(stderr, "PC Histogram size:%lu\n", pc_histogram.size());
    for(auto iterator = pc_histogram.begin(); iterator != pc_histogram.end(); ++iterator) {
      fprintf(stderr, "%0lx %lu\n", (iterator->first << 2), iterator->second);
    }
  }
#endif

  delete mmu;
  delete disassembler;
}

static void bad_isa_string(const char* isa)
{
  fprintf(stderr, "error: bad --isa option %s\n", isa);
  abort();
}

void processor_t::parse_isa_string(const char* isa)
{
  const char* p = isa;
  const char* all_subsets = "IMAFDC";

  max_xlen = 64;
  cpuid = reg_t(2) << 62;

  if (strncmp(p, "RV32", 4) == 0)
    max_xlen = 32, cpuid = 0, p += 4;
  else if (strncmp(p, "RV64", 4) == 0)
    p += 4;
  else if (strncmp(p, "RV", 2) == 0)
    p += 2;

  cpuid |= 1L << ('S' - 'A'); // advertise support for supervisor mode

  if (!*p)
    p = all_subsets;
  else if (*p != 'I')
    bad_isa_string(isa);

  while (*p) {
    cpuid |= 1L << (*p - 'A');

    if (auto next = strchr(all_subsets, *p)) {
      all_subsets = next + 1;
      p++;
    } else if (*p == 'X') {
      const char* ext = p+1, *end = ext;
      while (islower(*end))
        end++;
      register_extension(find_extension(std::string(ext, end - ext).c_str())());
      p = end;
    } else {
      bad_isa_string(isa);
    }
  }

  if (supports_extension('D') && !supports_extension('F'))
    bad_isa_string(isa);
}

void state_t::reset()
{
  memset(this, 0, sizeof(*this));
  mstatus = set_field(mstatus, MSTATUS_PRV, PRV_M);
  mstatus = set_field(mstatus, MSTATUS_PRV1, PRV_S);
  mstatus = set_field(mstatus, MSTATUS_PRV2, PRV_S);
  pc = DEFAULT_MTVEC + 0x100;
  load_reservation = -1;
}

void processor_t::set_debug(bool value)
{
  debug = value;
  if (ext)
    ext->set_debug(value);
}

void processor_t::set_histogram(bool value)
{
  histogram_enabled = value;
}

void processor_t::reset(bool value)
{
  if (run == !value)
    return;
  run = !value;

  state.reset();
  set_csr(CSR_MSTATUS, state.mstatus);

  if (ext)
    ext->reset(); // reset the extension
}

void processor_t::raise_interrupt(reg_t which)
{
  throw trap_t(((reg_t)1 << (max_xlen-1)) | which);
}

void processor_t::take_interrupt()
{
  int priv = get_field(state.mstatus, MSTATUS_PRV);
  int ie = get_field(state.mstatus, MSTATUS_IE);
  reg_t interrupts = state.mie & state.mip;

  if (priv < PRV_M || (priv == PRV_M && ie)) {
    if (interrupts & MIP_MSIP)
      raise_interrupt(IRQ_SOFT);

    if (state.fromhost != 0)
      raise_interrupt(IRQ_HOST);
  }

  if (priv < PRV_S || (priv == PRV_S && ie)) {
    if (interrupts & MIP_SSIP)
      raise_interrupt(IRQ_SOFT);

    if (interrupts & MIP_STIP)
      raise_interrupt(IRQ_TIMER);
  }
}

static void commit_log(state_t* state, reg_t pc, insn_t insn)
{
#ifdef RISCV_ENABLE_COMMITLOG
  if (get_field(state->mstatus, MSTATUS_IE)) {
    uint64_t mask = (insn.length() == 8 ? uint64_t(0) : (uint64_t(1) << (insn.length() * 8))) - 1;
    if (state->log_reg_write.addr) {
      fprintf(stderr, "0x%016" PRIx64 " (0x%08" PRIx64 ") %c%2" PRIu64 " 0x%016" PRIx64 "\n",
              pc,
              insn.bits() & mask,
              state->log_reg_write.addr & 1 ? 'f' : 'x',
              state->log_reg_write.addr >> 1,
              state->log_reg_write.data);
    } else {
      fprintf(stderr, "0x%016" PRIx64 " (0x%08" PRIx64 ")\n", pc, insn.bits() & mask);
    }
  }
  state->log_reg_write.addr = 0;
#endif
}

inline void processor_t::update_histogram(size_t pc)
{
#ifdef RISCV_ENABLE_HISTOGRAM
  size_t idx = pc >> 2;
  pc_histogram[idx]++;
#endif
}

static reg_t execute_insn(processor_t* p, reg_t pc, insn_fetch_t fetch)
{
  reg_t npc = fetch.func(p, fetch.insn, pc);
  if (npc != PC_SERIALIZE) {
    commit_log(p->get_state(), pc, fetch.insn);
    p->update_histogram(pc);
  }
  return npc;
}

void processor_t::check_timer()
{
  // this assumes the rtc doesn't change asynchronously during step(),
  if (state.stimecmp >= (uint32_t)state.prev_rtc
      && state.stimecmp < (uint32_t)sim->rtc)
    state.mip |= MIP_STIP;
  state.prev_rtc = sim->rtc;
}

void processor_t::step(size_t n)
{
  size_t instret = 0;
  reg_t pc = state.pc;
  mmu_t* _mmu = mmu;

  if (unlikely(!run || !n))
    return;

  #define maybe_serialize() \
   if (unlikely(pc == PC_SERIALIZE)) { \
     pc = state.pc; \
     state.serialized = true; \
     break; \
   }

  try
  {
    check_timer();
    take_interrupt();

    if (unlikely(debug))
    {
      while (instret < n)
      {
        insn_fetch_t fetch = mmu->load_insn(pc);
        if (!state.serialized)
          disasm(fetch.insn);
        pc = execute_insn(this, pc, fetch);
        maybe_serialize();
        instret++;
        state.pc = pc;
      }
    }
    else while (instret < n)
    {
      size_t idx = _mmu->icache_index(pc);
      auto ic_entry = _mmu->access_icache(pc);

      #define ICACHE_ACCESS(idx) { \
        insn_fetch_t fetch = ic_entry->data; \
        ic_entry++; \
        pc = execute_insn(this, pc, fetch); \
        if (idx == mmu_t::ICACHE_ENTRIES-1) break; \
        if (unlikely(ic_entry->tag != pc)) break; \
        if (unlikely(instret+1 == n)) break; \
        instret++; \
        state.pc = pc; \
      }

      switch (idx) {
        #include "icache.h"
      }

      maybe_serialize();
      instret++;
      state.pc = pc;
    }
  }
  catch(trap_t& t)
  {
    take_trap(t, pc);
  }

  state.minstret += instret;

  // tail-recurse if we didn't execute as many instructions as we'd hoped
  if (instret < n)
    step(n - instret);
}

void processor_t::push_privilege_stack()
{
  reg_t s = state.mstatus;
  s = set_field(s, MSTATUS_PRV2, get_field(state.mstatus, MSTATUS_PRV1));
  s = set_field(s, MSTATUS_IE2, get_field(state.mstatus, MSTATUS_IE1));
  s = set_field(s, MSTATUS_PRV1, get_field(state.mstatus, MSTATUS_PRV));
  s = set_field(s, MSTATUS_IE1, get_field(state.mstatus, MSTATUS_IE));
  s = set_field(s, MSTATUS_PRV, PRV_M);
  s = set_field(s, MSTATUS_MPRV, 0);
  s = set_field(s, MSTATUS_IE, 0);
  set_csr(CSR_MSTATUS, s);
}

void processor_t::pop_privilege_stack()
{
  reg_t s = state.mstatus;
  s = set_field(s, MSTATUS_PRV, get_field(state.mstatus, MSTATUS_PRV1));
  s = set_field(s, MSTATUS_IE, get_field(state.mstatus, MSTATUS_IE1));
  s = set_field(s, MSTATUS_PRV1, get_field(state.mstatus, MSTATUS_PRV2));
  s = set_field(s, MSTATUS_IE1, get_field(state.mstatus, MSTATUS_IE2));
  s = set_field(s, MSTATUS_PRV2, PRV_U);
  s = set_field(s, MSTATUS_IE2, 1);
  set_csr(CSR_MSTATUS, s);
}

void processor_t::take_trap(trap_t& t, reg_t epc)
{
  if (debug)
    fprintf(stderr, "core %3d: exception %s, epc 0x%016" PRIx64 "\n",
            id, t.name(), epc);

  state.pc = DEFAULT_MTVEC + 0x40 * get_field(state.mstatus, MSTATUS_PRV);
  push_privilege_stack();
  yield_load_reservation();
  state.mcause = t.cause();
  state.mepc = epc;
  t.side_effects(&state); // might set badvaddr etc.
}

void processor_t::deliver_ipi()
{
  state.mip |= MIP_MSIP;
}

void processor_t::disasm(insn_t insn)
{
  uint64_t bits = insn.bits() & ((1ULL << (8 * insn_length(insn.bits()))) - 1);
  fprintf(stderr, "core %3d: 0x%016" PRIx64 " (0x%08" PRIx64 ") %s\n",
          id, state.pc, bits, disassembler->disassemble(insn).c_str());
}

static bool validate_priv(reg_t priv)
{
  return priv == PRV_U || priv == PRV_S || priv == PRV_M;
}

static bool validate_vm(int max_xlen, reg_t vm)
{
  if (max_xlen == 64 && (vm == VM_SV39 || vm == VM_SV48))
    return true;
  if (max_xlen == 32 && vm == VM_SV32)
    return true;
  return vm == VM_MBARE;
}

void processor_t::set_csr(int which, reg_t val)
{
  switch (which)
  {
    case CSR_FFLAGS:
      dirty_fp_state;
      state.fflags = val & (FSR_AEXC >> FSR_AEXC_SHIFT);
      break;
    case CSR_FRM:
      dirty_fp_state;
      state.frm = val & (FSR_RD >> FSR_RD_SHIFT);
      break;
    case CSR_FCSR:
      dirty_fp_state;
      state.fflags = (val & FSR_AEXC) >> FSR_AEXC_SHIFT;
      state.frm = (val & FSR_RD) >> FSR_RD_SHIFT;
      break;
    case CSR_MTIME:
    case CSR_STIMEW:
      // this implementation ignores writes to MTIME
      break;
    case CSR_MTIMEH:
    case CSR_STIMEHW:
      // this implementation ignores writes to MTIME
      break;
    case CSR_TIMEW:
      val -= sim->rtc;
      if (xlen == 32)
        state.sutime_delta = (uint32_t)val | (state.sutime_delta >> 32 << 32);
      else
        state.sutime_delta = val;
      break;
    case CSR_TIMEHW:
      val = ((val << 32) - sim->rtc) >> 32;
      state.sutime_delta = (val << 32) | (uint32_t)state.sutime_delta;
      break;
    case CSR_CYCLEW:
    case CSR_INSTRETW:
      val -= state.minstret;
      if (xlen == 32)
        state.suinstret_delta = (uint32_t)val | (state.suinstret_delta >> 32 << 32);
      else
        state.suinstret_delta = val;
      break;
    case CSR_CYCLEHW:
    case CSR_INSTRETHW:
      val = ((val << 32) - state.minstret) >> 32;
      state.suinstret_delta = (val << 32) | (uint32_t)state.suinstret_delta;
      break;
    case CSR_MSTATUS: {
      if ((val ^ state.mstatus) & (MSTATUS_VM | MSTATUS_PRV | MSTATUS_PRV1 | MSTATUS_MPRV))
        mmu->flush_tlb();

      reg_t mask = MSTATUS_IE | MSTATUS_IE1 | MSTATUS_IE2 | MSTATUS_MPRV
                   | MSTATUS_FS | (ext ? MSTATUS_XS : 0);

      if (validate_vm(max_xlen, get_field(val, MSTATUS_VM)))
        mask |= MSTATUS_VM;
      if (validate_priv(get_field(val, MSTATUS_PRV)))
        mask |= MSTATUS_PRV;
      if (validate_priv(get_field(val, MSTATUS_PRV1)))
        mask |= MSTATUS_PRV1;
      if (validate_priv(get_field(val, MSTATUS_PRV2)))
        mask |= MSTATUS_PRV2;

      state.mstatus = (state.mstatus & ~mask) | (val & mask);

      bool dirty = (state.mstatus & MSTATUS_FS) == MSTATUS_FS;
      dirty |= (state.mstatus & MSTATUS_XS) == MSTATUS_XS;
      if (max_xlen == 32)
        state.mstatus = set_field(state.mstatus, MSTATUS32_SD, dirty);
      else
        state.mstatus = set_field(state.mstatus, MSTATUS64_SD, dirty);

      // spike supports the notion of xlen < max_xlen, but current priv spec
      // doesn't provide a mechanism to run RV32 software on an RV64 machine
      xlen = max_xlen;
      break;
    }
    case CSR_MIP: {
      reg_t mask = MIP_SSIP | MIP_MSIP;
      state.mip = (state.mip & ~mask) | (val & mask);
      break;
    }
    case CSR_MIE: {
      reg_t mask = MIP_SSIP | MIP_MSIP | MIP_STIP;
      state.mie = (state.mie & ~mask) | (val & mask);
      break;
    }
    case CSR_SSTATUS: {
      reg_t ms = state.mstatus;
      ms = set_field(ms, MSTATUS_IE, get_field(val, SSTATUS_IE));
      ms = set_field(ms, MSTATUS_IE1, get_field(val, SSTATUS_PIE));
      ms = set_field(ms, MSTATUS_PRV1, get_field(val, SSTATUS_PS));
      ms = set_field(ms, MSTATUS_FS, get_field(val, SSTATUS_FS));
      ms = set_field(ms, MSTATUS_XS, get_field(val, SSTATUS_XS));
      ms = set_field(ms, MSTATUS_MPRV, get_field(val, SSTATUS_MPRV));
      return set_csr(CSR_MSTATUS, ms);
    }
    case CSR_SIP: {
      reg_t mask = MIP_SSIP;
      state.mip = (state.mip & ~mask) | (val & mask);
      break;
    }
    case CSR_SIE: {
      reg_t mask = MIP_SSIP | MIP_STIP;
      state.mie = (state.mie & ~mask) | (val & mask);
      break;
    }
    case CSR_SEPC: state.sepc = val; break;
    case CSR_STVEC: state.stvec = val & ~3; break;
    case CSR_STIMECMP:
      state.mip &= ~MIP_STIP;
      state.stimecmp = val;
      break;
    case CSR_SPTBR: state.sptbr = zext_xlen(val & -PGSIZE); break;
    case CSR_SSCRATCH: state.sscratch = val; break;
    case CSR_MEPC: state.mepc = val; break;
    case CSR_MSCRATCH: state.mscratch = val; break;
    case CSR_MCAUSE: state.mcause = val; break;
    case CSR_MBADADDR: state.mbadaddr = val; break;
    case CSR_SEND_IPI: sim->send_ipi(val); break;
    case CSR_MTOHOST:
      if (state.tohost == 0)
        state.tohost = val;
      break;
    case CSR_MFROMHOST: state.fromhost = val; break;
  }
}

reg_t processor_t::get_csr(int which)
{
  switch (which)
  {
    case CSR_FFLAGS:
      require_fp;
      if (!supports_extension('F'))
        break;
      return state.fflags;
    case CSR_FRM:
      require_fp;
      if (!supports_extension('F'))
        break;
      return state.frm;
    case CSR_FCSR:
      require_fp;
      if (!supports_extension('F'))
        break;
      return (state.fflags << FSR_AEXC_SHIFT) | (state.frm << FSR_RD_SHIFT);
    case CSR_MTIME:
    case CSR_STIME:
    case CSR_STIMEW:
      return sim->rtc;
    case CSR_MTIMEH:
    case CSR_STIMEH:
    case CSR_STIMEHW:
      return sim->rtc >> 32;
    case CSR_TIME:
    case CSR_TIMEW:
      return sim->rtc + state.sutime_delta;
    case CSR_CYCLE:
    case CSR_CYCLEW:
    case CSR_INSTRET:
    case CSR_INSTRETW:
      return state.minstret + state.suinstret_delta;
    case CSR_TIMEH:
    case CSR_TIMEHW:
      if (xlen == 64)
        break;
      return (sim->rtc + state.sutime_delta) >> 32;
    case CSR_CYCLEH:
    case CSR_INSTRETH:
    case CSR_CYCLEHW:
    case CSR_INSTRETHW:
      if (xlen == 64)
        break;
      return (state.minstret + state.suinstret_delta) >> 32;
    case CSR_SSTATUS: {
      reg_t ss = 0;
      ss = set_field(ss, SSTATUS_IE, get_field(state.mstatus, MSTATUS_IE));
      ss = set_field(ss, SSTATUS_PIE, get_field(state.mstatus, MSTATUS_IE1));
      ss = set_field(ss, SSTATUS_PS, get_field(state.mstatus, MSTATUS_PRV1));
      ss = set_field(ss, SSTATUS_FS, get_field(state.mstatus, MSTATUS_FS));
      ss = set_field(ss, SSTATUS_XS, get_field(state.mstatus, MSTATUS_XS));
      ss = set_field(ss, SSTATUS_MPRV, get_field(state.mstatus, MSTATUS_MPRV));
      if (get_field(state.mstatus, MSTATUS64_SD))
        ss = set_field(ss, (xlen == 32 ? SSTATUS32_SD : SSTATUS64_SD), 1);
      return ss;
    }
    case CSR_SIP: return state.mip & (MIP_SSIP | MIP_STIP);
    case CSR_SIE: return state.mie & (MIP_SSIP | MIP_STIP);
    case CSR_SEPC: return state.sepc;
    case CSR_SBADADDR: return state.sbadaddr;
    case CSR_STVEC: return state.stvec;
    case CSR_STIMECMP: return state.stimecmp;
    case CSR_SCAUSE:
      if (max_xlen > xlen)
        return state.scause | ((state.scause >> (max_xlen-1)) << (xlen-1));
      return state.scause;
    case CSR_SPTBR: return state.sptbr;
    case CSR_SASID: return 0;
    case CSR_SSCRATCH: return state.sscratch;
    case CSR_MSTATUS: return state.mstatus;
    case CSR_MIP: return state.mip;
    case CSR_MIE: return state.mie;
    case CSR_MEPC: return state.mepc;
    case CSR_MSCRATCH: return state.mscratch;
    case CSR_MCAUSE: return state.mcause;
    case CSR_MBADADDR: return state.mbadaddr;
    case CSR_MCPUID: return cpuid;
    case CSR_MIMPID: return IMPL_ROCKET;
    case CSR_MHARTID: return id;
    case CSR_MTVEC: return DEFAULT_MTVEC;
    case CSR_MTDELEG: return 0;
    case CSR_MTOHOST:
      sim->get_htif()->tick(); // not necessary, but faster
      return state.tohost;
    case CSR_MFROMHOST:
      sim->get_htif()->tick(); // not necessary, but faster
      return state.fromhost;
    case CSR_SEND_IPI: return 0;
    case CSR_UARCH0:
    case CSR_UARCH1:
    case CSR_UARCH2:
    case CSR_UARCH3:
    case CSR_UARCH4:
    case CSR_UARCH5:
    case CSR_UARCH6:
    case CSR_UARCH7:
    case CSR_UARCH8:
    case CSR_UARCH9:
    case CSR_UARCH10:
    case CSR_UARCH11:
    case CSR_UARCH12:
    case CSR_UARCH13:
    case CSR_UARCH14:
    case CSR_UARCH15:
      return 0;
  }
  throw trap_illegal_instruction();
}

reg_t illegal_instruction(processor_t* p, insn_t insn, reg_t pc)
{
  throw trap_illegal_instruction();
}

insn_func_t processor_t::decode_insn(insn_t insn)
{
  size_t mask = opcode_map.size()-1;
  insn_desc_t* desc = opcode_map[insn.bits() & mask]; 

  while ((insn.bits() & desc->mask) != desc->match)
    desc++;

  return xlen == 64 ? desc->rv64 : desc->rv32;
}

void processor_t::register_insn(insn_desc_t desc)
{
  assert(desc.mask & 1);
  instructions.push_back(desc);
}

void processor_t::build_opcode_map()
{
  size_t buckets = -1;
  for (auto& inst : instructions)
    while ((inst.mask & buckets) != buckets)
      buckets /= 2;
  buckets++;

  struct cmp {
    decltype(insn_desc_t::match) mask;
    cmp(decltype(mask) mask) : mask(mask) {}
    bool operator()(const insn_desc_t& lhs, const insn_desc_t& rhs) {
      if ((lhs.match & mask) != (rhs.match & mask))
        return (lhs.match & mask) < (rhs.match & mask);
      return lhs.match < rhs.match;
    }
  };
  std::sort(instructions.begin(), instructions.end(), cmp(buckets-1));

  opcode_map.resize(buckets);
  opcode_store.resize(instructions.size() + 1);

  size_t j = 0;
  for (size_t b = 0, i = 0; b < buckets; b++)
  {
    opcode_map[b] = &opcode_store[j];
    while (i < instructions.size() && b == (instructions[i].match & (buckets-1)))
      opcode_store[j++] = instructions[i++];
  }

  assert(j == opcode_store.size()-1);
  opcode_store[j].match = opcode_store[j].mask = 0;
  opcode_store[j].rv32 = &illegal_instruction;
  opcode_store[j].rv64 = &illegal_instruction;
}

void processor_t::register_extension(extension_t* x)
{
  for (auto insn : x->get_instructions())
    register_insn(insn);
  build_opcode_map();
  for (auto disasm_insn : x->get_disasms())
    disassembler->add_insn(disasm_insn);
  if (ext != NULL)
    throw std::logic_error("only one extension may be registered");
  ext = x;
  x->set_processor(this);
}
