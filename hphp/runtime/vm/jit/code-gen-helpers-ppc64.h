/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2014 Facebook, Inc. (http://www.facebook.com)     |
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

#ifndef incl_HPHP_VM_CODEGENHELPERS_PPC64_H_
#define incl_HPHP_VM_CODEGENHELPERS_PPC64_H_

#include "hphp/util/asm-ppc64.h"
#include "hphp/util/ringbuffer.h"

#include "hphp/runtime/base/types.h"
#include "hphp/runtime/vm/jit/abi-ppc64.h"
#include "hphp/runtime/vm/jit/code-gen-cf.h"
#include "hphp/runtime/vm/jit/code-gen-helpers.h"
#include "hphp/runtime/vm/jit/cpp-call.h"
#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/jit/phys-reg.h"
#include "hphp/runtime/vm/jit/service-requests-ppc64.h"
#include "hphp/runtime/vm/jit/service-requests.h"
#include "hphp/runtime/vm/jit/translator.h"
#include "hphp/runtime/vm/jit/vasm-emit.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-reg.h"

namespace HPHP {
//////////////////////////////////////////////////////////////////////

struct Func;

namespace jit {
//////////////////////////////////////////////////////////////////////

struct Fixup;
struct SSATmp;

namespace ppc64 {
//////////////////////////////////////////////////////////////////////

typedef PPC64Assembler Asm;

constexpr size_t kJmpTargetAlign = 16;

void moveToAlign(CodeBlock& cb, size_t alignment = kJmpTargetAlign);

void emitEagerSyncPoint(Vout& v, const Op* pc, Vreg rds, Vreg vmfp, Vreg vmsp);
void emitGetGContext(Asm& as, PhysReg dest);
void emitGetGContext(Vout& as, Vreg dest);

void emitTransCounterInc(Asm& a);
void emitTransCounterInc(Vout&);

/*
 * Emit a decrement on the m_count field of `base', which must contain a
 * reference counted heap object.  This helper also conditionally makes some
 * sanity checks on the reference count of the object.
 *
 * Returns: the status flags register for the decrement instruction.
 */
Vreg emitDecRef(Vout& v, Vreg base);

/*
 * Assuming rData is the data pointer for a refcounted (but possibly static)
 * value, emit a static check and DecRef, executing the code emitted by
 * `destroy' if the count would go to zero.
 */
template<class Destroy>
void emitDecRefWork(Vout& v, Vout& vcold, Vreg rData,
                    Destroy destroy, bool unlikelyDestroy) {}

void emitIncRef(Asm& as, PhysReg base);
void emitIncRef(Vout& v, Vreg base);
void emitIncRefCheckNonStatic(Asm& as, PhysReg base, DataType dtype);
void emitIncRefGenericRegSafe(Asm& as, PhysReg base, int disp, PhysReg tmpReg);

void emitAssertFlagsNonNegative(Vout& v, Vreg sf);
void emitAssertRefCount(Vout& v, Vreg base);

void emitMovRegReg(Asm& as, PhysReg srcReg, PhysReg dstReg);
void emitLea(Asm& as, MemoryRef mr, PhysReg dst);

Vreg emitLdObjClass(Vout& v, Vreg objReg, Vreg dstReg);
Vreg emitLdClsCctx(Vout& v, Vreg srcReg, Vreg dstReg);

void emitCall(Asm& as, TCA dest, RegSet args);
void emitCall(Asm& as, CppCall call, RegSet args);
void emitCall(Vout& v, CppCall call, RegSet args);

// store imm to the 8-byte memory location at ref. Warning: don't use this
// if you wanted an atomic store; large imms cause two stores.
void emitImmStoreq(Vout& v, Immed64 imm, Vptr ref);
void emitImmStoreq(Asm& as, Immed64 imm, MemoryRef ref);

void emitRB(Vout& v, Trace::RingBufferType t, const char* msg);

/*
 * Test the current thread's surprise flags for a nonzero value. Should be used
 * before a jnz to surprise handling code.
 */
void emitTestSurpriseFlags(Asm& as, PhysReg rds);
Vreg emitTestSurpriseFlags(Vout& v, Vreg rds);

void emitCheckSurpriseFlagsEnter(Vout& main, Vout& cold, Vreg fp, Vreg rds,
                                 Fixup fixup, Vlabel catchBlock);
void emitCheckSurpriseFlagsEnter(CodeBlock& mainCode, CodeBlock& coldCode,
                                 PhysReg rds, Fixup fixup);

#ifdef USE_GCC_FAST_TLS

/*
 * TLS access: XXX we currently only support static-style TLS directly
 * linked off of FS.
 *
 * x86 terminology review: "Virtual addresses" are subject to both
 * segmented translation and paged translation. "Linear addresses" are
 * post-segmentation address, subject only to paging. C and C++ generally
 * only have access to bitwise linear addresses.
 *
 * TLS data live at negative virtual addresses off FS: the first datum
 * is typically at VA(FS:-sizeof(datum)). Linux's ppc64 ABI stores the linear
 * address of the base of TLS at VA(FS:0). While this is just a convention, it
 * is firm: gcc builds binaries that assume it when, e.g., evaluating
 * "&myTlsDatum".
 *
 * The virtual addresses of TLS data are not exposed to C/C++. To figure it
 * out, we take a datum's linear address, and subtract it from the linear
 * address where TLS starts.
 */
template<typename T>
inline Vptr getTLSPtr(const T& data) {
  uintptr_t virtualAddress = uintptr_t(&data) - tlsBase();
  return Vptr{baseless(virtualAddress), Vptr::FS};
}

template<typename T>
inline void
emitTLSLoad(Vout& v, const ThreadLocalNoCheck<T>& datum, Vreg reg) {
  v << load{getTLSPtr(datum.m_node.m_p), reg};
}

template<typename T>
inline void
emitTLSLoad(PPC64Assembler& a, const ThreadLocalNoCheck<T>& datum, Reg64 reg) {}

#else // USE_GCC_FAST_TLS

template<typename T>
inline void
emitTLSLoad(Vout& v, const ThreadLocalNoCheck<T>& datum, Vreg dest) {
  PhysRegSaver(v, kGPCallerSaved); // we don't know for sure what's alive
  v << ldimmq{datum.m_key, argNumToRegName[0]};
  const CodeAddress addr = (CodeAddress)pthread_getspecific;
  if (deltaFits((uintptr_t)addr, sz::dword)) {
    v << call{addr, argSet(1)};
  } else {
    v << ldimmq{addr, reg::rax};
    v << callr{reg::rax, argSet(1)};
  }
  if (dest != Vreg(reg::rax)) {
    v << copy{reg::rax, dest};
  }
}

template<typename T>
inline void
emitTLSLoad(PPC64Assembler& a, const ThreadLocalNoCheck<T>& datum, Reg64 dest) {}

#endif // USE_GCC_FAST_TLS

// Emit a load of a low pointer.
void emitLdLowPtr(Vout& v, Vptr mem, Vreg reg, size_t size);

void emitCmpClass(Vout& v, Vreg sf, const Class* c, Vptr mem);
void emitCmpClass(Vout& v, Vreg sf, Vreg reg, Vptr mem);
void emitCmpClass(Vout& v, Vreg sf, Vreg reg1, Vreg reg2);

void copyTV(Vout& v, Vloc src, Vloc dst, Type destType);
void pack2(Vout& v, Vreg s0, Vreg s1, Vreg d0);

Vreg zeroExtendIfBool(Vout& v, const SSATmp* src, Vreg reg);

template<ConditionCode Jcc, class Lambda>
void jccBlock(Asm& a, Lambda body) {}

/*
 * lookupDestructor --
 *
 * Return a MemoryRef pointer to the destructor for the type in typeReg.
 */

inline MemoryRef lookupDestructor(PPC64Assembler& a, PhysReg typeReg) {}

inline Vptr lookupDestructor(Vout& v, Vreg typeReg) {}

//////////////////////////////////////////////////////////////////////

}}}

#endif
