/*
 * Copyright (C) 2011 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "DFGJITCompiler.h"

#if ENABLE(DFG_JIT)

#include "CodeBlock.h"
#include "DFGJITCodeGenerator.h"
#include "DFGNonSpeculativeJIT.h"
#include "DFGOperations.h"
#include "DFGRegisterBank.h"
#include "DFGSpeculativeJIT.h"
#include "JSGlobalData.h"
#include "LinkBuffer.h"

namespace JSC { namespace DFG {

// This method used to fill a numeric value to a FPR when linking speculative -> non-speculative.
void JITCompiler::fillNumericToDouble(NodeIndex nodeIndex, FPRReg fpr, GPRReg temporary)
{
    Node& node = graph()[nodeIndex];

    if (node.isConstant()) {
        ASSERT(isNumberConstant(nodeIndex));
        move(MacroAssembler::ImmPtr(reinterpret_cast<void*>(reinterpretDoubleToIntptr(valueOfNumberConstant(nodeIndex)))), temporary);
        movePtrToDouble(temporary, fpr);
    } else {
        loadPtr(addressFor(node.virtualRegister()), temporary);
        Jump isInteger = branchPtr(MacroAssembler::AboveOrEqual, temporary, GPRInfo::tagTypeNumberRegister);
        unboxDouble(temporary, fpr);
        Jump hasUnboxedDouble = jump();
        isInteger.link(this);
        convertInt32ToDouble(temporary, fpr);
        hasUnboxedDouble.link(this);
    }
}

// This method used to fill an integer value to a GPR when linking speculative -> non-speculative.
void JITCompiler::fillInt32ToInteger(NodeIndex nodeIndex, GPRReg gpr)
{
    Node& node = graph()[nodeIndex];

    if (node.isConstant()) {
        ASSERT(isInt32Constant(nodeIndex));
        move(MacroAssembler::Imm32(valueOfInt32Constant(nodeIndex)), gpr);
    } else {
#if ENABLE(DFG_JIT_ASSERT)
        // Redundant load, just so we can check the tag!
        loadPtr(addressFor(node.virtualRegister()), gpr);
        jitAssertIsJSInt32(gpr);
#endif
        load32(addressFor(node.virtualRegister()), gpr);
    }
}

// This method used to fill a JSValue to a GPR when linking speculative -> non-speculative.
void JITCompiler::fillToJS(NodeIndex nodeIndex, GPRReg gpr)
{
    Node& node = graph()[nodeIndex];

    if (node.isConstant()) {
        if (isInt32Constant(nodeIndex)) {
            JSValue jsValue = jsNumber(valueOfInt32Constant(nodeIndex));
            move(MacroAssembler::ImmPtr(JSValue::encode(jsValue)), gpr);
        } else if (isNumberConstant(nodeIndex)) {
            JSValue jsValue(JSValue::EncodeAsDouble, valueOfNumberConstant(nodeIndex));
            move(MacroAssembler::ImmPtr(JSValue::encode(jsValue)), gpr);
        } else {
            ASSERT(isJSConstant(nodeIndex));
            JSValue jsValue = valueOfJSConstant(nodeIndex);
            move(MacroAssembler::ImmPtr(JSValue::encode(jsValue)), gpr);
        }
        return;
    }

    loadPtr(addressFor(node.virtualRegister()), gpr);
}

#if ENABLE(DFG_OSR_EXIT)
void JITCompiler::exitSpeculativeWithOSR(const OSRExit& exit, SpeculationRecovery* recovery, Vector<BytecodeAndMachineOffset>& decodedCodeMap)
{
    // 1) Pro-forma stuff.
    exit.m_check.link(this);

#if ENABLE(DFG_DEBUG_VERBOSE)
    fprintf(stderr, "OSR exit for Node @%d (bc#%u) at JIT offset 0x%x   ", (int)exit.m_nodeIndex, exit.m_bytecodeIndex, debugOffset());
    exit.dump(stderr);
#endif
#if ENABLE(DFG_JIT_BREAK_ON_SPECULATION_FAILURE)
    breakpoint();
#endif
    
#if ENABLE(DFG_VERBOSE_SPECULATION_FAILURE)
    SpeculationFailureDebugInfo* debugInfo = new SpeculationFailureDebugInfo;
    debugInfo->codeBlock = m_codeBlock;
    debugInfo->debugOffset = debugOffset();
    
    debugCall(debugOperationPrintSpeculationFailure, debugInfo);
#endif
    
#if ENABLE(DFG_SUCCESS_STATS)
    static SamplingCounter counter("SpeculationFailure");
    emitCount(counter);
#endif

    // 2) Perform speculation recovery. This only comes into play when an operation
    //    starts mutating state before verifying the speculation it has already made.
    
    GPRReg alreadyBoxed = InvalidGPRReg;
    
    if (recovery) {
        switch (recovery->type()) {
        case SpeculativeAdd:
            sub32(recovery->src(), recovery->dest());
            orPtr(GPRInfo::tagTypeNumberRegister, recovery->dest());
            alreadyBoxed = recovery->dest();
            break;
            
        case BooleanSpeculationCheck:
            xorPtr(TrustedImm32(static_cast<int32_t>(ValueFalse)), recovery->dest());
            break;
            
        default:
            break;
        }
    }

    // 3) Figure out how many scratch slots we'll need. We need one for every GPR/FPR
    //    whose destination is now occupied by a DFG virtual register, and we need
    //    one for every displaced virtual register if there are more than
    //    GPRInfo::numberOfRegisters of them. Also see if there are any constants,
    //    any undefined slots, any FPR slots, and any unboxed ints.
            
    Vector<bool> poisonedVirtualRegisters(exit.m_variables.size());
    for (unsigned i = 0; i < poisonedVirtualRegisters.size(); ++i)
        poisonedVirtualRegisters[i] = false;

    unsigned numberOfPoisonedVirtualRegisters = 0;
    unsigned numberOfDisplacedVirtualRegisters = 0;
    
    // Booleans for fast checks. We expect that most OSR exits do not have to rebox
    // Int32s, have no FPRs, and have no constants. If there are constants, we
    // expect most of them to be jsUndefined(); if that's true then we handle that
    // specially to minimize code size and execution time.
    bool haveUnboxedInt32s = false;
    bool haveFPRs = false;
    bool haveConstants = false;
    bool haveUndefined = false;
    
    for (int index = 0; index < exit.numberOfRecoveries(); ++index) {
        const ValueRecovery& recovery = exit.valueRecovery(index);
        switch (recovery.technique()) {
        case DisplacedInRegisterFile:
            numberOfDisplacedVirtualRegisters++;
            ASSERT((int)recovery.virtualRegister() >= 0);
            
            // See if we might like to store to this virtual register before doing
            // virtual register shuffling. If so, we say that the virtual register
            // is poisoned: it cannot be stored to until after displaced virtual
            // registers are handled. We track poisoned virtual register carefully
            // to ensure this happens efficiently. Note that we expect this case
            // to be rare, so the handling of it is optimized for the cases in
            // which it does not happen.
            if (recovery.virtualRegister() < (int)exit.m_variables.size()) {
                switch (exit.m_variables[recovery.virtualRegister()].technique()) {
                case InGPR:
                case UnboxedInt32InGPR:
                case InFPR:
                    if (!poisonedVirtualRegisters[recovery.virtualRegister()]) {
                        poisonedVirtualRegisters[recovery.virtualRegister()] = true;
                        numberOfPoisonedVirtualRegisters++;
                    }
                    break;
                default:
                    break;
                }
            }
            break;
            
        case UnboxedInt32InGPR:
            haveUnboxedInt32s = true;
            break;
            
        case InFPR:
            haveFPRs = true;
            break;
            
        case Constant:
            haveConstants = true;
            if (recovery.constant().isUndefined())
                haveUndefined = true;
            break;
            
        default:
            break;
        }
    }
    
    EncodedJSValue* scratchBuffer = static_cast<EncodedJSValue*>(globalData()->osrScratchBufferForSize(sizeof(EncodedJSValue) * (numberOfPoisonedVirtualRegisters + (numberOfDisplacedVirtualRegisters <= GPRInfo::numberOfRegisters ? 0 : numberOfDisplacedVirtualRegisters))));

    // From here on, the code assumes that it is profitable to maximize the distance
    // between when something is computed and when it is stored.
    
    // 4) Perform all reboxing of integers.
    
    if (haveUnboxedInt32s) {
        for (int index = 0; index < exit.numberOfRecoveries(); ++index) {
            const ValueRecovery& recovery = exit.valueRecovery(index);
            if (recovery.technique() == UnboxedInt32InGPR && recovery.gpr() != alreadyBoxed)
                orPtr(GPRInfo::tagTypeNumberRegister, recovery.gpr());
        }
    }
    
    // 5) Dump all non-poisoned GPRs. For poisoned GPRs, save them into the scratch storage.
    //    Note that GPRs do not have a fast change (like haveFPRs) because we expect that
    //    most OSR failure points will have at least one GPR that needs to be dumped.
    
    unsigned scratchIndex = 0;
    for (int index = 0; index < exit.numberOfRecoveries(); ++index) {
        const ValueRecovery& recovery = exit.valueRecovery(index);
        int operand = exit.operandForIndex(index);
        switch (recovery.technique()) {
        case InGPR:
        case UnboxedInt32InGPR:
            if (exit.isVariable(index) && poisonedVirtualRegisters[exit.variableForIndex(index)])
                storePtr(recovery.gpr(), scratchBuffer + scratchIndex++);
            else
                storePtr(recovery.gpr(), addressFor((VirtualRegister)operand));
            break;
        default:
            break;
        }
    }
    
    // At this point all GPRs are available for scratch use.
    
    if (haveFPRs) {
        // 6) Box all doubles (relies on there being more GPRs than FPRs)
        
        for (int index = 0; index < exit.numberOfRecoveries(); ++index) {
            const ValueRecovery& recovery = exit.valueRecovery(index);
            if (recovery.technique() != InFPR)
                continue;
            FPRReg fpr = recovery.fpr();
            GPRReg gpr = GPRInfo::toRegister(FPRInfo::toIndex(fpr));
            boxDouble(fpr, gpr);
        }
        
        // 7) Dump all doubles into the register file, or to the scratch storage if
        //    the destination virtual register is poisoned.
        
        for (int index = 0; index < exit.numberOfRecoveries(); ++index) {
            const ValueRecovery& recovery = exit.valueRecovery(index);
            if (recovery.technique() != InFPR)
                continue;
            GPRReg gpr = GPRInfo::toRegister(FPRInfo::toIndex(recovery.fpr()));
            if (exit.isVariable(index) && poisonedVirtualRegisters[exit.variableForIndex(index)])
                storePtr(gpr, scratchBuffer + scratchIndex++);
            else
                storePtr(gpr, addressFor((VirtualRegister)exit.operandForIndex(index)));
        }
    }
    
    ASSERT(scratchIndex == numberOfPoisonedVirtualRegisters);
    
    // 8) Reshuffle displaced virtual registers. Optimize for the case that
    //    the number of displaced virtual registers is not more than the number
    //    of available physical registers.
    
    if (numberOfDisplacedVirtualRegisters) {
        if (numberOfDisplacedVirtualRegisters <= GPRInfo::numberOfRegisters) {
            // So far this appears to be the case that triggers all the time, but
            // that is far from guaranteed.
        
            unsigned displacementIndex = 0;
            for (int index = 0; index < exit.numberOfRecoveries(); ++index) {
                const ValueRecovery& recovery = exit.valueRecovery(index);
                if (recovery.technique() != DisplacedInRegisterFile)
                    continue;
                loadPtr(addressFor(recovery.virtualRegister()), GPRInfo::toRegister(displacementIndex++));
            }
        
            displacementIndex = 0;
            for (int index = 0; index < exit.numberOfRecoveries(); ++index) {
                const ValueRecovery& recovery = exit.valueRecovery(index);
                if (recovery.technique() != DisplacedInRegisterFile)
                    continue;
                storePtr(GPRInfo::toRegister(displacementIndex++), addressFor((VirtualRegister)exit.operandForIndex(index)));
            }
        } else {
            // FIXME: This should use the shuffling algorithm that we use
            // for speculative->non-speculative jumps, if we ever discover that
            // some hot code with lots of live values that get displaced and
            // spilled really enjoys frequently failing speculation.
        
            // For now this code is engineered to be correct but probably not
            // super. In particular, it correctly handles cases where for example
            // the displacements are a permutation of the destination values, like
            //
            // 1 -> 2
            // 2 -> 1
            //
            // It accomplishes this by simply lifting all of the virtual registers
            // from their old (DFG JIT) locations and dropping them in a scratch
            // location in memory, and then transferring from that scratch location
            // to their new (old JIT) locations.
        
            for (int index = 0; index < exit.numberOfRecoveries(); ++index) {
                const ValueRecovery& recovery = exit.valueRecovery(index);
                if (recovery.technique() != DisplacedInRegisterFile)
                    continue;
                loadPtr(addressFor(recovery.virtualRegister()), GPRInfo::regT0);
                storePtr(GPRInfo::regT0, scratchBuffer + scratchIndex++);
            }
        
            scratchIndex = numberOfPoisonedVirtualRegisters;
            for (int index = 0; index < exit.numberOfRecoveries(); ++index) {
                const ValueRecovery& recovery = exit.valueRecovery(index);
                if (recovery.technique() != DisplacedInRegisterFile)
                    continue;
                loadPtr(scratchBuffer + scratchIndex++, GPRInfo::regT0);
                storePtr(GPRInfo::regT0, addressFor((VirtualRegister)exit.operandForIndex(index)));
            }
        
            ASSERT(scratchIndex == numberOfPoisonedVirtualRegisters + numberOfDisplacedVirtualRegisters);
        }
    }
    
    // 9) Dump all poisoned virtual registers.
    
    scratchIndex = 0;
    if (numberOfPoisonedVirtualRegisters) {
        for (int virtualRegister = 0; virtualRegister < (int)exit.m_variables.size(); ++virtualRegister) {
            if (!poisonedVirtualRegisters[virtualRegister])
                continue;
            
            const ValueRecovery& recovery = exit.m_variables[virtualRegister];
            switch (recovery.technique()) {
            case InGPR:
            case UnboxedInt32InGPR:
            case InFPR:
                loadPtr(scratchBuffer + scratchIndex++, GPRInfo::regT0);
                storePtr(GPRInfo::regT0, addressFor((VirtualRegister)virtualRegister));
                break;
                
            default:
                break;
            }
        }
    }
    ASSERT(scratchIndex == numberOfPoisonedVirtualRegisters);
    
    // 10) Dump all constants. Optimize for Undefined, since that's a constant we see
    //     often.

    if (haveConstants) {
        if (haveUndefined)
            move(TrustedImmPtr(JSValue::encode(jsUndefined())), GPRInfo::regT0);
        
        for (int index = 0; index < exit.numberOfRecoveries(); ++index) {
            const ValueRecovery& recovery = exit.valueRecovery(index);
            if (recovery.technique() != Constant)
                continue;
            if (recovery.constant().isUndefined())
                storePtr(GPRInfo::regT0, addressFor((VirtualRegister)exit.operandForIndex(index)));
            else
                storePtr(TrustedImmPtr(JSValue::encode(recovery.constant())), addressFor((VirtualRegister)exit.operandForIndex(index)));
        }
    }
    
    // 11) Adjust the old JIT's execute counter. Since we are exiting OSR, we know
    //     that all new calls into this code will go to the new JIT, so the execute
    //     counter only affects call frames that performed OSR exit and call frames
    //     that were still executing the old JIT at the time of another call frame's
    //     OSR exit. We want to ensure that the following is true:
    //
    //     (a) Code the performs an OSR exit gets a chance to reenter optimized
    //         code eventually, since optimized code is faster. But we don't
    //         want to do such reentery too aggressively (see (c) below).
    //
    //     (b) If there is code on the call stack that is still running the old
    //         JIT's code and has never OSR'd, then it should get a chance to
    //         perform OSR entry despite the fact that we've exited.
    //
    //     (c) Code the performs an OSR exit should not immediately retry OSR
    //         entry, since both forms of OSR are expensive. OSR entry is
    //         particularly expensive.
    //
    //     To ensure (c), we'd like to set the execute counter to
    //     counterValueForOptimizeAfterWarmUp(). This seems like it would endanger
    //     (a) and (b), since then every OSR exit would delay the opportunity for
    //     every call frame to perform OSR entry. Essentially, if OSR exit happens
    //     frequently and the function has few loops, then the counter will never
    //     become non-negative and OSR entry will never be triggered. OSR entry
    //     will only happen if a loop gets hot in the old JIT, which does a pretty
    //     good job of ensuring (a) and (b). This heuristic may need to be
    //     rethought in the future, particularly if we support reoptimizing code
    //     with new value profiles gathered from code that did OSR exit.
    
    store32(Imm32(codeBlock()->alternative()->counterValueForOptimizeAfterWarmUp()), codeBlock()->alternative()->addressOfExecuteCounter());
    
    // 12) Load the result of the last bytecode operation into regT0.
    
    if (exit.m_lastSetOperand != std::numeric_limits<int>::max())
        loadPtr(addressFor((VirtualRegister)exit.m_lastSetOperand), GPRInfo::cachedResultRegister);
    
    // 13) Fix call frame.
    
    ASSERT(codeBlock()->alternative()->getJITType() == JITCode::BaselineJIT);
    storePtr(TrustedImmPtr(codeBlock()->alternative()), addressFor((VirtualRegister)RegisterFile::CodeBlock));
    
    // 14) Jump into the corresponding baseline JIT code.
    
    BytecodeAndMachineOffset* mapping = binarySearch<BytecodeAndMachineOffset, unsigned, BytecodeAndMachineOffset::getBytecodeIndex>(decodedCodeMap.begin(), decodedCodeMap.size(), exit.m_bytecodeIndex);
    
    ASSERT(mapping);
    ASSERT(mapping->m_bytecodeIndex == exit.m_bytecodeIndex);
    
    void* jumpTarget = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(codeBlock()->alternative()->getJITCode().start()) + mapping->m_machineCodeOffset);
    
    ASSERT(GPRInfo::regT1 != GPRInfo::cachedResultRegister);
    
    move(TrustedImmPtr(jumpTarget), GPRInfo::regT1);
    jump(GPRInfo::regT1);

#if ENABLE(DFG_DEBUG_VERBOSE)
    fprintf(stderr, "   -> %p\n", jumpTarget);
#endif
}

void JITCompiler::linkOSRExits(SpeculativeJIT& speculative)
{
    Vector<BytecodeAndMachineOffset> decodedCodeMap;
    ASSERT(codeBlock()->alternative());
    ASSERT(codeBlock()->alternative()->getJITType() == JITCode::BaselineJIT);
    ASSERT(codeBlock()->alternative()->jitCodeMap());
    codeBlock()->alternative()->jitCodeMap()->decode(decodedCodeMap);
    
    OSRExitVector::Iterator exitsIter = speculative.osrExits().begin();
    OSRExitVector::Iterator exitsEnd = speculative.osrExits().end();
    
    while (exitsIter != exitsEnd) {
        const OSRExit& exit = *exitsIter;
        exitSpeculativeWithOSR(exit, speculative.speculationRecovery(exit.m_recoveryIndex), decodedCodeMap);
        ++exitsIter;
    }
}
#else // ENABLE(DFG_OSR_EXIT)
class GeneralizedRegister {
public:
    GeneralizedRegister() { }
    
    static GeneralizedRegister createGPR(GPRReg gpr)
    {
        GeneralizedRegister result;
        result.m_isFPR = false;
        result.m_register.gpr = gpr;
        return result;
    }
    
    static GeneralizedRegister createFPR(FPRReg fpr)
    {
        GeneralizedRegister result;
        result.m_isFPR = true;
        result.m_register.fpr = fpr;
        return result;
    }
    
    bool isFPR() const
    {
        return m_isFPR;
    }
    
    GPRReg gpr() const
    {
        ASSERT(!m_isFPR);
        return m_register.gpr;
    }
    
    FPRReg fpr() const
    {
        ASSERT(m_isFPR);
        return m_register.fpr;
    }
    
    const SpeculationCheck::RegisterInfo& findInSpeculationCheck(const SpeculationCheck& check)
    {
        if (isFPR())
            return check.m_fprInfo[FPRInfo::toIndex(fpr())];
        return check.m_gprInfo[GPRInfo::toIndex(gpr())];
    }
    
    const EntryLocation::RegisterInfo& findInEntryLocation(const EntryLocation& entry)
    {
        if (isFPR())
            return entry.m_fprInfo[FPRInfo::toIndex(fpr())];
        return entry.m_gprInfo[GPRInfo::toIndex(gpr())];
    }
    
    DataFormat previousDataFormat(const SpeculationCheck& check)
    {
        return findInSpeculationCheck(check).format;
    }
    
    DataFormat nextDataFormat(const EntryLocation& entry)
    {
        return findInEntryLocation(entry).format;
    }
    
    void convert(DataFormat oldDataFormat, DataFormat newDataFormat, JITCompiler& jit)
    {
        if (LIKELY(!needDataFormatConversion(oldDataFormat, newDataFormat)))
            return;
        
        if (oldDataFormat == DataFormatInteger) {
            jit.orPtr(GPRInfo::tagTypeNumberRegister, gpr());
            return;
        }
        
        ASSERT(newDataFormat == DataFormatInteger);
        jit.zeroExtend32ToPtr(gpr(), gpr());
        return;
    }
    
    void moveTo(GeneralizedRegister& other, DataFormat myDataFormat, DataFormat otherDataFormat, JITCompiler& jit, FPRReg scratchFPR)
    {
        if (UNLIKELY(isFPR())) {
            if (UNLIKELY(other.isFPR())) {
                jit.moveDouble(fpr(), other.fpr());
                return;
            }
            
            JITCompiler::Jump done;
            
            if (scratchFPR != InvalidFPRReg) {
                // we have a scratch FPR, so attempt a conversion to int
                JITCompiler::JumpList notInt;
                jit.branchConvertDoubleToInt32(fpr(), other.gpr(), notInt, scratchFPR);
                jit.orPtr(GPRInfo::tagTypeNumberRegister, other.gpr());
                done = jit.jump();
                notInt.link(&jit);
            }
            
            jit.boxDouble(fpr(), other.gpr());
            
            if (done.isSet())
                done.link(&jit);
            return;
        }
        
        if (UNLIKELY(other.isFPR())) {
            jit.unboxDouble(gpr(), other.fpr());
            return;
        }
        
        if (LIKELY(!needDataFormatConversion(myDataFormat, otherDataFormat))) {
            jit.move(gpr(), other.gpr());
            return;
        }
        
        if (myDataFormat == DataFormatInteger) {
            jit.orPtr(gpr(), GPRInfo::tagTypeNumberRegister, other.gpr());
            return;
        }
        
        ASSERT(otherDataFormat == DataFormatInteger);
        jit.zeroExtend32ToPtr(gpr(), other.gpr());
    }
    
    void swapWith(GeneralizedRegister& other, DataFormat myDataFormat, DataFormat myNewDataFormat, DataFormat otherDataFormat, DataFormat otherNewDataFormat, JITCompiler& jit, GPRReg scratchGPR, FPRReg scratchFPR)
    {
        if (UNLIKELY(isFPR())) {
            if (UNLIKELY(other.isFPR())) {
                if (scratchFPR == InvalidFPRReg)
                    jit.moveDoubleToPtr(fpr(), scratchGPR);
                else
                    jit.moveDouble(fpr(), scratchFPR);
                jit.moveDouble(other.fpr(), fpr());
                if (scratchFPR == InvalidFPRReg)
                    jit.movePtrToDouble(scratchGPR, other.fpr());
                else
                    jit.moveDouble(scratchFPR, other.fpr());
                return;
            }
            
            jit.move(other.gpr(), scratchGPR);
            
            JITCompiler::Jump done;
            
            if (scratchFPR != InvalidFPRReg) {
                JITCompiler::JumpList notInt;
                jit.branchConvertDoubleToInt32(fpr(), other.gpr(), notInt, scratchFPR);
                jit.orPtr(GPRInfo::tagTypeNumberRegister, other.gpr());
                done = jit.jump();
                notInt.link(&jit);
            }
            
            jit.boxDouble(fpr(), other.gpr());
            
            if (done.isSet())
                done.link(&jit);
            
            jit.unboxDouble(scratchGPR, fpr());
            return;
        }
        
        if (UNLIKELY(other.isFPR())) {
            other.swapWith(*this, otherDataFormat, otherNewDataFormat, myDataFormat, myNewDataFormat, jit, scratchGPR, scratchFPR);
            return;
        }
        
        jit.swap(gpr(), other.gpr());
        
        if (UNLIKELY(needDataFormatConversion(otherDataFormat, myNewDataFormat))) {
            if (otherDataFormat == DataFormatInteger)
                jit.orPtr(GPRInfo::tagTypeNumberRegister, gpr());
            else if (myNewDataFormat == DataFormatInteger)
                jit.zeroExtend32ToPtr(gpr(), gpr());
        }
        
        if (UNLIKELY(needDataFormatConversion(myDataFormat, otherNewDataFormat))) {
            if (myDataFormat == DataFormatInteger)
                jit.orPtr(GPRInfo::tagTypeNumberRegister, other.gpr());
            else if (otherNewDataFormat == DataFormatInteger)
                jit.zeroExtend32ToPtr(other.gpr(), other.gpr());
        }
    }

private:
    bool m_isFPR;
    union {
        GPRReg gpr;
        FPRReg fpr;
    } m_register;
};

struct ShuffledRegister {
    GeneralizedRegister reg;
    ShuffledRegister* previous;
    bool hasFrom;
    bool hasTo;
    bool handled;
    
    ShuffledRegister() { }
    
    ShuffledRegister(GeneralizedRegister reg)
        : reg(reg)
        , previous(0)
        , hasFrom(false)
        , hasTo(false)
        , handled(false)
    {
    }
    
    bool isEndOfNonCyclingPermutation()
    {
        return hasTo && !hasFrom;
    }
    
    void handleNonCyclingPermutation(const SpeculationCheck& check, const EntryLocation& entry, JITCompiler& jit, FPRReg& scratchFPR1, FPRReg& scratchFPR2)
    {
        ShuffledRegister* cur = this;
        while (cur->previous) {
            cur->previous->reg.moveTo(cur->reg, cur->previous->reg.previousDataFormat(check), cur->reg.nextDataFormat(entry), jit, scratchFPR1);
            cur->handled = true;
            if (cur->reg.isFPR()) {
                if (scratchFPR1 == InvalidFPRReg)
                    scratchFPR1 = cur->reg.fpr();
                else {
                    ASSERT(scratchFPR1 != cur->reg.fpr());
                    scratchFPR2 = cur->reg.fpr();
                }
            }
            cur = cur->previous;
        }
        cur->handled = true;
        if (cur->reg.isFPR()) {
            if (scratchFPR1 == InvalidFPRReg)
                scratchFPR1 = cur->reg.fpr();
            else {
                ASSERT(scratchFPR1 != cur->reg.fpr());
                scratchFPR2 = cur->reg.fpr();
            }
        }
    }
    
    void handleCyclingPermutation(const SpeculationCheck& check, const EntryLocation& entry, JITCompiler& jit, GPRReg scratchGPR, FPRReg scratchFPR1, FPRReg scratchFPR2)
    {
        // first determine the cycle length
        
        unsigned cycleLength = 0;
        
        ShuffledRegister* cur = this;
        ShuffledRegister* next = 0;
        do {
            ASSERT(cur);
            cycleLength++;
            cur->handled = true;
            next = cur;
            cur = cur->previous;
        } while (cur != this);
        
        ASSERT(cycleLength);
        ASSERT(next->previous == cur);
        
        // now determine the best way to handle the permutation, depending on the
        // length.
        
        switch (cycleLength) {
        case 1:
            reg.convert(reg.previousDataFormat(check), reg.nextDataFormat(entry), jit);
            break;
            
        case 2:
            reg.swapWith(previous->reg, reg.previousDataFormat(check), reg.nextDataFormat(entry), previous->reg.previousDataFormat(check), previous->reg.nextDataFormat(entry), jit, scratchGPR, scratchFPR1);
            break;
            
        default:
            GeneralizedRegister scratch;
            if (UNLIKELY(reg.isFPR() && next->reg.isFPR())) {
                if (scratchFPR2 == InvalidFPRReg) {
                    scratch = GeneralizedRegister::createGPR(scratchGPR);
                    reg.moveTo(scratch, DataFormatDouble, DataFormatJSDouble, jit, scratchFPR1);
                } else {
                    scratch = GeneralizedRegister::createFPR(scratchFPR2);
                    reg.moveTo(scratch, DataFormatDouble, DataFormatDouble, jit, scratchFPR1);
                }
            } else {
                scratch = GeneralizedRegister::createGPR(scratchGPR);
                reg.moveTo(scratch, reg.previousDataFormat(check), next->reg.nextDataFormat(entry), jit, scratchFPR1);
            }
            
            cur = this;
            while (cur->previous != this) {
                ASSERT(cur);
                cur->previous->reg.moveTo(cur->reg, cur->previous->reg.previousDataFormat(check), cur->reg.nextDataFormat(entry), jit, scratchFPR1);
                cur = cur->previous;
            }
            
            if (UNLIKELY(reg.isFPR() && next->reg.isFPR())) {
                if (scratchFPR2 == InvalidFPRReg)
                    scratch.moveTo(next->reg, DataFormatJSDouble, DataFormatDouble, jit, scratchFPR1);
                else
                    scratch.moveTo(next->reg, DataFormatDouble, DataFormatDouble, jit, scratchFPR1);
            } else
                scratch.moveTo(next->reg, next->reg.nextDataFormat(entry), next->reg.nextDataFormat(entry), jit, scratchFPR1);
            break;
        }
    }
    
    static ShuffledRegister* lookup(ShuffledRegister* gprs, ShuffledRegister* fprs, GeneralizedRegister& reg)
    {
        if (reg.isFPR())
            return fprs + FPRInfo::toIndex(reg.fpr());
        return gprs + GPRInfo::toIndex(reg.gpr());
    }
};

template<typename T>
T& lookupForRegister(T* gprs, T* fprs, unsigned index)
{
    ASSERT(index < GPRInfo::numberOfRegisters + FPRInfo::numberOfRegisters);
    if (index < GPRInfo::numberOfRegisters)
        return gprs[index];
    return fprs[index - GPRInfo::numberOfRegisters];
}

// This is written in a way that allows for a HashMap<NodeIndex, GeneralizedRegister> to be
// easily substituted, if it is found to be wise to do so. So far performance measurements
// indicate that this is faster, likely because the HashMap would have never grown very big
// and we would thus be wasting time performing complex hashing logic that, though O(1) on
// average, would be less than the ~7 loop iterations that the find() method below would do
// (since it's uncommon that we'd have register allocated more than 7 registers, in the
// current scheme).
class NodeToRegisterMap {
public:
    struct Tuple {
        NodeIndex first;
        GeneralizedRegister second;
        
        Tuple()
        {
        }
    };
    
    typedef Tuple* iterator;
    
    NodeToRegisterMap()
        : m_occupancy(0)
    {
    }
    
    void set(NodeIndex first, GeneralizedRegister second)
    {
        m_payload[m_occupancy].first = first;
        m_payload[m_occupancy].second = second;
        m_occupancy++;
    }
    
    Tuple* end()
    {
        return 0;
    }
    
    Tuple* find(NodeIndex first)
    {
        for (unsigned i = m_occupancy; i-- > 0;) {
            if (m_payload[i].first == first)
                return m_payload + i;
        }
        return 0;
    }
    
    void clear()
    {
        m_occupancy = 0;
    }
    
private:
    Tuple m_payload[GPRInfo::numberOfRegisters + FPRInfo::numberOfRegisters];
    unsigned m_occupancy;
};

void JITCompiler::jumpFromSpeculativeToNonSpeculative(const SpeculationCheck& check, const EntryLocation& entry, SpeculationRecovery* recovery, NodeToRegisterMap& checkNodeToRegisterMap, NodeToRegisterMap& entryNodeToRegisterMap)
{
    ASSERT(check.m_nodeIndex == entry.m_nodeIndex);

    // Link the jump from the Speculative path to here.
    check.m_check.link(this);

#if ENABLE(DFG_DEBUG_VERBOSE)
    fprintf(stderr, "Speculation failure for Node @%d at JIT offset 0x%x\n", (int)check.m_nodeIndex, debugOffset());
#endif
#if ENABLE(DFG_JIT_BREAK_ON_SPECULATION_FAILURE)
    breakpoint();
#endif
    
#if ENABLE(DFG_VERBOSE_SPECULATION_FAILURE)
    SpeculationFailureDebugInfo* debugInfo = new SpeculationFailureDebugInfo;
    debugInfo->codeBlock = m_codeBlock;
    debugInfo->debugOffset = debugOffset();
    
    debugCall(debugOperationPrintSpeculationFailure, debugInfo);
#endif

#if ENABLE(DFG_SUCCESS_STATS)
    static SamplingCounter counter("SpeculationFailure");
    emitCount(counter);
#endif

    // Does this speculation check require any additional recovery to be performed,
    // to restore any state that has been overwritten before we enter back in to the
    // non-speculative path.
    if (recovery) {
        switch (recovery->type()) {
        case SpeculativeAdd: {
            ASSERT(check.m_gprInfo[GPRInfo::toIndex(recovery->dest())].nodeIndex != NoNode);
            // Revert the add.
            sub32(recovery->src(), recovery->dest());
            
            // If recovery->dest() should have been boxed prior to the addition, then rebox
            // it.
            DataFormat format = check.m_gprInfo[GPRInfo::toIndex(recovery->dest())].format;
            ASSERT(format == DataFormatInteger || format == DataFormatJSInteger || format == DataFormatJS);
            if (format != DataFormatInteger)
                orPtr(GPRInfo::tagTypeNumberRegister, recovery->dest());
            break;
        }
            
        case BooleanSpeculationCheck: {
            ASSERT(check.m_gprInfo[GPRInfo::toIndex(recovery->dest())].nodeIndex != NoNode);
            // Rebox the (non-)boolean
            xorPtr(TrustedImm32(static_cast<int32_t>(ValueFalse)), recovery->dest());
            break;
        }
            
        default:
            ASSERT_NOT_REACHED();
            break;
        }
    }
    
    // First, we need a reverse mapping that tells us, for a NodeIndex, which register
    // that node is in.
    
    checkNodeToRegisterMap.clear();
    entryNodeToRegisterMap.clear();
    
    GPRReg scratchGPR = InvalidGPRReg;
    FPRReg scratchFPR1 = InvalidFPRReg;
    FPRReg scratchFPR2 = InvalidFPRReg;
    bool needToRestoreTagMaskRegister = false;
    
    for (unsigned index = 0; index < GPRInfo::numberOfRegisters; ++index) {
        NodeIndex nodeIndexInCheck = check.m_gprInfo[index].nodeIndex;
        if (nodeIndexInCheck != NoNode)
            checkNodeToRegisterMap.set(nodeIndexInCheck, GeneralizedRegister::createGPR(GPRInfo::toRegister(index)));
        NodeIndex nodeIndexInEntry = entry.m_gprInfo[index].nodeIndex;
        if (nodeIndexInEntry != NoNode)
            entryNodeToRegisterMap.set(nodeIndexInEntry, GeneralizedRegister::createGPR(GPRInfo::toRegister(index)));
        else if (nodeIndexInCheck == NoNode)
            scratchGPR = GPRInfo::toRegister(index);
    }
    
    for (unsigned index = 0; index < FPRInfo::numberOfRegisters; ++index) {
        NodeIndex nodeIndexInCheck = check.m_fprInfo[index].nodeIndex;
        if (nodeIndexInCheck != NoNode)
            checkNodeToRegisterMap.set(nodeIndexInCheck, GeneralizedRegister::createFPR(FPRInfo::toRegister(index)));
        NodeIndex nodeIndexInEntry = entry.m_fprInfo[index].nodeIndex;
        if (nodeIndexInEntry != NoNode)
            entryNodeToRegisterMap.set(nodeIndexInEntry, GeneralizedRegister::createFPR(FPRInfo::toRegister(index)));
        else if (nodeIndexInCheck == NoNode) {
            if (scratchFPR1 == InvalidFPRReg)
                scratchFPR1 = FPRInfo::toRegister(index);
            else
                scratchFPR2 = FPRInfo::toRegister(index);
        }
    }
    
    ASSERT((scratchFPR1 == InvalidFPRReg && scratchFPR2 == InvalidFPRReg) || (scratchFPR1 != scratchFPR2));
    
    // How this works:
    // 1) Spill any values that are not spilled on speculative, but are spilled
    //    on non-speculative.
    // 2) For the set of nodes that are in registers on both paths, perform a
    //    shuffling.
    // 3) Fill any values that were spilled on speculative, but are not spilled
    //    on non-speculative.
    
    // If we find registers that can be used as scratch registers along the way,
    // save them.
    
    // Part 1: spill any values that are not spilled on speculative, but are
    //         spilled on non-speculative.
    
    // This also sets up some data structures that Part 2 will need.
    
    ShuffledRegister gprs[GPRInfo::numberOfRegisters];
    ShuffledRegister fprs[FPRInfo::numberOfRegisters];
    
    for (unsigned index = 0; index < GPRInfo::numberOfRegisters; ++index)
        gprs[index] = ShuffledRegister(GeneralizedRegister::createGPR(GPRInfo::toRegister(index)));
    for (unsigned index = 0; index < FPRInfo::numberOfRegisters; ++index)
        fprs[index] = ShuffledRegister(GeneralizedRegister::createFPR(FPRInfo::toRegister(index)));

    for (unsigned index = 0; index < GPRInfo::numberOfRegisters; ++index) {
        NodeIndex nodeIndex = check.m_gprInfo[index].nodeIndex;
        
        // Bail out if this register isn't assigned to anything.
        if (nodeIndex == NoNode)
            continue;
        
        // If the non-speculative path also has a register for the nodeIndex that this
        // register stores, link them together.
        NodeToRegisterMap::iterator mapIterator = entryNodeToRegisterMap.find(nodeIndex);
        if (mapIterator != entryNodeToRegisterMap.end()) {
            gprs[index].hasFrom = true;
            
            ShuffledRegister* next = ShuffledRegister::lookup(gprs, fprs, mapIterator->second);
            next->previous = gprs + index;
            next->hasTo = true;
            
            // If the non-speculative path has not spilled this register, then skip the spillin
            // part below regardless of whether or not the speculative path has spilled it.
            if (!mapIterator->second.findInEntryLocation(entry).isSpilled)
                continue;
        } else {
            // If the non-speculative entry isn't using this register and it does not need
            // the value in this register to be placed into any other register, then this
            // register can be used for scratch.
            if (entry.m_gprInfo[index].nodeIndex == NoNode)
                scratchGPR = GPRInfo::toRegister(index);
        }
        
        // If the speculative path has already spilled the register then there is no need to
        // spill it.
        if (check.m_gprInfo[index].isSpilled)
            continue;
        
        DataFormat dataFormat = check.m_gprInfo[index].format;
        VirtualRegister virtualRegister = graph()[nodeIndex].virtualRegister();
        
        ASSERT(dataFormat == DataFormatInteger || DataFormatCell || dataFormat & DataFormatJS);
        if (dataFormat == DataFormatInteger)
            orPtr(GPRInfo::tagTypeNumberRegister, GPRInfo::toRegister(index));
        storePtr(GPRInfo::toRegister(index), addressFor(virtualRegister));
    }
    
    if (scratchGPR == InvalidGPRReg) {
        scratchGPR = GPRInfo::tagMaskRegister;
        needToRestoreTagMaskRegister = true;
    }

    for (unsigned index = 0; index < FPRInfo::numberOfRegisters; ++index) {
        NodeIndex nodeIndex = check.m_fprInfo[index].nodeIndex;
        if (nodeIndex == NoNode)
            continue;

        NodeToRegisterMap::iterator mapIterator = entryNodeToRegisterMap.find(nodeIndex);
        if (mapIterator != entryNodeToRegisterMap.end()) {
            fprs[index].hasFrom = true;
            
            ShuffledRegister* next = ShuffledRegister::lookup(gprs, fprs, mapIterator->second);
            next->previous = fprs + index;
            next->hasTo = true;

            if (!mapIterator->second.findInEntryLocation(entry).isSpilled)
                continue;
        } else {
            // If the non-speculative entry isn't using this register and it does not need
            // the value in this register to be placed into any other register, then this
            // register can be used for scratch.
            if (entry.m_fprInfo[index].nodeIndex == NoNode) {
                if (scratchFPR1 == InvalidFPRReg)
                    scratchFPR1 = FPRInfo::toRegister(index);
                else if (scratchFPR2)
                    scratchFPR2 = FPRInfo::toRegister(index);
                ASSERT((scratchFPR1 == InvalidFPRReg && scratchFPR2 == InvalidFPRReg) || (scratchFPR1 != scratchFPR2));
            }
        }
        
        if (check.m_fprInfo[index].isSpilled)
            continue;

        VirtualRegister virtualRegister = graph()[nodeIndex].virtualRegister();

        moveDoubleToPtr(FPRInfo::toRegister(index), scratchGPR);
        subPtr(GPRInfo::tagTypeNumberRegister, scratchGPR);
        storePtr(scratchGPR, addressFor(virtualRegister));
    }
    
#if !ASSERT_DISABLED
    // Assert that we've not assigned a scratch register to something that we're going to shuffle.
    ASSERT(scratchGPR != InvalidGPRReg);
    if (scratchGPR != GPRInfo::tagMaskRegister) {
        ASSERT(!gprs[GPRInfo::toIndex(scratchGPR)].hasTo);
        ASSERT(!gprs[GPRInfo::toIndex(scratchGPR)].hasFrom);
    }
    if (scratchFPR1 != InvalidFPRReg) {
        ASSERT(scratchFPR1 != scratchFPR2);
        ASSERT(!fprs[FPRInfo::toIndex(scratchFPR1)].hasTo);
        ASSERT(!fprs[FPRInfo::toIndex(scratchFPR1)].hasFrom);
        if (scratchFPR2 != InvalidFPRReg) {
            ASSERT(!fprs[FPRInfo::toIndex(scratchFPR2)].hasTo);
            ASSERT(!fprs[FPRInfo::toIndex(scratchFPR2)].hasFrom);
        }
    } else
        ASSERT(scratchFPR2 == InvalidFPRReg);
#endif
    
    // Part 2: For the set of nodes that are in registers on both paths,
    //         perform a shuffling.
    
    for (unsigned index = 0; index < GPRInfo::numberOfRegisters + FPRInfo::numberOfRegisters; ++index) {
        ShuffledRegister& reg = lookupForRegister(gprs, fprs, index);
        if (!reg.isEndOfNonCyclingPermutation() || reg.handled || (!reg.hasFrom && !reg.hasTo))
            continue;
        
        reg.handleNonCyclingPermutation(check, entry, *this, scratchFPR1, scratchFPR2);
        ASSERT((scratchFPR1 == InvalidFPRReg && scratchFPR2 == InvalidFPRReg) || (scratchFPR1 != scratchFPR2));
    }
    
    for (unsigned index = 0; index < GPRInfo::numberOfRegisters + FPRInfo::numberOfRegisters; ++index) {
        ShuffledRegister& reg = lookupForRegister(gprs, fprs, index);
        if (reg.handled || (!reg.hasFrom && !reg.hasTo))
            continue;
        
        reg.handleCyclingPermutation(check, entry, *this, scratchGPR, scratchFPR1, scratchFPR2);
        ASSERT((scratchFPR1 == InvalidFPRReg && scratchFPR2 == InvalidFPRReg) || (scratchFPR1 != scratchFPR2));
    }

#if !ASSERT_DISABLED
    for (unsigned index = 0; index < GPRInfo::numberOfRegisters + FPRInfo::numberOfRegisters; ++index) {
        ShuffledRegister& reg = lookupForRegister(gprs, fprs, index);
        ASSERT(reg.handled || (!reg.hasFrom && !reg.hasTo));
    }
#endif

    // Part 3: Fill any values that were spilled on speculative, but are not spilled
    //         on non-speculative.

    for (unsigned index = 0; index < FPRInfo::numberOfRegisters; ++index) {
        NodeIndex nodeIndex = entry.m_fprInfo[index].nodeIndex;
        if (nodeIndex == NoNode || entry.m_fprInfo[index].isSpilled)
            continue;
        
        NodeToRegisterMap::iterator mapIterator = checkNodeToRegisterMap.find(nodeIndex);
        if (mapIterator != checkNodeToRegisterMap.end()
            && !mapIterator->second.findInSpeculationCheck(check).isSpilled)
            continue;

        fillNumericToDouble(nodeIndex, FPRInfo::toRegister(index), GPRInfo::regT0);
    }

    for (unsigned index = 0; index < GPRInfo::numberOfRegisters; ++index) {
        NodeIndex nodeIndex = entry.m_gprInfo[index].nodeIndex;
        if (nodeIndex == NoNode || entry.m_gprInfo[index].isSpilled)
            continue;

        NodeToRegisterMap::iterator mapIterator = checkNodeToRegisterMap.find(nodeIndex);
        if (mapIterator != checkNodeToRegisterMap.end()
            && !mapIterator->second.findInSpeculationCheck(check).isSpilled)
            continue;

        DataFormat dataFormat = entry.m_gprInfo[index].format;
        if (dataFormat == DataFormatInteger)
            fillInt32ToInteger(nodeIndex, GPRInfo::toRegister(index));
        else {
            ASSERT(dataFormat & DataFormatJS || dataFormat == DataFormatCell); // Treat cell as JSValue for now!
            fillToJS(nodeIndex, GPRInfo::toRegister(index));
            // FIXME: For subtypes of DataFormatJS, should jitAssert the subtype?
        }
    }
    
    if (needToRestoreTagMaskRegister)
        move(TrustedImmPtr(reinterpret_cast<void*>(TagMask)), GPRInfo::tagMaskRegister);

    // Jump into the non-speculative path.
    jump(entry.m_entry);
}

void JITCompiler::linkSpeculationChecks(SpeculativeJIT& speculative, NonSpeculativeJIT& nonSpeculative)
{
    // Iterators to walk over the set of bail outs & corresponding entry points.
    SpeculationCheckVector::Iterator checksIter = speculative.speculationChecks().begin();
    SpeculationCheckVector::Iterator checksEnd = speculative.speculationChecks().end();
    NonSpeculativeJIT::EntryLocationVector::Iterator entriesIter = nonSpeculative.entryLocations().begin();
    NonSpeculativeJIT::EntryLocationVector::Iterator entriesEnd = nonSpeculative.entryLocations().end();
    
    NodeToRegisterMap checkNodeToRegisterMap;
    NodeToRegisterMap entryNodeToRegisterMap;
    
    // Iterate over the speculation checks.
    while (checksIter != checksEnd) {
        // For every bail out from the speculative path, we must have provided an entry point
        // into the non-speculative one.
        ASSERT(checksIter->m_nodeIndex == entriesIter->m_nodeIndex);

        // There may be multiple bail outs that map to the same entry point!
        do {
            ASSERT(checksIter != checksEnd);
            ASSERT(entriesIter != entriesEnd);

            // Plant code to link this speculation failure.
            const SpeculationCheck& check = *checksIter;
            const EntryLocation& entry = *entriesIter;
            jumpFromSpeculativeToNonSpeculative(check, entry, speculative.speculationRecovery(check.m_recoveryIndex), checkNodeToRegisterMap, entryNodeToRegisterMap);
             ++checksIter;
        } while (checksIter != checksEnd && checksIter->m_nodeIndex == entriesIter->m_nodeIndex);
         ++entriesIter;
    }

    // FIXME: https://bugs.webkit.org/show_bug.cgi?id=56289
    ASSERT(!(checksIter != checksEnd));
    ASSERT(!(entriesIter != entriesEnd));
}
#endif // ENABLE(DFG_OSR_EXIT)

void JITCompiler::compileEntry()
{
    m_startOfCode = label();
    
    // This code currently matches the old JIT. In the function header we need to
    // pop the return address (since we do not allow any recursion on the machine
    // stack), and perform a fast register file check.
    // FIXME: https://bugs.webkit.org/show_bug.cgi?id=56292
    // We'll need to convert the remaining cti_ style calls (specifically the register file
    // check) which will be dependent on stack layout. (We'd need to account for this in
    // both normal return code and when jumping to an exception handler).
    preserveReturnAddressAfterCall(GPRInfo::regT2);
    emitPutToCallFrameHeader(GPRInfo::regT2, RegisterFile::ReturnPC);
}

void JITCompiler::compileBody()
{
    // We generate the speculative code path, followed by the non-speculative
    // code for the function. Next we need to link the two together, making
    // bail-outs from the speculative path jump to the corresponding point on
    // the non-speculative one (and generating any code necessary to juggle
    // register values around, rebox values, and ensure spilled, to match the
    // non-speculative path's requirements).

#if ENABLE(DFG_JIT_BREAK_ON_EVERY_FUNCTION)
    // Handy debug tool!
    breakpoint();
#endif

    // First generate the speculative path.
    Label speculativePathBegin = label();
    SpeculativeJIT speculative(*this);
#if !ENABLE(DFG_DEBUG_LOCAL_DISBALE_SPECULATIVE)
    bool compiledSpeculative = speculative.compile();
#else
    bool compiledSpeculative = false;
#endif

    // Next, generate the non-speculative path. We pass this a SpeculationCheckIndexIterator
    // to allow it to check which nodes in the graph may bail out, and may need to reenter the
    // non-speculative path.
    if (compiledSpeculative) {
#if ENABLE(DFG_OSR_ENTRY)
        m_codeBlock->setJITCodeMap(m_jitCodeMapEncoder.finish());
#endif
        
#if ENABLE(DFG_OSR_EXIT)
        linkOSRExits(speculative);
#else
        SpeculationCheckIndexIterator checkIterator(speculative.speculationChecks());
        NonSpeculativeJIT nonSpeculative(*this);
        nonSpeculative.compile(checkIterator);

        // Link the bail-outs from the speculative path to the corresponding entry points into the non-speculative one.
        linkSpeculationChecks(speculative, nonSpeculative);
#endif
    } else {
        // If compilation through the SpeculativeJIT failed, throw away the code we generated.
        m_calls.clear();
        m_propertyAccesses.clear();
        m_jsCalls.clear();
        m_methodGets.clear();
        rewindToLabel(speculativePathBegin);

#if ENABLE(DFG_OSR_EXIT)
        SpeculationCheckIndexIterator checkIterator;
#else
        SpeculationCheckVector noChecks;
        SpeculationCheckIndexIterator checkIterator(noChecks);
#endif
        NonSpeculativeJIT nonSpeculative(*this);
        nonSpeculative.compile(checkIterator);
    }

    // Iterate over the m_calls vector, checking for exception checks,
    // and linking them to here.
    for (unsigned i = 0; i < m_calls.size(); ++i) {
        Jump& exceptionCheck = m_calls[i].m_exceptionCheck;
        if (exceptionCheck.isSet()) {
            exceptionCheck.link(this);
            ++m_exceptionCheckCount;
        }
    }
    // If any exception checks were linked, generate code to lookup a handler.
    if (m_exceptionCheckCount) {
        // lookupExceptionHandler is passed two arguments, exec (the CallFrame*), and
        // an identifier for the operation that threw the exception, which we can use
        // to look up handler information. The identifier we use is the return address
        // of the call out from JIT code that threw the exception; this is still
        // available on the stack, just below the stack pointer!
        move(GPRInfo::callFrameRegister, GPRInfo::argumentGPR0);
        peek(GPRInfo::argumentGPR1, -1);
        m_calls.append(CallRecord(call(), lookupExceptionHandler));
        // lookupExceptionHandler leaves the handler CallFrame* in the returnValueGPR,
        // and the address of the handler in returnValueGPR2.
        jump(GPRInfo::returnValueGPR2);
    }
}

void JITCompiler::link(LinkBuffer& linkBuffer)
{
    // Link the code, populate data in CodeBlock data structures.
#if ENABLE(DFG_DEBUG_VERBOSE)
    fprintf(stderr, "JIT code for %p start at [%p, %p)\n", m_codeBlock, linkBuffer.debugAddress(), static_cast<char*>(linkBuffer.debugAddress()) + linkBuffer.debugSize());
#endif

    // Link all calls out from the JIT code to their respective functions.
    for (unsigned i = 0; i < m_calls.size(); ++i) {
        if (m_calls[i].m_function.value())
            linkBuffer.link(m_calls[i].m_call, m_calls[i].m_function);
    }

    if (m_codeBlock->needsCallReturnIndices()) {
        m_codeBlock->callReturnIndexVector().reserveCapacity(m_exceptionCheckCount);
        for (unsigned i = 0; i < m_calls.size(); ++i) {
            if (m_calls[i].m_handlesExceptions) {
                unsigned returnAddressOffset = linkBuffer.returnAddressOffset(m_calls[i].m_call);
                unsigned exceptionInfo = m_calls[i].m_codeOrigin.bytecodeIndex();
                m_codeBlock->callReturnIndexVector().append(CallReturnOffsetToBytecodeOffset(returnAddressOffset, exceptionInfo));
            }
        }
    }

    m_codeBlock->setNumberOfStructureStubInfos(m_propertyAccesses.size());
    for (unsigned i = 0; i < m_propertyAccesses.size(); ++i) {
        StructureStubInfo& info = m_codeBlock->structureStubInfo(i);
        info.callReturnLocation = linkBuffer.locationOf(m_propertyAccesses[i].m_functionCall);
        info.u.unset.deltaCheckImmToCall = m_propertyAccesses[i].m_deltaCheckImmToCall;
        info.deltaCallToStructCheck = m_propertyAccesses[i].m_deltaCallToStructCheck;
        info.u.unset.deltaCallToLoadOrStore = m_propertyAccesses[i].m_deltaCallToLoadOrStore;
        info.deltaCallToSlowCase = m_propertyAccesses[i].m_deltaCallToSlowCase;
        info.deltaCallToDone = m_propertyAccesses[i].m_deltaCallToDone;
        info.baseGPR = m_propertyAccesses[i].m_baseGPR;
        info.valueGPR = m_propertyAccesses[i].m_valueGPR;
        info.scratchGPR = m_propertyAccesses[i].m_scratchGPR;
    }
    
    m_codeBlock->setNumberOfCallLinkInfos(m_jsCalls.size());
    for (unsigned i = 0; i < m_jsCalls.size(); ++i) {
        CallLinkInfo& info = m_codeBlock->callLinkInfo(i);
        info.isCall = m_jsCalls[i].m_isCall;
        info.isDFG = true;
        info.callReturnLocation = CodeLocationLabel(linkBuffer.locationOf(m_jsCalls[i].m_slowCall));
        info.hotPathBegin = linkBuffer.locationOf(m_jsCalls[i].m_targetToCheck);
        info.hotPathOther = linkBuffer.locationOfNearCall(m_jsCalls[i].m_fastCall);
    }
    
    m_codeBlock->addMethodCallLinkInfos(m_methodGets.size());
    for (unsigned i = 0; i < m_methodGets.size(); ++i) {
        MethodCallLinkInfo& info = m_codeBlock->methodCallLinkInfo(i);
        info.cachedStructure.setLocation(linkBuffer.locationOf(m_methodGets[i].m_structToCompare));
        info.cachedPrototypeStructure.setLocation(linkBuffer.locationOf(m_methodGets[i].m_protoStructToCompare));
        info.cachedFunction.setLocation(linkBuffer.locationOf(m_methodGets[i].m_putFunction));
        info.cachedPrototype.setLocation(linkBuffer.locationOf(m_methodGets[i].m_protoObj));
        info.callReturnLocation = linkBuffer.locationOf(m_methodGets[i].m_slowCall);
    }
}

void JITCompiler::compile(JITCode& entry)
{
    // Preserve the return address to the callframe.
    compileEntry();
    // Generate the body of the program.
    compileBody();
    // Link
    LinkBuffer linkBuffer(*m_globalData, this);
    link(linkBuffer);
    entry = JITCode(linkBuffer.finalizeCode(), JITCode::DFGJIT);
}

void JITCompiler::compileFunction(JITCode& entry, MacroAssemblerCodePtr& entryWithArityCheck)
{
    compileEntry();

    // === Function header code generation ===
    // This is the main entry point, without performing an arity check.
    // If we needed to perform an arity check we will already have moved the return address,
    // so enter after this.
    Label fromArityCheck(this);
    // Setup a pointer to the codeblock in the CallFrameHeader.
    emitPutImmediateToCallFrameHeader(m_codeBlock, RegisterFile::CodeBlock);
    // Plant a check that sufficient space is available in the RegisterFile.
    // FIXME: https://bugs.webkit.org/show_bug.cgi?id=56291
    addPtr(Imm32(m_codeBlock->m_numCalleeRegisters * sizeof(Register)), GPRInfo::callFrameRegister, GPRInfo::regT1);
    Jump registerFileCheck = branchPtr(Below, AbsoluteAddress(m_globalData->interpreter->registerFile().addressOfEnd()), GPRInfo::regT1);
    // Return here after register file check.
    Label fromRegisterFileCheck = label();


    // === Function body code generation ===
    compileBody();

    // === Function footer code generation ===
    //
    // Generate code to perform the slow register file check (if the fast one in
    // the function header fails), and generate the entry point with arity check.
    //
    // Generate the register file check; if the fast check in the function head fails,
    // we need to call out to a helper function to check whether more space is available.
    // FIXME: change this from a cti call to a DFG style operation (normal C calling conventions).
    registerFileCheck.link(this);
    move(stackPointerRegister, GPRInfo::argumentGPR0);
    poke(GPRInfo::callFrameRegister, OBJECT_OFFSETOF(struct JITStackFrame, callFrame) / sizeof(void*));
    Call callRegisterFileCheck = call();
    jump(fromRegisterFileCheck);
    
    // The fast entry point into a function does not check the correct number of arguments
    // have been passed to the call (we only use the fast entry point where we can statically
    // determine the correct number of arguments have been passed, or have already checked).
    // In cases where an arity check is necessary, we enter here.
    // FIXME: change this from a cti call to a DFG style operation (normal C calling conventions).
    Label arityCheck = label();
    preserveReturnAddressAfterCall(GPRInfo::regT2);
    emitPutToCallFrameHeader(GPRInfo::regT2, RegisterFile::ReturnPC);
    branch32(Equal, GPRInfo::regT1, Imm32(m_codeBlock->m_numParameters)).linkTo(fromArityCheck, this);
    move(stackPointerRegister, GPRInfo::argumentGPR0);
    poke(GPRInfo::callFrameRegister, OBJECT_OFFSETOF(struct JITStackFrame, callFrame) / sizeof(void*));
    Call callArityCheck = call();
    move(GPRInfo::regT0, GPRInfo::callFrameRegister);
    jump(fromArityCheck);


    // === Link ===
    LinkBuffer linkBuffer(*m_globalData, this);
    link(linkBuffer);
    
    // FIXME: switch the register file check & arity check over to DFGOpertaion style calls, not JIT stubs.
    linkBuffer.link(callRegisterFileCheck, cti_register_file_check);
    linkBuffer.link(callArityCheck, m_codeBlock->m_isConstructor ? cti_op_construct_arityCheck : cti_op_call_arityCheck);

    entryWithArityCheck = linkBuffer.locationOf(arityCheck);
    entry = JITCode(linkBuffer.finalizeCode(), JITCode::DFGJIT);
}

#if ENABLE(DFG_JIT_ASSERT)
void JITCompiler::jitAssertIsInt32(GPRReg gpr)
{
#if CPU(X86_64)
    Jump checkInt32 = branchPtr(BelowOrEqual, gpr, TrustedImmPtr(reinterpret_cast<void*>(static_cast<uintptr_t>(0xFFFFFFFFu))));
    breakpoint();
    checkInt32.link(this);
#else
    UNUSED_PARAM(gpr);
#endif
}

void JITCompiler::jitAssertIsJSInt32(GPRReg gpr)
{
    Jump checkJSInt32 = branchPtr(AboveOrEqual, gpr, GPRInfo::tagTypeNumberRegister);
    breakpoint();
    checkJSInt32.link(this);
}

void JITCompiler::jitAssertIsJSNumber(GPRReg gpr)
{
    Jump checkJSNumber = branchTestPtr(MacroAssembler::NonZero, gpr, GPRInfo::tagTypeNumberRegister);
    breakpoint();
    checkJSNumber.link(this);
}

void JITCompiler::jitAssertIsJSDouble(GPRReg gpr)
{
    Jump checkJSInt32 = branchPtr(AboveOrEqual, gpr, GPRInfo::tagTypeNumberRegister);
    Jump checkJSNumber = branchTestPtr(MacroAssembler::NonZero, gpr, GPRInfo::tagTypeNumberRegister);
    checkJSInt32.link(this);
    breakpoint();
    checkJSNumber.link(this);
}

void JITCompiler::jitAssertIsCell(GPRReg gpr)
{
    Jump checkCell = branchTestPtr(MacroAssembler::Zero, gpr, GPRInfo::tagMaskRegister);
    breakpoint();
    checkCell.link(this);
}
#endif

#if ENABLE(SAMPLING_COUNTERS) && CPU(X86_64) // Or any other 64-bit platform!
void JITCompiler::emitCount(MacroAssembler& jit, AbstractSamplingCounter& counter, uint32_t increment)
{
    jit.addPtr(TrustedImm32(increment), AbsoluteAddress(counter.addressOfCounter()));
}
#endif

#if ENABLE(SAMPLING_COUNTERS) && CPU(X86) // Or any other little-endian 32-bit platform!
void JITCompiler::emitCount(MacroAsembler& jit, AbstractSamplingCounter& counter, uint32_t increment)
{
    intptr_t hiWord = reinterpret_cast<intptr_t>(counter.addressOfCounter()) + sizeof(int32_t);
    jit.add32(TrustedImm32(increment), AbsoluteAddress(counter.addressOfCounter()));
    jit.addWithCarry32(TrustedImm32(0), AbsoluteAddress(reinterpret_cast<void*>(hiWord)));
}
#endif

#if ENABLE(SAMPLING_FLAGS)
void JITCompiler::setSamplingFlag(int32_t flag)
{
    ASSERT(flag >= 1);
    ASSERT(flag <= 32);
    or32(TrustedImm32(1u << (flag - 1)), AbsoluteAddress(SamplingFlags::addressOfFlags()));
}

void JITCompiler::clearSamplingFlag(int32_t flag)
{
    ASSERT(flag >= 1);
    ASSERT(flag <= 32);
    and32(TrustedImm32(~(1u << (flag - 1))), AbsoluteAddress(SamplingFlags::addressOfFlags()));
}
#endif

} } // namespace JSC::DFG

#endif
