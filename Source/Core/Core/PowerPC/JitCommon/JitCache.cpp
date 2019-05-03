// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

// Enable define below to enable oprofile integration. For this to work,
// it requires at least oprofile version 0.9.4, and changing the build
// system to link the Dolphin executable against libopagent.  Since the
// dependency is a little inconvenient and this is possibly a slight
// performance hit, it's not enabled by default, but it's useful for
// locating performance issues.

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <utility>
#include <stdio.h>
#include <time.h>

#include "Common/CommonTypes.h"
#include "Common/JitRegister.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace Gen;

bool JitBlock::OverlapsPhysicalRange(u32 address, u32 length) const
{
  return physical_addresses.lower_bound(address) !=
         physical_addresses.lower_bound(address + length);
}

JitBaseBlockCache::JitBaseBlockCache(JitBase& jit) : m_jit{jit}
{
}

JitBaseBlockCache::~JitBaseBlockCache() = default;

void JitBaseBlockCache::Init()
{
  JitRegister::Init(SConfig::GetInstance().m_perfDir);

  Clear();
}

void JitBaseBlockCache::Shutdown()
{
  JitRegister::Shutdown();
}

// e.second.profile_data.ticStop = time(NULL);
//    printf("BLOCK TOTAL RUN\t0x%x\t%d\n", e.second.effectiveAddress,
//          e.second.profile_data.runCount*1000 /
//             (e.second.profile_data.ticStop - e.second.profile_data.ticStart));

u32 JitBaseBlockCache::hot_score(JitBlock e)
{
  u32 hotness;
  hotness = (u64) ((e.profile_data.runCount*1000)/
                            (e.profile_data.ticStop - e.profile_data.ticStart));
  return (0.1) * e.profile_data.old_hotness + (1 - 0.1) * hotness;
} 

void JitBaseBlockCache::Profile_block_map(std::multimap<u32, u32>& address_and_code){
  std::multimap<u64, u32> sorted_heat;
  u64 hotness;
  u64 CC_size = block_map.size();
  JitBlock b;

  for (auto& e : block_map)
    {
      hotness = hot_score(e.second);
      sorted_heat.insert(std::pair<u64, u32>(hotness, e.first));
    }
    while (1){
      if( sorted_heat.size() <= CC_size / 4){
        break;
      }
      sorted_heat.erase(sorted_heat.begin()->first);
    }
    //does b do what I want??? I have no idea
    for (auto& e : sorted_heat){
      b = block_map.find(e.second)->second;
      address_and_code.insert(std::pair<u32, u32>(b.effectiveAddress, 0));
    }
}

void JitBaseBlockCache::New_Clear()
{
 

  #if defined(_DEBUG) || defined(DEBUGFAST)
  Core::DisplayMessage("Clearing code cache.", 3000);
#endif
  m_jit.js.fifoWriteAddresses.clear();
  m_jit.js.pairedQuantizeAddresses.clear();

  //insert blocks by heat for sorting
  //TODO CALL Profile
  for (auto& e : block_map)
  {
    DestroyBlock(e.second);
  }
  block_map.clear();
  links_to.clear();
  block_range_map.clear();

  valid_block.ClearAll();

  fast_block_map.fill(nullptr);
}

// This clears the JIT cache. It's called from JitCache.cpp when the JIT cache
// is full and when saving and loading states.
void JitBaseBlockCache::Clear()
{
  printf("Entering Clear()\n");
#if defined(_DEBUG) || defined(DEBUGFAST)
  Core::DisplayMessage("Clearing code cache.", 3000);
#endif
  m_jit.js.fifoWriteAddresses.clear();
  m_jit.js.pairedQuantizeAddresses.clear();
  for (auto& e : block_map)
  {
    DestroyBlock(e.second);
    /*printf("%5x : Effective Address\n", e.second.effectiveAddress);
    printf("%5x : Normal Entry\n", e.second.normalEntry);
    printf("%5x : Start\n", e.second.start);
    printf("%5x : physical addr\n", e.second.physicalAddress);
    printf("---------\n");
    */
  }
  block_map.clear();
  links_to.clear();
  block_range_map.clear();

  valid_block.ClearAll();

  fast_block_map.fill(nullptr);
}

