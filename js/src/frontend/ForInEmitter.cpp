/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/ForInEmitter.h"

#include "frontend/BytecodeEmitter.h"
#include "frontend/EmitterScope.h"
#include "frontend/SourceNotes.h"
#include "vm/Opcodes.h"
#include "vm/Scope.h"

using namespace js;
using namespace js::frontend;

using mozilla::Maybe;
using mozilla::Nothing;

ForInEmitter::ForInEmitter(BytecodeEmitter* bce,
                           const EmitterScope* headLexicalEmitterScope)
    : bce_(bce), headLexicalEmitterScope_(headLexicalEmitterScope) {}

bool ForInEmitter::emitIterated() {
  MOZ_ASSERT(state_ == State::Start);
  tdzCacheForIteratedValue_.emplace(bce_);

#ifdef DEBUG
  state_ = State::Iterated;
#endif
  return true;
}

bool ForInEmitter::emitInitialize() {
  MOZ_ASSERT(state_ == State::Iterated);
  tdzCacheForIteratedValue_.reset();

  if (!bce_->emit1(JSOP_ITER)) {
    //              [stack] ITER
    return false;
  }

  loopInfo_.emplace(bce_, StatementKind::ForInLoop);

  if (!bce_->newSrcNote(SRC_FOR_IN)) {
    return false;
  }

  if (!loopInfo_->emitLoopHead(bce_, Nothing())) {
    //              [stack] ITER
    return false;
  }

  if (!bce_->emit1(JSOP_MOREITER)) {
    //              [stack] ITER NEXTITERVAL?
    return false;
  }
  if (!bce_->emit1(JSOP_ISNOITER)) {
    //              [stack] ITER NEXTITERVAL? ISNOITER
    return false;
  }
  if (!bce_->emitJump(JSOP_IFNE, &loopInfo_->breaks)) {
    //              [stack] ITER NEXTITERVAL?
    return false;
  }

  // If the loop had an escaping lexical declaration, reset the declaration's
  // bindings to uninitialized to implement TDZ semantics.
  if (headLexicalEmitterScope_) {
    // The environment chain only includes an environment for the
    // for-in loop head *if* a scope binding is captured, thereby
    // requiring recreation each iteration. If a lexical scope exists
    // for the head, it must be the innermost one. If that scope has
    // closed-over bindings inducing an environment, recreate the
    // current environment.
    MOZ_ASSERT(headLexicalEmitterScope_ == bce_->innermostEmitterScope());
    MOZ_ASSERT(headLexicalEmitterScope_->scope(bce_)->kind() ==
               ScopeKind::Lexical);

    if (headLexicalEmitterScope_->hasEnvironment()) {
      if (!bce_->emit1(JSOP_RECREATELEXICALENV)) {
        //          [stack] ITER ITERVAL
        return false;
      }
    }

    // For uncaptured bindings, put them back in TDZ.
    if (!headLexicalEmitterScope_->deadZoneFrameSlots(bce_)) {
      return false;
    }
  }

#ifdef DEBUG
  loopDepth_ = bce_->bytecodeSection().stackDepth();
#endif
  MOZ_ASSERT(loopDepth_ >= 2);

  if (!bce_->emit1(JSOP_ITERNEXT)) {
    //              [stack] ITER ITERVAL
    return false;
  }

#ifdef DEBUG
  state_ = State::Initialize;
#endif
  return true;
}

bool ForInEmitter::emitBody() {
  MOZ_ASSERT(state_ == State::Initialize);

  MOZ_ASSERT(bce_->bytecodeSection().stackDepth() == loopDepth_,
             "iterator and iterval must be left on the stack");

#ifdef DEBUG
  state_ = State::Body;
#endif
  return true;
}

bool ForInEmitter::emitEnd(const Maybe<uint32_t>& forPos) {
  MOZ_ASSERT(state_ == State::Body);

  if (forPos) {
    // Make sure this code is attributed to the "for".
    if (!bce_->updateSourceCoordNotes(*forPos)) {
      return false;
    }
  }

  if (!loopInfo_->emitContinueTarget(bce_)) {
    //              [stack] ITER ITERVAL
    return false;
  }

  if (!bce_->emit1(JSOP_POP)) {
    //              [stack] ITER
    return false;
  }
  if (!loopInfo_->emitLoopEnd(bce_, JSOP_GOTO)) {
    //              [stack] ITER
    return false;
  }

  // When we leave the loop body and jump to this point, the iteration value is
  // still on the stack. Account for that by updating the stack depth manually.
  int32_t stackDepth = bce_->bytecodeSection().stackDepth() + 1;
  MOZ_ASSERT(stackDepth == loopDepth_);
  bce_->bytecodeSection().setStackDepth(stackDepth);

  if (!loopInfo_->patchBreaksAndContinues(bce_)) {
    //              [stack] ITER ITERVAL
    return false;
  }

  // Pop the enumeration value.
  if (!bce_->emit1(JSOP_POP)) {
    //              [stack] ITER
    return false;
  }

  if (!bce_->addTryNote(JSTRY_FOR_IN, bce_->bytecodeSection().stackDepth(),
                        loopInfo_->headOffset(),
                        bce_->bytecodeSection().offset())) {
    return false;
  }

  if (!bce_->emit1(JSOP_ENDITER)) {
    //              [stack]
    return false;
  }

  loopInfo_.reset();

#ifdef DEBUG
  state_ = State::End;
#endif
  return true;
}
