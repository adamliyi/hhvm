/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | (c) Copyright IBM Corporation 2015-2016                              |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/jit/smashable-instr-ppc64.h"

#include "hphp/runtime/vm/jit/abi-ppc64.h"
#include "hphp/runtime/vm/jit/align-ppc64.h"
#include "hphp/runtime/vm/jit/cg-meta.h"
#include "hphp/runtime/vm/jit/mc-generator.h"

#include "hphp/ppc64-asm/asm-ppc64.h"
#include "hphp/ppc64-asm/decoded-instr-ppc64.h"

#include "hphp/util/data-block.h"

namespace HPHP { namespace jit { namespace ppc64 {

using ppc64_asm::PPC64Instr;
using ppc64_asm::Assembler;

///////////////////////////////////////////////////////////////////////////////


#define EMIT_BODY(cb, meta, inst, ...)      \
  ([&] {                                    \
    auto const start = cb.frontier();       \
    meta.smashableLocations.insert(start);  \
    Assembler a { cb };                     \
    a.inst(__VA_ARGS__);                    \
    return start;                           \
  }())

TCA emitSmashableMovq(CodeBlock& cb, CGMeta& fixups, uint64_t imm,
                      PhysReg d) {
  return EMIT_BODY(cb, fixups, li64, d, imm, true);
}

TCA emitSmashableCmpq(CodeBlock& cb, CGMeta& fixups, int32_t imm,
                      PhysReg r, int8_t disp) {
  auto const start = cb.frontier();
  fixups.smashableLocations.insert(start);
  Assembler a { cb };

  // don't use cmpqim because of smashableCmpqImm implementation. A "load 32bits
  // immediate" is mandatory
  a.li32 (rfuncln(), imm);
  a.lwz  (rAsm, r[disp]); // base + displacement
  a.extsw(rAsm, rAsm);
  a.cmpd (rfuncln(), rAsm);
  return start;
}

TCA emitSmashableCall(CodeBlock& cb, CGMeta& fixups, TCA target) {
  return EMIT_BODY(cb, fixups, call, target, Assembler::CallArg::Smashable);
}

TCA emitSmashableJmp(CodeBlock& cb, CGMeta& fixups, TCA target) {
  return EMIT_BODY(cb, fixups, branchFar, target);
}

TCA emitSmashableJcc(CodeBlock& cb, CGMeta& fixups, TCA target,
                     ConditionCode cc) {
  assertx(cc != CC_None);
  return EMIT_BODY(cb, fixups, branchFar, target, cc);
}

#undef EMIT_BODY

///////////////////////////////////////////////////////////////////////////////

void smashMovq(TCA inst, uint64_t imm) {
  CodeBlock cb;
  // Initialize code block cb pointing to li64
  cb.init(inst, Assembler::kLi64Len, "smashing Movq");
  CodeCursor cursor { cb, inst };
  Assembler a { cb };

  const ppc64_asm::DecodedInstruction di(inst);
  Reg64 reg = di.getLi64Reg();

  a.li64(reg, imm, true);
}

void smashCmpq(TCA inst, uint32_t imm) {
  auto& cb = mcg->code().blockFor(inst);
  CodeCursor cursor { cb, inst };
  Assembler a { cb };

  // the first instruction is a vasm ldimml, which is a li32
  const ppc64_asm::DecodedInstruction di(inst);
  Reg64 reg = di.getLi32Reg();

  a.li32(reg, imm);
}

void smashCall(TCA inst, TCA target) {
  auto& cb = mcg->code().blockFor(inst);
  CodeCursor cursor { cb, inst };
  Assembler a { cb };

  const ppc64_asm::DecodedInstruction di(inst);
  if (!di.isCall()) {
    always_assert(false && "smashCall has unexpected block");
  }

  a.setFrontier(inst);

  a.li64(rfuncentry(), reinterpret_cast<uint64_t>(target), true);
}

void smashJmp(TCA inst, TCA target) {
  auto& cb = mcg->code().blockFor(inst);
  CodeCursor cursor { cb, inst };
  Assembler a { cb };

  if (target > inst && target - inst <= smashableJmpLen()) {
    a.emitNop(target - inst);
  } else {
    a.branchAuto(target);
  }
}

void smashJcc(TCA inst, TCA target, ConditionCode cc) {
  if (cc == CC_None) {
    // It was emitted as a conditional, so patching needs to be conditional
    Assembler::patchBranch(inst, target, true);
  } else {
    auto& cb = mcg->code().blockFor(inst);
    CodeCursor cursor { cb, inst };
    Assembler a { cb };
    a.branchAuto(target, cc);
  }
}

///////////////////////////////////////////////////////////////////////////////

uint64_t smashableMovqImm(TCA inst) {
  const ppc64_asm::DecodedInstruction di(inst);
  return di.immediate();
}

uint32_t smashableCmpqImm(TCA inst) {
  const ppc64_asm::DecodedInstruction di(inst);
  return static_cast<uint32_t>(di.immediate());
}

TCA smashableCallTarget(TCA inst) {
  const ppc64_asm::DecodedInstruction di(inst);
  if (!di.isCall()) return nullptr;

  return di.farBranchTarget();
}

static TCA smashableBranchTarget(TCA inst, bool allowCond) {
  const ppc64_asm::DecodedInstruction di(inst);
  if (!di.isBranch(allowCond)) return nullptr;

  return di.farBranchTarget();
}

TCA smashableJmpTarget(TCA inst) {
  return smashableBranchTarget(inst, false);
}

TCA smashableJccTarget(TCA inst) {
  return smashableBranchTarget(inst, true);
}

ConditionCode smashableJccCond(TCA inst) {
  // To analyse the cc, it has to be on the bcctr so we need go backward one
  // instruction.
  uint8_t jccLen = smashableJccLen() - kStdIns;
  // skip to the branch instruction in order to get its condition
  ppc64_asm::BranchParams bp(reinterpret_cast<PPC64Instr*>(inst + jccLen));
  return bp;
}

///////////////////////////////////////////////////////////////////////////////

}}}