bool JitBaseBlockCache::ThanosEval(const u8* r, size_t code_size)
{
  printf("Entering Thanos\n");
  auto MID = r + code_size / 2;
  u32 tmpHot;
  float UpperAvg = 0;
  float LowerAvg = 0;
  u32 UpperHotness = 0;
  u32 UpperCount = 0;
  u32 LowerHotness = 0;
  u32 LowerCount = 0;

  for (auto& e : block_map)
  {
  //  printf("entering hot_score\n");
    //tmpHot = 1;
    tmpHot = hot_score(e.second);
   // printf("leaving hot_score\n");
    if (e.second.start + e.second.codeSize >= MID)
    {
      UpperCount++;
      UpperHotness += tmpHot;
    }
    else
    {
      LowerCount++;
      LowerHotness += tmpHot;
    }
    e.second.profile_data.old_hotness = tmpHot;
  }
  if(UpperCount != 0)
    UpperAvg = (UpperHotness * 1.0) / (UpperCount * 1.0);
  if(LowerCount != 0)
    LowerAvg = (LowerHotness * 1.0) / (LowerCount * 1.0);

  printf("UpperCount:\t%d\n", UpperCount);
  printf("UpperHotness:\t%d\n", UpperHotness);
  printf("Upper AVG Hotness:\t%f\n", UpperAvg);
  printf("LowerCount:\t%d\n", LowerCount);
  printf("LowerHotness:\t%d\n", LowerHotness);
  printf("Lower AVG Hotness:\t%f\n", LowerAvg);

  return UpperCount >= LowerCount;
  return UpperAvg >= LowerAvg;
}

/*
JitBlock* JitBaseBlockCache::DupJitBlock(u32 em_address, u32 msr)
{
  JitBlock* tmp =  GetBlockFromStartAddress(em_address, msr);
  void * ptr = malloc(sizeof(*tmp));
  memccpy(ptr, tmp, 1, sizeof(*tmp));
  return (JitBlock*)ptr;
}
*/

void JitBaseBlockCache::Clear2(const u8* r, size_t code_size, std::multimap<u8*,JitBlock> & t_block_m)
{
  //void * TMP_CACHE = malloc(code_size/2);
  //void * ptr = TMP_CACHE;
  printf("Entering Clear2()\n");

#if defined(_DEBUG) || defined(DEBUGFAST)
  Core::DisplayMessage("Clearing code cache. - THANOS", 3000);
#endif
  m_jit.js.fifoWriteAddresses.clear();
  m_jit.js.pairedQuantizeAddresses.clear();

//  std::multimap<u32,JitBlock> t_block_m;

  auto MID = r + code_size / 2;
  for (auto& e : block_map)
  {
    if (e.second.start + e.second.codeSize >= MID)
    {
      DestroyBlock(e.second);
    }
    else
    {
   //   memccpy(ptr, e.second.normalEntry, e.second.codeSize, 1);
    //  ptr = ptr + e.second.codeSize;
      t_block_m.insert(std::pair<u8*, JitBlock>(e.second.normalEntry, e.second));
    }
    
  }
/*
  printf("Left Destroy loop\n");
  for(std::multimap<u32,JitBlock>::iterator i = block_map.begin(); i != block_map.end(); i++)
  {
    if ((*i).second.start + (*i).second.codeSize >= MID)
    {
      block_map.erase(i);
      //DestroyBlock((*i).second);
    }
  }
  */
/*  for (auto& e : block_map)
  {
    printf("Inside block_map erase loop:\t%x\n", e.first);
    if (e.second.start + e.second.codeSize >= MID)
    {
      printf("Erasing Value:\t%x\n", e.first);
      //block_map.erase(block_map.find(e.first);
    }
  }
  */
/* for(auto& e: t_block_m)
 {
   //printf("HELP\n");
   //printf("Iterating through block_m: %x\n", e.second.normalEntry);
 }
 */
  block_map.clear();
  //block_map = t_block_m;
  links_to.clear();
  block_range_map.clear();

  valid_block.ClearAll();

  fast_block_map.fill(nullptr);
}

void JitBaseBlockCache::Reset()
{
  Shutdown();
  Init();
}

JitBlock** JitBaseBlockCache::GetFastBlockMap()
{
  return fast_block_map.data();
}

void JitBaseBlockCache::RunOnBlocks(std::function<void(const JitBlock&)> f)
{
  for (const auto& e : block_map)
    f(e.second);
}

JitBlock* JitBaseBlockCache::AllocateBlock(u32 em_address)
{
  u32 physicalAddress = PowerPC::JitCache_TranslateAddress(em_address).address;
  JitBlock& b = block_map.emplace(physicalAddress, JitBlock())->second;
  b.profile_data.old_hotness = 0;
  b.effectiveAddress = em_address;
  b.physicalAddress = physicalAddress;
  b.msrBits = MSR.Hex & JIT_CACHE_MSR_MASK;
  b.linkData.clear();
  b.fast_block_map_index = 0;
  return &b;
}

