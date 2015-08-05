// See LICENSE for license details.

#ifndef _RISCV_MMU_H
#define _RISCV_MMU_H

//#include <iostream> //for debugging
#include "decode.h"
#include "trap.h"
#include "common.h"
#include "config.h"
#include "processor.h"
#include "memtracer.h"
#include <stdlib.h>
#include <vector>
//#include <stdlib.h> //TODO
#include <iostream>

// virtual memory configuration
#define PGSHIFT 12
const reg_t PGSIZE = 1 << PGSHIFT;

struct insn_fetch_t
{
  insn_func_t func;
  insn_t insn;
};

struct icache_entry_t {
  reg_t tag;
  reg_t pad;
  insn_fetch_t data;
};

// this class implements a processor's port into the virtual memory system.
// an MMU and instruction cache are maintained for simulator performance.
class mmu_t
{
public:
  mmu_t(char* _mem, char* _tagmem, size_t _memsz);
  ~mmu_t();

  // template for functions that load an aligned value from memory
  #define load_func(type) \
    type##_t load_##type(reg_t addr) __attribute__((always_inline)) { \
      void* paddr = translate(addr, sizeof(type##_t), false, false); \
	  if(tag_read(addr) & 2) printf("Illegal load at addr %p.\n",addr); \
      return *(type##_t*)paddr; \
    }

      //if(tag_read(addr)==0x03) printf("Load after free at addr %p.\n",addr); 
  // load value from memory at aligned address; zero extend to register width
  load_func(uint8)
  load_func(uint16)
  load_func(uint32)
  load_func(uint64)

  // load value from memory at aligned address; sign extend to register width
  load_func(int8)
  load_func(int16)
  load_func(int32)
  load_func(int64)

  // template for functions that store an aligned value to memory
  #define store_func(type) \
    void store_##type(reg_t addr, type##_t val) { \
      void* paddr = translate(addr, sizeof(type##_t), true, false); \
	  if(tag_read(addr) & 1) printf("Illegal store at addr %p.\n",addr); \
      *(type##_t*)paddr = val; \
    }

      //if(tag_read(addr)==0x03) printf("Store after free at addr %p.\n",addr); 
  // store value to memory at aligned address
  store_func(uint8)
  store_func(uint16)
  store_func(uint32)
  store_func(uint64)
  
  void load_dummy(reg_t addr)
  {
      void* paddr = translate(addr, sizeof(uint64_t), false, false);
  }
  
  void store_dummy(reg_t addr)
  {
      void *paddr = translate(addr, sizeof(uint64_t), true, false);
  }

  static const reg_t ICACHE_ENTRIES = 1024;
  
  //TODO read and write tags
  void tag_write(reg_t addr, char tag)
  {
      //translation from virtual addr to physical addr
      reg_t pte = walk(addr);
      reg_t pgoff = addr & (PGSIZE-1);
      reg_t pgbase = pte >> PGSHIFT << PGSHIFT;
      reg_t paddr = pgbase + pgoff;
      //set or clear corresponding bit
      *(tagmem + (paddr>>3)) = tag;
      //std::cout<<"Store tag at address: "<<std::hex<<paddr<<std::endl;
  }
  
  char tag_read(reg_t addr)
  {
      //translation from virtual addr to physical addr
      reg_t pte = walk(addr);
      reg_t pgoff = addr & (PGSIZE-1);
      reg_t pgbase = pte >> PGSHIFT << PGSHIFT;
      reg_t paddr = pgbase + pgoff;
      //read corresponding bit
      //std::cout<<"Load tag at address: "<<std::hex<<paddr<<std::endl;
      return *(tagmem + (paddr>>3));
  }

  inline size_t icache_index(reg_t addr)
  {
    // for instruction sizes != 4, this hash still works but is suboptimal
    return (addr / 4) % ICACHE_ENTRIES;
  }

  // load instruction from memory at aligned address.
  icache_entry_t* access_icache(reg_t addr) __attribute__((always_inline))
  {
    reg_t idx = icache_index(addr);
    icache_entry_t* entry = &icache[idx];
    if (likely(entry->tag == addr))
      return entry;

    char* iaddr = (char*)translate(addr, 1, false, true);
    insn_bits_t insn = *(uint16_t*)iaddr;
    int length = insn_length(insn);

    if (likely(length == 4)) {
      if (likely(addr % PGSIZE < PGSIZE-2))
        insn |= (insn_bits_t)*(int16_t*)(iaddr + 2) << 16;
      else
        insn |= (insn_bits_t)*(int16_t*)translate(addr + 2, 1, false, true) << 16;
    } else if (length == 2) {
      insn = (int16_t)insn;
    } else if (length == 6) {
      insn |= (insn_bits_t)*(int16_t*)translate(addr + 4, 1, false, true) << 32;
      insn |= (insn_bits_t)*(uint16_t*)translate(addr + 2, 1, false, true) << 16;
    } else {
      static_assert(sizeof(insn_bits_t) == 8, "insn_bits_t must be uint64_t");
      insn |= (insn_bits_t)*(int16_t*)translate(addr + 6, 1, false, true) << 48;
      insn |= (insn_bits_t)*(uint16_t*)translate(addr + 4, 1, false, true) << 32;
      insn |= (insn_bits_t)*(uint16_t*)translate(addr + 2, 1, false, true) << 16;
    }

    insn_fetch_t fetch = {proc->decode_insn(insn), insn};
    icache[idx].tag = addr;
    icache[idx].data = fetch;

    reg_t paddr = iaddr - mem;
    if (!tracer.empty() && tracer.interested_in_range(paddr, paddr + 1, false, true))
    {
      icache[idx].tag = -1;
      tracer.trace(paddr, length, false, true);
    }
    return &icache[idx];
  }

  inline insn_fetch_t load_insn(reg_t addr)
  {
    return access_icache(addr)->data;
  }

  void set_processor(processor_t* p) { proc = p; flush_tlb(); }

  void flush_tlb();
  void flush_icache();

  void register_memtracer(memtracer_t*);

private:
  char* mem;
  size_t memsz;
  processor_t* proc;
  memtracer_list_t tracer;  
  //TODO: locate a memory space for tags
  
  char* tagmem;

  // implement an instruction cache for simulator performance
  icache_entry_t icache[ICACHE_ENTRIES];

  // implement a TLB for simulator performance
  static const reg_t TLB_ENTRIES = 256;
  char* tlb_data[TLB_ENTRIES];
  reg_t tlb_insn_tag[TLB_ENTRIES];
  reg_t tlb_load_tag[TLB_ENTRIES];
  reg_t tlb_store_tag[TLB_ENTRIES];

  // finish translation on a TLB miss and upate the TLB
  void* refill_tlb(reg_t addr, reg_t bytes, bool store, bool fetch);

  // perform a page table walk for a given VA; set referenced/dirty bits
  reg_t walk(reg_t addr, bool supervisor, bool store, bool fetch);

  // translate a virtual address to a physical address
  void* translate(reg_t addr, reg_t bytes, bool store, bool fetch)
    __attribute__((always_inline))
  {
    reg_t idx = (addr >> PGSHIFT) % TLB_ENTRIES;
    reg_t expected_tag = addr >> PGSHIFT;
    reg_t* tags = fetch ? tlb_insn_tag : store ? tlb_store_tag :tlb_load_tag;
    reg_t tag = tags[idx];
    void* data = tlb_data[idx] + addr;

    if (unlikely(addr & (bytes-1)))
      store ? throw trap_store_address_misaligned(addr) :
      fetch ? throw trap_instruction_address_misaligned(addr) :
      throw trap_load_address_misaligned(addr);

    if (likely(tag == expected_tag))
      return data;

    return refill_tlb(addr, bytes, store, fetch);
  }
  
  friend class processor_t;
};

#endif