void JitBaseBlockCache::FinalizeBlock(JitBlock& block, bool block_link,
                                      const std::set<u32>& physical_addresses)
{
  size_t index = FastLookupIndexForAddress(block.effectiveAddress);
  // block.profile_data.ticStart = time(NULL);
  fast_block_map[index] = &block;
  block.fast_block_map_index = index;

  block.physical_addresses = physical_addresses;

  u32 range_mask = ~(BLOCK_RANGE_MAP_ELEMENTS - 1);
  for (u32 addr : physical_addresses)
  {
    valid_block.Set(addr / 32);
    block_range_map[addr & range_mask].insert(&block);
  }

  if (block_link)
  {
    for (const auto& e : block.linkData)
    {
      links_to.emplace(e.exitAddress, &block);
    }

    LinkBlock(block);
  }

  Common::Symbol* symbol = nullptr;
  if (JitRegister::IsEnabled() &&
      (symbol = g_symbolDB.GetSymbolFromAddr(block.effectiveAddress)) != nullptr)
  {
    JitRegister::Register(block.checkedEntry, block.codeSize, "JIT_PPC_%s_%08x",
                          symbol->function_name.c_str(), block.physicalAddress);
  }
  else
  {
    JitRegister::Register(block.checkedEntry, block.codeSize, "JIT_PPC_%08x",
                          block.physicalAddress);
  }
}

JitBlock* JitBaseBlockCache::GetBlockFromStartAddress(u32 addr, u32 msr)
{
  u32 translated_addr = addr;
  if (UReg_MSR(msr).IR)
  {
    auto translated = PowerPC::JitCache_TranslateAddress(addr);
    if (!translated.valid)
    {
      return nullptr;
    }
    translated_addr = translated.address;
  }

  auto iter = block_map.equal_range(translated_addr);
  for (; iter.first != iter.second; iter.first++)
  {
    JitBlock& b = iter.first->second;
    if (b.effectiveAddress == addr && b.msrBits == (msr & JIT_CACHE_MSR_MASK))
      return &b;
  }

  return nullptr;
}

const u8* JitBaseBlockCache::Dispatch()
{
  // printf("Entering Dispatch()\n");
  JitBlock* block = fast_block_map[FastLookupIndexForAddress(PC)];

  if (!block || block->effectiveAddress != PC || block->msrBits != (MSR.Hex & JIT_CACHE_MSR_MASK))
  {
    block = MoveBlockIntoFastCache(PC, MSR.Hex & JIT_CACHE_MSR_MASK);
    // block->profile_data.runCount++;
    // printf("DISPATCH:\t0x%x\t%d\n", PC, block->profile_data.runCount);
  }

  if (!block)
    return nullptr;

  // block->profile_data.runCount++;
  // printf("DISPATCH:\t0x%x\t%d\n", PC, block->profile_data.runCount);
  return block->normalEntry;
}

void JitBaseBlockCache::InvalidateICache(u32 address, u32 length, bool forced)
{
  auto translated = PowerPC::JitCache_TranslateAddress(address);
  if (!translated.valid)
    return;
  u32 pAddr = translated.address;

  // Optimize the common case of length == 32 which is used by Interpreter::dcb*
  bool destroy_block = true;
  if (length == 32)
  {
    if (!valid_block.Test(pAddr / 32))
      destroy_block = false;
    else
      valid_block.Clear(pAddr / 32);
  }

  if (destroy_block)
  {
    // destroy JIT blocks
    ErasePhysicalRange(pAddr, length);

    // If the code was actually modified, we need to clear the relevant entries from the
    // FIFO write address cache, so we don't end up with FIFO checks in places they shouldn't
    // be (this can clobber flags, and thus break any optimization that relies on flags
    // being in the right place between instructions).
    if (!forced)
    {
      for (u32 i = address; i < address + length; i += 4)
      {
        m_jit.js.fifoWriteAddresses.erase(i);
        m_jit.js.pairedQuantizeAddresses.erase(i);
      }
    }
  }
}

void JitBaseBlockCache::ErasePhysicalRange(u32 address, u32 length)
{
  // Iterate over all macro blocks which overlap the given range.
  u32 range_mask = ~(BLOCK_RANGE_MAP_ELEMENTS - 1);
  auto start = block_range_map.lower_bound(address & range_mask);
  auto end = block_range_map.lower_bound(address + length);
  while (start != end)
  {
    // Iterate over all blocks in the macro block.
    auto iter = start->second.begin();
    while (iter != start->second.end())
    {
      JitBlock* block = *iter;
      if (block->OverlapsPhysicalRange(address, length))
      {
        // If the block overlaps, also remove all other occupied slots in the other macro blocks.
        // This will leak empty macro blocks, but they may be reused or cleared later on.
        for (u32 addr : block->physical_addresses)
          if ((addr & range_mask) != start->first)
            block_range_map[addr & range_mask].erase(block);

        // And remove the block.
        DestroyBlock(*block);
        auto block_map_iter = block_map.equal_range(block->physicalAddress);
        while (block_map_iter.first != block_map_iter.second)
        {
          if (&block_map_iter.first->second == block)
          {
            block_map.erase(block_map_iter.first);
            break;
          }
          block_map_iter.first++;
        }
        iter = start->second.erase(iter);
      }
      else
      {
        iter++;
      }
    }

    // If the macro block is empty, drop it.
    if (start->second.empty())
      start = block_range_map.erase(start);
    else
      start++;
  }
}

u32* JitBaseBlockCache::GetBlockBitSet() const
{
  return valid_block.m_valid_block.get();
}

void JitBaseBlockCache::WriteDestroyBlock(const JitBlock& block)
{
}

// Block linker
// Make sure to have as many blocks as possible compiled before calling this
// It's O(N), so it's fast :)
// Can be faster by doing a queue for blocks to link up, and only process those
// Should probably be done

void JitBaseBlockCache::LinkBlockExits(JitBlock& block)
{
  for (auto& e : block.linkData)
  {
    if (!e.linkStatus)
    {
      JitBlock* destinationBlock = GetBlockFromStartAddress(e.exitAddress, block.msrBits);
      if (destinationBlock)
      {
        WriteLinkBlock(e, destinationBlock);
        e.linkStatus = true;
      }
    }
  }
}

void JitBaseBlockCache::LinkBlock(JitBlock& block)
{
  LinkBlockExits(block);
  auto ppp = links_to.equal_range(block.effectiveAddress);

  for (auto iter = ppp.first; iter != ppp.second; ++iter)
  {
    JitBlock& b2 = *iter->second;
    if (block.msrBits == b2.msrBits)
      LinkBlockExits(b2);
  }
}

void JitBaseBlockCache::UnlinkBlock(const JitBlock& block)
{
  // Unlink all exits of this block.
  for (auto& e : block.linkData)
  {
    WriteLinkBlock(e, nullptr);
  }

  // Unlink all exits of other blocks which points to this block
  auto ppp = links_to.equal_range(block.effectiveAddress);
  for (auto iter = ppp.first; iter != ppp.second; ++iter)
  {
    JitBlock& sourceBlock = *iter->second;
    if (sourceBlock.msrBits != block.msrBits)
      continue;

    for (auto& e : sourceBlock.linkData)
    {
      if (e.exitAddress == block.effectiveAddress)
      {
        WriteLinkBlock(e, nullptr);
        e.linkStatus = false;
      }
    }
  }
}

void JitBaseBlockCache::DestroyBlock(JitBlock& block)
{
  if (fast_block_map[block.fast_block_map_index] == &block)
    fast_block_map[block.fast_block_map_index] = nullptr;

  UnlinkBlock(block);

  // Delete linking addresses
  for (const auto& e : block.linkData)
  {
    auto it = links_to.equal_range(e.exitAddress);
    while (it.first != it.second)
    {
      if (it.first->second == &block)
        it.first = links_to.erase(it.first);
      else
        it.first++;
    }
  }

  // Raise an signal if we are going to call this block again
  WriteDestroyBlock(block);
}

JitBlock* JitBaseBlockCache::MoveBlockIntoFastCache(u32 addr, u32 msr)
{
  JitBlock* block = GetBlockFromStartAddress(addr, msr);

  if (!block)
    return nullptr;

  // Drop old fast block map entry
  if (fast_block_map[block->fast_block_map_index] == block)
    fast_block_map[block->fast_block_map_index] = nullptr;

  // And create a new one
  size_t index = FastLookupIndexForAddress(addr);
  fast_block_map[index] = block;
  block->fast_block_map_index = index;

  return block;
}

size_t JitBaseBlockCache::FastLookupIndexForAddress(u32 address)
{
  return (address >> 2) & FAST_BLOCK_MAP_MASK;
}
