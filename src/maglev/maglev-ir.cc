// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/maglev/maglev-ir.h"

#include "src/base/bits.h"
#include "src/base/logging.h"
#include "src/baseline/baseline-assembler-inl.h"
#include "src/builtins/builtins-constructor.h"
#include "src/codegen/interface-descriptors-inl.h"
#include "src/codegen/maglev-safepoint-table.h"
#include "src/codegen/register.h"
#include "src/codegen/reglist.h"
#include "src/codegen/x64/assembler-x64.h"
#include "src/codegen/x64/register-x64.h"
#include "src/common/globals.h"
#include "src/compiler/backend/instruction.h"
#include "src/compiler/js-heap-broker.h"
#include "src/deoptimizer/deoptimize-reason.h"
#include "src/ic/handler-configuration.h"
#include "src/interpreter/bytecode-flags.h"
#include "src/maglev/maglev-assembler-inl.h"
#include "src/maglev/maglev-code-gen-state.h"
#include "src/maglev/maglev-compilation-unit.h"
#include "src/maglev/maglev-graph-labeller.h"
#include "src/maglev/maglev-graph-printer.h"
#include "src/maglev/maglev-graph-processor.h"
#include "src/maglev/maglev-interpreter-frame-state.h"
#include "src/maglev/maglev-ir-inl.h"
#include "src/maglev/maglev-vreg-allocator.h"
#include "src/objects/instance-type.h"
#include "src/objects/js-array-buffer.h"

namespace v8 {
namespace internal {
namespace maglev {

const char* OpcodeToString(Opcode opcode) {
#define DEF_NAME(Name) #Name,
  static constexpr const char* const names[] = {NODE_BASE_LIST(DEF_NAME)};
#undef DEF_NAME
  return names[static_cast<int>(opcode)];
}

#define __ masm->

namespace {

// ---
// Vreg allocation helpers.
// ---

int GetVirtualRegister(Node* node) {
  return compiler::UnallocatedOperand::cast(node->result().operand())
      .virtual_register();
}

void DefineAsRegister(MaglevVregAllocationState* vreg_state, Node* node) {
  node->result().SetUnallocated(
      compiler::UnallocatedOperand::MUST_HAVE_REGISTER,
      vreg_state->AllocateVirtualRegister());
}
void DefineAsConstant(MaglevVregAllocationState* vreg_state, Node* node) {
  node->result().SetUnallocated(compiler::UnallocatedOperand::NONE,
                                vreg_state->AllocateVirtualRegister());
}

void DefineAsFixed(MaglevVregAllocationState* vreg_state, Node* node,
                   Register reg) {
  node->result().SetUnallocated(compiler::UnallocatedOperand::FIXED_REGISTER,
                                reg.code(),
                                vreg_state->AllocateVirtualRegister());
}

void DefineSameAsFirst(MaglevVregAllocationState* vreg_state, Node* node) {
  node->result().SetUnallocated(vreg_state->AllocateVirtualRegister(), 0);
}

void UseRegister(Input& input) {
  input.SetUnallocated(compiler::UnallocatedOperand::MUST_HAVE_REGISTER,
                       compiler::UnallocatedOperand::USED_AT_END,
                       GetVirtualRegister(input.node()));
}
void UseAndClobberRegister(Input& input) {
  input.SetUnallocated(compiler::UnallocatedOperand::MUST_HAVE_REGISTER,
                       compiler::UnallocatedOperand::USED_AT_START,
                       GetVirtualRegister(input.node()));
}
void UseAny(Input& input) {
  input.SetUnallocated(
      compiler::UnallocatedOperand::REGISTER_OR_SLOT_OR_CONSTANT,
      compiler::UnallocatedOperand::USED_AT_END,
      GetVirtualRegister(input.node()));
}
void UseFixed(Input& input, Register reg) {
  input.SetUnallocated(compiler::UnallocatedOperand::FIXED_REGISTER, reg.code(),
                       GetVirtualRegister(input.node()));
}
[[maybe_unused]] void UseFixed(Input& input, DoubleRegister reg) {
  input.SetUnallocated(compiler::UnallocatedOperand::FIXED_FP_REGISTER,
                       reg.code(), GetVirtualRegister(input.node()));
}

void AddDeoptRegistersToSnapshot(RegisterSnapshot* snapshot,
                                 const EagerDeoptInfo* deopt_info) {
  detail::DeepForEachInput(deopt_info, [&](ValueNode* node,
                                           InputLocation* input) {
    if (!input->IsAnyRegister()) return;
    if (input->IsDoubleRegister()) {
      snapshot->live_double_registers.set(input->AssignedDoubleRegister());
    } else {
      snapshot->live_registers.set(input->AssignedGeneralRegister());
      if (node->is_tagged()) {
        snapshot->live_tagged_registers.set(input->AssignedGeneralRegister());
      }
    }
  });
}

#ifdef DEBUG
RegList GetGeneralRegistersUsedAsInputs(const EagerDeoptInfo* deopt_info) {
  RegList regs;
  detail::DeepForEachInput(deopt_info,
                           [&regs](ValueNode* value, InputLocation* input) {
                             if (input->IsGeneralRegister()) {
                               regs.set(input->AssignedGeneralRegister());
                             }
                           });
  return regs;
}
#endif  // DEBUG

// Helper macro for checking that a reglist is empty which prints the contents
// when non-empty.
#define DCHECK_REGLIST_EMPTY(...) DCHECK_EQ((__VA_ARGS__), RegList{})

// ---
// Print
// ---

void PrintInputs(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                 const NodeBase* node) {
  if (!node->has_inputs()) return;

  os << " [";
  for (int i = 0; i < node->input_count(); i++) {
    if (i != 0) os << ", ";
    graph_labeller->PrintInput(os, node->input(i));
  }
  os << "]";
}

void PrintResult(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                 const NodeBase* node) {}

void PrintResult(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                 const ValueNode* node) {
  os << " → " << node->result().operand();
  if (node->has_valid_live_range()) {
    os << ", live range: [" << node->live_range().start << "-"
       << node->live_range().end << "]";
  }
}

void PrintTargets(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                  const NodeBase* node) {}

void PrintTargets(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                  const UnconditionalControlNode* node) {
  os << " b" << graph_labeller->BlockId(node->target());
}

void PrintTargets(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                  const BranchControlNode* node) {
  os << " b" << graph_labeller->BlockId(node->if_true()) << " b"
     << graph_labeller->BlockId(node->if_false());
}

void PrintTargets(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                  const Switch* node) {
  for (int i = 0; i < node->size(); i++) {
    const BasicBlockRef& target = node->Cast<Switch>()->targets()[i];
    os << " b" << graph_labeller->BlockId(target.block_ptr());
  }
  if (node->Cast<Switch>()->has_fallthrough()) {
    BasicBlock* fallthrough_target = node->Cast<Switch>()->fallthrough();
    os << " b" << graph_labeller->BlockId(fallthrough_target);
  }
}

class MaybeUnparkForPrint {
 public:
  MaybeUnparkForPrint() {
    LocalHeap* local_heap = LocalHeap::Current();
    if (!local_heap) {
      local_heap = Isolate::Current()->main_thread_local_heap();
    }
    DCHECK_NOT_NULL(local_heap);
    if (local_heap->IsParked()) {
      scope_.emplace(local_heap);
    }
  }

 private:
  base::Optional<UnparkedScope> scope_;
};

template <typename NodeT>
void PrintImpl(std::ostream& os, MaglevGraphLabeller* graph_labeller,
               const NodeT* node, bool skip_targets) {
  MaybeUnparkForPrint unpark;
  os << node->opcode();
  node->PrintParams(os, graph_labeller);
  PrintInputs(os, graph_labeller, node);
  PrintResult(os, graph_labeller, node);
  if (!skip_targets) {
    PrintTargets(os, graph_labeller, node);
  }
}

}  // namespace

void NodeBase::Print(std::ostream& os, MaglevGraphLabeller* graph_labeller,
                     bool skip_targets) const {
  switch (opcode()) {
#define V(Name)         \
  case Opcode::k##Name: \
    return PrintImpl(os, graph_labeller, this->Cast<Name>(), skip_targets);
    NODE_BASE_LIST(V)
#undef V
  }
  UNREACHABLE();
}

void NodeBase::Print() const {
  MaglevGraphLabeller labeller;
  Print(std::cout, &labeller);
  std::cout << std::endl;
}

namespace {
size_t GetInputLocationsArraySize(const DeoptFrame& top_frame) {
  size_t size = 0;
  const DeoptFrame* frame = &top_frame;
  do {
    switch (frame->type()) {
      case DeoptFrame::FrameType::kInterpretedFrame:
        size += frame->as_interpreted().frame_state()->size(
            frame->as_interpreted().unit());
        break;
      case DeoptFrame::FrameType::kBuiltinContinuationFrame:
        size += frame->as_builtin_continuation().parameters().size() + 1;
        break;
    }
    frame = frame->parent();
  } while (frame != nullptr);
  return size;
}
}  // namespace

DeoptInfo::DeoptInfo(Zone* zone, DeoptFrame top_frame)
    : top_frame_(top_frame),
      input_locations_(zone->NewArray<InputLocation>(
          GetInputLocationsArraySize(top_frame))) {
  // Initialise InputLocations so that they correctly don't have a next use id.
  for (size_t i = 0; i < GetInputLocationsArraySize(top_frame); ++i) {
    new (&input_locations_[i]) InputLocation();
  }
}

bool LazyDeoptInfo::IsResultRegister(interpreter::Register reg) const {
  if (V8_LIKELY(result_size_ == 1)) {
    return reg == result_location_;
  }
  DCHECK_EQ(result_size_, 2);
  return reg == result_location_ ||
         reg == interpreter::Register(result_location_.index() + 1);
}

// ---
// Nodes
// ---
namespace {
template <typename NodeT>
void LoadToRegisterHelper(NodeT* node, MaglevAssembler* masm, Register reg) {
  if constexpr (NodeT::kProperties.value_representation() !=
                ValueRepresentation::kFloat64) {
    return node->DoLoadToRegister(masm, reg);
  } else {
    UNREACHABLE();
  }
}
template <typename NodeT>
void LoadToRegisterHelper(NodeT* node, MaglevAssembler* masm,
                          DoubleRegister reg) {
  if constexpr (NodeT::kProperties.value_representation() ==
                ValueRepresentation::kFloat64) {
    return node->DoLoadToRegister(masm, reg);
  } else {
    UNREACHABLE();
  }
}
}  // namespace
void ValueNode::LoadToRegister(MaglevAssembler* masm, Register reg) {
  switch (opcode()) {
#define V(Name)         \
  case Opcode::k##Name: \
    return LoadToRegisterHelper(this->Cast<Name>(), masm, reg);
    VALUE_NODE_LIST(V)
#undef V
    default:
      UNREACHABLE();
  }
}
void ValueNode::LoadToRegister(MaglevAssembler* masm, DoubleRegister reg) {
  switch (opcode()) {
#define V(Name)         \
  case Opcode::k##Name: \
    return LoadToRegisterHelper(this->Cast<Name>(), masm, reg);
    VALUE_NODE_LIST(V)
#undef V
    default:
      UNREACHABLE();
  }
}
void ValueNode::DoLoadToRegister(MaglevAssembler* masm, Register reg) {
  DCHECK(is_spilled());
  DCHECK(!use_double_register());
  __ movq(reg,
          masm->GetStackSlot(compiler::AllocatedOperand::cast(spill_slot())));
}
void ValueNode::DoLoadToRegister(MaglevAssembler* masm, DoubleRegister reg) {
  DCHECK(is_spilled());
  DCHECK(use_double_register());
  __ Movsd(reg,
           masm->GetStackSlot(compiler::AllocatedOperand::cast(spill_slot())));
}
Handle<Object> ValueNode::Reify(LocalIsolate* isolate) {
  switch (opcode()) {
#define V(Name)         \
  case Opcode::k##Name: \
    return this->Cast<Name>()->DoReify(isolate);
    CONSTANT_VALUE_NODE_LIST(V)
#undef V
    default:
      UNREACHABLE();
  }
}

void ValueNode::SetNoSpillOrHint() {
  DCHECK_EQ(state_, kLastUse);
  DCHECK(!IsConstantNode(opcode()));
#ifdef DEBUG
  state_ = kSpillOrHint;
#endif  // DEBUG
  spill_or_hint_ = compiler::InstructionOperand();
}

void ValueNode::SetConstantLocation() {
  DCHECK(IsConstantNode(opcode()));
#ifdef DEBUG
  state_ = kSpillOrHint;
#endif  // DEBUG
  spill_or_hint_ = compiler::ConstantOperand(
      compiler::UnallocatedOperand::cast(result().operand())
          .virtual_register());
}

void SmiConstant::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  DefineAsConstant(vreg_state, this);
}
void SmiConstant::GenerateCode(MaglevAssembler* masm,
                               const ProcessingState& state) {}
Handle<Object> SmiConstant::DoReify(LocalIsolate* isolate) {
  return handle(value_, isolate);
}
void SmiConstant::DoLoadToRegister(MaglevAssembler* masm, Register reg) {
  __ Move(reg, Immediate(value()));
}
void SmiConstant::PrintParams(std::ostream& os,
                              MaglevGraphLabeller* graph_labeller) const {
  os << "(" << value() << ")";
}

void Float64Constant::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  DefineAsConstant(vreg_state, this);
}
void Float64Constant::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {}
Handle<Object> Float64Constant::DoReify(LocalIsolate* isolate) {
  return isolate->factory()->NewNumber<AllocationType::kOld>(value_);
}
void Float64Constant::DoLoadToRegister(MaglevAssembler* masm,
                                       DoubleRegister reg) {
  __ Move(reg, value());
}
void Float64Constant::PrintParams(std::ostream& os,
                                  MaglevGraphLabeller* graph_labeller) const {
  os << "(" << value() << ")";
}

void Constant::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  DefineAsConstant(vreg_state, this);
}
void Constant::GenerateCode(MaglevAssembler* masm,
                            const ProcessingState& state) {}
void Constant::DoLoadToRegister(MaglevAssembler* masm, Register reg) {
  __ Move(reg, object_.object());
}
Handle<Object> Constant::DoReify(LocalIsolate* isolate) {
  return object_.object();
}
void Constant::PrintParams(std::ostream& os,
                           MaglevGraphLabeller* graph_labeller) const {
  os << "(" << object_ << ")";
}

void DeleteProperty::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  using D = CallInterfaceDescriptorFor<Builtin::kDeleteProperty>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(object(), D::GetRegisterParameter(D::kObject));
  UseFixed(key(), D::GetRegisterParameter(D::kKey));
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void DeleteProperty::GenerateCode(MaglevAssembler* masm,
                                  const ProcessingState& state) {
  using D = CallInterfaceDescriptorFor<Builtin::kDeleteProperty>::type;
  DCHECK_EQ(ToRegister(context()), kContextRegister);
  DCHECK_EQ(ToRegister(object()), D::GetRegisterParameter(D::kObject));
  DCHECK_EQ(ToRegister(key()), D::GetRegisterParameter(D::kKey));
  __ Move(D::GetRegisterParameter(D::kLanguageMode),
          Smi::FromInt(static_cast<int>(mode())));
  __ CallBuiltin(Builtin::kDeleteProperty);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}
void DeleteProperty::PrintParams(std::ostream& os,
                                 MaglevGraphLabeller* graph_labeller) const {
  os << "(" << LanguageMode2String(mode()) << ")";
}

void GeneratorStore::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseAny(context_input());
  UseRegister(generator_input());
  for (int i = 0; i < num_parameters_and_registers(); i++) {
    UseAny(parameters_and_registers(i));
  }
  RequireSpecificTemporary(WriteBarrierDescriptor::ObjectRegister());
  RequireSpecificTemporary(WriteBarrierDescriptor::SlotAddressRegister());
}
void GeneratorStore::GenerateCode(MaglevAssembler* masm,
                                  const ProcessingState& state) {
  Register generator = ToRegister(generator_input());
  Register array = WriteBarrierDescriptor::ObjectRegister();
  __ LoadTaggedPointerField(
      array, FieldOperand(generator,
                          JSGeneratorObject::kParametersAndRegistersOffset));

  for (int i = 0; i < num_parameters_and_registers(); i++) {
    // Use WriteBarrierDescriptor::SlotAddressRegister() as the scratch
    // register since it's a known temporary, and the write barrier slow path
    // generates better code when value == scratch. Can't use kScratchRegister
    // because CheckPageFlag uses it.
    Register value =
        __ FromAnyToRegister(parameters_and_registers(i),
                             WriteBarrierDescriptor::SlotAddressRegister());

    ZoneLabelRef done(masm);
    DeferredCodeInfo* deferred_write_barrier = __ PushDeferredCode(
        [](MaglevAssembler* masm, ZoneLabelRef done, Register value,
           Register array, GeneratorStore* node, int32_t offset) {
          ASM_CODE_COMMENT_STRING(masm, "Write barrier slow path");
          // Use WriteBarrierDescriptor::SlotAddressRegister() as the scratch
          // register, see comment above.
          __ CheckPageFlag(
              value, WriteBarrierDescriptor::SlotAddressRegister(),
              MemoryChunk::kPointersToHereAreInterestingOrInSharedHeapMask,
              zero, *done);

          Register slot_reg = WriteBarrierDescriptor::SlotAddressRegister();

          __ leaq(slot_reg, FieldOperand(array, offset));

          // TODO(leszeks): Add an interface for flushing all double registers
          // before this Node, to avoid needing to save them here.
          SaveFPRegsMode const save_fp_mode =
              !node->register_snapshot().live_double_registers.is_empty()
                  ? SaveFPRegsMode::kSave
                  : SaveFPRegsMode::kIgnore;

          __ CallRecordWriteStub(array, slot_reg, save_fp_mode);

          __ jmp(*done);
        },
        done, value, array, this, FixedArray::OffsetOfElementAt(i));

    __ StoreTaggedField(FieldOperand(array, FixedArray::OffsetOfElementAt(i)),
                        value);
    __ JumpIfSmi(value, *done, Label::kNear);
    // TODO(leszeks): This will stay either false or true throughout this loop.
    // Consider hoisting the check out of the loop and duplicating the loop into
    // with and without write barrier.
    __ CheckPageFlag(array, kScratchRegister,
                     MemoryChunk::kPointersFromHereAreInterestingMask, not_zero,
                     &deferred_write_barrier->deferred_code_label);

    __ bind(*done);
  }

  // Use WriteBarrierDescriptor::SlotAddressRegister() as the scratch
  // register, see comment above.
  Register context = __ FromAnyToRegister(
      context_input(), WriteBarrierDescriptor::SlotAddressRegister());

  ZoneLabelRef done(masm);
  DeferredCodeInfo* deferred_context_write_barrier = __ PushDeferredCode(
      [](MaglevAssembler* masm, ZoneLabelRef done, Register context,
         Register generator, GeneratorStore* node) {
        ASM_CODE_COMMENT_STRING(masm, "Write barrier slow path");
        // Use WriteBarrierDescriptor::SlotAddressRegister() as the scratch
        // register, see comment above.
        // TODO(leszeks): The context is almost always going to be in
        // old-space, consider moving this check to the fast path, maybe even
        // as the first bailout.
        __ CheckPageFlag(
            context, WriteBarrierDescriptor::SlotAddressRegister(),
            MemoryChunk::kPointersToHereAreInterestingOrInSharedHeapMask, zero,
            *done);

        __ Move(WriteBarrierDescriptor::ObjectRegister(), generator);
        generator = WriteBarrierDescriptor::ObjectRegister();
        Register slot_reg = WriteBarrierDescriptor::SlotAddressRegister();

        __ leaq(slot_reg,
                FieldOperand(generator, JSGeneratorObject::kContextOffset));

        // TODO(leszeks): Add an interface for flushing all double registers
        // before this Node, to avoid needing to save them here.
        SaveFPRegsMode const save_fp_mode =
            !node->register_snapshot().live_double_registers.is_empty()
                ? SaveFPRegsMode::kSave
                : SaveFPRegsMode::kIgnore;

        __ CallRecordWriteStub(generator, slot_reg, save_fp_mode);

        __ jmp(*done);
      },
      done, context, generator, this);
  __ StoreTaggedField(
      FieldOperand(generator, JSGeneratorObject::kContextOffset), context);
  __ AssertNotSmi(context);
  __ CheckPageFlag(generator, kScratchRegister,
                   MemoryChunk::kPointersFromHereAreInterestingMask, not_zero,
                   &deferred_context_write_barrier->deferred_code_label);
  __ bind(*done);

  __ StoreTaggedSignedField(
      FieldOperand(generator, JSGeneratorObject::kContinuationOffset),
      Smi::FromInt(suspend_id()));
  __ StoreTaggedSignedField(
      FieldOperand(generator, JSGeneratorObject::kInputOrDebugPosOffset),
      Smi::FromInt(bytecode_offset()));
}

void GeneratorRestoreRegister::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(array_input());
  DefineAsRegister(vreg_state, this);
  set_temporaries_needed(1);
}
void GeneratorRestoreRegister::GenerateCode(MaglevAssembler* masm,
                                            const ProcessingState& state) {
  Register array = ToRegister(array_input());
  Register result_reg = ToRegister(result());
  Register temp = general_temporaries().PopFirst();

  // The input and the output can alias, if that happen we use a temporary
  // register and a move at the end.
  Register value = (array == result_reg ? temp : result_reg);

  // Loads the current value in the generator register file.
  __ DecompressAnyTagged(
      value, FieldOperand(array, FixedArray::OffsetOfElementAt(index())));

  // And trashs it with StaleRegisterConstant.
  __ LoadRoot(kScratchRegister, RootIndex::kStaleRegister);
  __ StoreTaggedField(
      FieldOperand(array, FixedArray::OffsetOfElementAt(index())),
      kScratchRegister);

  if (value != result_reg) {
    __ Move(result_reg, value);
  }
}

void ForInPrepare::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  using D = CallInterfaceDescriptorFor<Builtin::kForInPrepare>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(enumerator(), D::GetRegisterParameter(D::kEnumerator));
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void ForInPrepare::GenerateCode(MaglevAssembler* masm,
                                const ProcessingState& state) {
  using D = CallInterfaceDescriptorFor<Builtin::kForInPrepare>::type;
  DCHECK_EQ(ToRegister(context()), kContextRegister);
  DCHECK_EQ(ToRegister(enumerator()), D::GetRegisterParameter(D::kEnumerator));
  __ Move(D::GetRegisterParameter(D::kVectorIndex),
          TaggedIndex::FromIntptr(feedback().index()));
  __ Move(D::GetRegisterParameter(D::kFeedbackVector), feedback().vector);
  __ CallBuiltin(Builtin::kForInPrepare);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void ForInNext::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  using D = CallInterfaceDescriptorFor<Builtin::kForInNext>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(receiver(), D::GetRegisterParameter(D::kReceiver));
  UseFixed(cache_array(), D::GetRegisterParameter(D::kCacheArray));
  UseFixed(cache_type(), D::GetRegisterParameter(D::kCacheType));
  UseFixed(cache_index(), D::GetRegisterParameter(D::kCacheIndex));
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void ForInNext::GenerateCode(MaglevAssembler* masm,
                             const ProcessingState& state) {
  using D = CallInterfaceDescriptorFor<Builtin::kForInNext>::type;
  DCHECK_EQ(ToRegister(context()), kContextRegister);
  DCHECK_EQ(ToRegister(receiver()), D::GetRegisterParameter(D::kReceiver));
  DCHECK_EQ(ToRegister(cache_array()), D::GetRegisterParameter(D::kCacheArray));
  DCHECK_EQ(ToRegister(cache_type()), D::GetRegisterParameter(D::kCacheType));
  DCHECK_EQ(ToRegister(cache_index()), D::GetRegisterParameter(D::kCacheIndex));
  __ Move(D::GetRegisterParameter(D::kSlot), Immediate(feedback().index()));
  // Feedback vector is pushed into the stack.
  static_assert(D::GetStackParameterIndex(D::kFeedbackVector) == 0);
  static_assert(D::GetStackParameterCount() == 1);
  __ Push(feedback().vector);
  __ CallBuiltin(Builtin::kForInNext);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void GetIterator::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  using D = CallInterfaceDescriptorFor<Builtin::kGetIteratorWithFeedback>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(receiver(), D::GetRegisterParameter(D::kReceiver));
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void GetIterator::GenerateCode(MaglevAssembler* masm,
                               const ProcessingState& state) {
  using D = CallInterfaceDescriptorFor<Builtin::kGetIteratorWithFeedback>::type;
  DCHECK_EQ(ToRegister(context()), kContextRegister);
  DCHECK_EQ(ToRegister(receiver()), D::GetRegisterParameter(D::kReceiver));
  __ Move(D::GetRegisterParameter(D::kLoadSlot),
          TaggedIndex::FromIntptr(load_slot()));
  __ Move(D::GetRegisterParameter(D::kCallSlot),
          TaggedIndex::FromIntptr(call_slot()));
  __ Move(D::GetRegisterParameter(D::kMaybeFeedbackVector), feedback());
  __ CallBuiltin(Builtin::kGetIteratorWithFeedback);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void GetSecondReturnedValue::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  DefineAsFixed(vreg_state, this, kReturnRegister1);
}
void GetSecondReturnedValue::GenerateCode(MaglevAssembler* masm,
                                          const ProcessingState& state) {
  // No-op. This is just a hack that binds kReturnRegister1 to a value node.
  // kReturnRegister1 is guaranteed to be free in the register allocator, since
  // previous node in the basic block is a call.
#ifdef DEBUG
  // Check if the previous node is call.
  Node* previous = nullptr;
  for (Node* node : state.block()->nodes()) {
    if (node == this) {
      break;
    }
    previous = node;
  }
  DCHECK_NE(previous, nullptr);
  DCHECK(previous->properties().is_call());
#endif  // DEBUG
}

void InitialValue::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  // TODO(leszeks): Make this nicer.
  result().SetUnallocated(compiler::UnallocatedOperand::FIXED_SLOT,
                          (StandardFrameConstants::kExpressionsOffset -
                           UnoptimizedFrameConstants::kRegisterFileFromFp) /
                                  kSystemPointerSize +
                              source().index(),
                          vreg_state->AllocateVirtualRegister());
}
void InitialValue::GenerateCode(MaglevAssembler* masm,
                                const ProcessingState& state) {
  // No-op, the value is already in the appropriate slot.
}
void InitialValue::PrintParams(std::ostream& os,
                               MaglevGraphLabeller* graph_labeller) const {
  os << "(" << source().ToString() << ")";
}

void LoadGlobal::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseFixed(context(), kContextRegister);
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void LoadGlobal::GenerateCode(MaglevAssembler* masm,
                              const ProcessingState& state) {
  // TODO(leszeks): Port the nice Sparkplug CallBuiltin helper.
  if (typeof_mode() == TypeofMode::kNotInside) {
    using D = CallInterfaceDescriptorFor<Builtin::kLoadGlobalIC>::type;
    DCHECK_EQ(ToRegister(context()), kContextRegister);
    __ Move(D::GetRegisterParameter(D::kName), name().object());
    __ Move(D::GetRegisterParameter(D::kSlot),
            TaggedIndex::FromIntptr(feedback().index()));
    __ Move(D::GetRegisterParameter(D::kVector), feedback().vector);

    __ CallBuiltin(Builtin::kLoadGlobalIC);
  } else {
    DCHECK_EQ(typeof_mode(), TypeofMode::kInside);
    using D =
        CallInterfaceDescriptorFor<Builtin::kLoadGlobalICInsideTypeof>::type;
    DCHECK_EQ(ToRegister(context()), kContextRegister);
    __ Move(D::GetRegisterParameter(D::kName), name().object());
    __ Move(D::GetRegisterParameter(D::kSlot),
            TaggedIndex::FromIntptr(feedback().index()));
    __ Move(D::GetRegisterParameter(D::kVector), feedback().vector);

    __ CallBuiltin(Builtin::kLoadGlobalICInsideTypeof);
  }

  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}
void LoadGlobal::PrintParams(std::ostream& os,
                             MaglevGraphLabeller* graph_labeller) const {
  os << "(" << name() << ")";
}

void StoreGlobal::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  using D = CallInterfaceDescriptorFor<Builtin::kStoreGlobalIC>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(value(), D::GetRegisterParameter(D::kValue));
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void StoreGlobal::GenerateCode(MaglevAssembler* masm,
                               const ProcessingState& state) {
  using D = CallInterfaceDescriptorFor<Builtin::kStoreGlobalIC>::type;
  DCHECK_EQ(ToRegister(context()), kContextRegister);
  DCHECK_EQ(ToRegister(value()), D::GetRegisterParameter(D::kValue));
  __ Move(D::GetRegisterParameter(D::kName), name().object());
  __ Move(D::GetRegisterParameter(D::kSlot),
          TaggedIndex::FromIntptr(feedback().index()));
  __ Move(D::GetRegisterParameter(D::kVector), feedback().vector);

  __ CallBuiltin(Builtin::kStoreGlobalIC);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}
void StoreGlobal::PrintParams(std::ostream& os,
                              MaglevGraphLabeller* graph_labeller) const {
  os << "(" << name() << ")";
}

void RegisterInput::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  DefineAsFixed(vreg_state, this, input());
}
void RegisterInput::GenerateCode(MaglevAssembler* masm,
                                 const ProcessingState& state) {
  // Nothing to be done, the value is already in the register.
}
void RegisterInput::PrintParams(std::ostream& os,
                                MaglevGraphLabeller* graph_labeller) const {
  os << "(" << input() << ")";
}

void RootConstant::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  DefineAsConstant(vreg_state, this);
}
void RootConstant::GenerateCode(MaglevAssembler* masm,
                                const ProcessingState& state) {}
bool RootConstant::ToBoolean(LocalIsolate* local_isolate) const {
  switch (index_) {
    case RootIndex::kFalseValue:
    case RootIndex::kNullValue:
    case RootIndex::kUndefinedValue:
    case RootIndex::kempty_string:
      return false;
    default:
      return true;
  }
}
void RootConstant::DoLoadToRegister(MaglevAssembler* masm, Register reg) {
  __ LoadRoot(reg, index());
}
Handle<Object> RootConstant::DoReify(LocalIsolate* isolate) {
  return isolate->root_handle(index());
}
void RootConstant::PrintParams(std::ostream& os,
                               MaglevGraphLabeller* graph_labeller) const {
  os << "(" << RootsTable::name(index()) << ")";
}

void CreateEmptyArrayLiteral::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void CreateEmptyArrayLiteral::GenerateCode(MaglevAssembler* masm,
                                           const ProcessingState& state) {
  using D = CreateEmptyArrayLiteralDescriptor;
  __ Move(kContextRegister, masm->native_context().object());
  __ Move(D::GetRegisterParameter(D::kSlot), Smi::FromInt(feedback().index()));
  __ Move(D::GetRegisterParameter(D::kFeedbackVector), feedback().vector);
  __ CallBuiltin(Builtin::kCreateEmptyArrayLiteral);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void CreateArrayLiteral::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void CreateArrayLiteral::GenerateCode(MaglevAssembler* masm,
                                      const ProcessingState& state) {
  __ Move(kContextRegister, masm->native_context().object());
  __ Push(feedback().vector);
  __ Push(TaggedIndex::FromIntptr(feedback().index()));
  __ Push(constant_elements().object());
  __ Push(Smi::FromInt(flags()));
  __ CallRuntime(Runtime::kCreateArrayLiteral);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void CreateShallowArrayLiteral::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void CreateShallowArrayLiteral::GenerateCode(MaglevAssembler* masm,
                                             const ProcessingState& state) {
  using D = CreateShallowArrayLiteralDescriptor;
  __ Move(D::ContextRegister(), masm->native_context().object());
  __ Move(D::GetRegisterParameter(D::kMaybeFeedbackVector), feedback().vector);
  __ Move(D::GetRegisterParameter(D::kSlot),
          TaggedIndex::FromIntptr(feedback().index()));
  __ Move(D::GetRegisterParameter(D::kConstantElements),
          constant_elements().object());
  __ Move(D::GetRegisterParameter(D::kFlags), Smi::FromInt(flags()));
  __ CallBuiltin(Builtin::kCreateShallowArrayLiteral);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void CreateObjectLiteral::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void CreateObjectLiteral::GenerateCode(MaglevAssembler* masm,
                                       const ProcessingState& state) {
  __ Move(kContextRegister, masm->native_context().object());
  __ Push(feedback().vector);
  __ Push(TaggedIndex::FromIntptr(feedback().index()));
  __ Push(boilerplate_descriptor().object());
  __ Push(Smi::FromInt(flags()));
  __ CallRuntime(Runtime::kCreateObjectLiteral);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void CreateEmptyObjectLiteral::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  DefineAsRegister(vreg_state, this);
}
void CreateEmptyObjectLiteral::GenerateCode(MaglevAssembler* masm,
                                            const ProcessingState& state) {
  Register object = ToRegister(result());
  RegisterSnapshot save_registers = register_snapshot();
  __ Allocate(save_registers, object, map().instance_size());
  __ Move(kScratchRegister, map().object());
  __ StoreTaggedField(FieldOperand(object, HeapObject::kMapOffset),
                      kScratchRegister);
  __ LoadRoot(kScratchRegister, RootIndex::kEmptyFixedArray);
  __ StoreTaggedField(FieldOperand(object, JSObject::kPropertiesOrHashOffset),
                      kScratchRegister);
  __ StoreTaggedField(FieldOperand(object, JSObject::kElementsOffset),
                      kScratchRegister);
  __ LoadRoot(kScratchRegister, RootIndex::kUndefinedValue);
  for (int i = 0; i < map().GetInObjectProperties(); i++) {
    int offset = map().GetInObjectPropertyOffset(i);
    __ StoreTaggedField(FieldOperand(object, offset), kScratchRegister);
  }
}

void CreateShallowObjectLiteral::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void CreateShallowObjectLiteral::GenerateCode(MaglevAssembler* masm,
                                              const ProcessingState& state) {
  using D = CreateShallowObjectLiteralDescriptor;
  __ Move(D::ContextRegister(), masm->native_context().object());
  __ Move(D::GetRegisterParameter(D::kMaybeFeedbackVector), feedback().vector);
  __ Move(D::GetRegisterParameter(D::kSlot),
          TaggedIndex::FromIntptr(feedback().index()));
  __ Move(D::GetRegisterParameter(D::kDesc), boilerplate_descriptor().object());
  __ Move(D::GetRegisterParameter(D::kFlags), Smi::FromInt(flags()));
  __ CallBuiltin(Builtin::kCreateShallowObjectLiteral);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void CreateFunctionContext::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  DCHECK_LE(slot_count(),
            static_cast<uint32_t>(
                ConstructorBuiltins::MaximumFunctionContextSlots()));
  if (scope_type() == FUNCTION_SCOPE) {
    using D = CallInterfaceDescriptorFor<
        Builtin::kFastNewFunctionContextFunction>::type;
    static_assert(D::HasContextParameter());
    UseFixed(context(), D::ContextRegister());
  } else {
    DCHECK_EQ(scope_type(), ScopeType::EVAL_SCOPE);
    using D =
        CallInterfaceDescriptorFor<Builtin::kFastNewFunctionContextEval>::type;
    static_assert(D::HasContextParameter());
    UseFixed(context(), D::ContextRegister());
  }
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void CreateFunctionContext::GenerateCode(MaglevAssembler* masm,
                                         const ProcessingState& state) {
  if (scope_type() == FUNCTION_SCOPE) {
    using D = CallInterfaceDescriptorFor<
        Builtin::kFastNewFunctionContextFunction>::type;
    DCHECK_EQ(ToRegister(context()), D::ContextRegister());
    __ Move(D::GetRegisterParameter(D::kScopeInfo), scope_info().object());
    __ Move(D::GetRegisterParameter(D::kSlots), Immediate(slot_count()));
    // TODO(leszeks): Consider inlining this allocation.
    __ CallBuiltin(Builtin::kFastNewFunctionContextFunction);
  } else {
    DCHECK_EQ(scope_type(), ScopeType::EVAL_SCOPE);
    using D =
        CallInterfaceDescriptorFor<Builtin::kFastNewFunctionContextEval>::type;
    DCHECK_EQ(ToRegister(context()), D::ContextRegister());
    __ Move(D::GetRegisterParameter(D::kScopeInfo), scope_info().object());
    __ Move(D::GetRegisterParameter(D::kSlots), Immediate(slot_count()));
    __ CallBuiltin(Builtin::kFastNewFunctionContextEval);
  }
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}
void CreateFunctionContext::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << *scope_info().object() << ", " << slot_count() << ")";
}

void FastCreateClosure::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  using D = CallInterfaceDescriptorFor<Builtin::kFastNewClosure>::type;
  static_assert(D::HasContextParameter());
  UseFixed(context(), D::ContextRegister());
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void FastCreateClosure::GenerateCode(MaglevAssembler* masm,
                                     const ProcessingState& state) {
  using D = CallInterfaceDescriptorFor<Builtin::kFastNewClosure>::type;

  DCHECK_EQ(ToRegister(context()), D::ContextRegister());
  __ Move(D::GetRegisterParameter(D::kSharedFunctionInfo),
          shared_function_info().object());
  __ Move(D::GetRegisterParameter(D::kFeedbackCell), feedback_cell().object());
  __ CallBuiltin(Builtin::kFastNewClosure);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}
void FastCreateClosure::PrintParams(std::ostream& os,
                                    MaglevGraphLabeller* graph_labeller) const {
  os << "(" << *shared_function_info().object() << ", "
     << feedback_cell().object() << ")";
}

void CreateClosure::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseFixed(context(), kContextRegister);
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void CreateClosure::GenerateCode(MaglevAssembler* masm,
                                 const ProcessingState& state) {
  Runtime::FunctionId function_id =
      pretenured() ? Runtime::kNewClosure_Tenured : Runtime::kNewClosure;
  __ Push(shared_function_info().object());
  __ Push(feedback_cell().object());
  __ CallRuntime(function_id);
}
void CreateClosure::PrintParams(std::ostream& os,
                                MaglevGraphLabeller* graph_labeller) const {
  os << "(" << *shared_function_info().object() << ", "
     << feedback_cell().object();
  if (pretenured()) {
    os << " [pretenured]";
  }
  os << ")";
}

void CreateRegExpLiteral::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void CreateRegExpLiteral::GenerateCode(MaglevAssembler* masm,
                                       const ProcessingState& state) {
  using D = CreateRegExpLiteralDescriptor;
  __ Move(D::ContextRegister(), masm->native_context().object());
  __ Move(D::GetRegisterParameter(D::kMaybeFeedbackVector), feedback().vector);
  __ Move(D::GetRegisterParameter(D::kSlot),
          TaggedIndex::FromIntptr(feedback().index()));
  __ Move(D::GetRegisterParameter(D::kPattern), pattern().object());
  __ Move(D::GetRegisterParameter(D::kFlags), Smi::FromInt(flags()));
  __ CallBuiltin(Builtin::kCreateRegExpLiteral);
}

void GetTemplateObject::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  using D = GetTemplateObjectDescriptor;
  UseFixed(description(), D::GetRegisterParameter(D::kDescription));
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}

void GetTemplateObject::GenerateCode(MaglevAssembler* masm,
                                     const ProcessingState& state) {
  using D = GetTemplateObjectDescriptor;
  __ Move(D::ContextRegister(), masm->native_context().object());
  __ Move(D::GetRegisterParameter(D::kMaybeFeedbackVector), feedback().vector);
  __ Move(D::GetRegisterParameter(D::kSlot), feedback().slot.ToInt());
  __ Move(D::GetRegisterParameter(D::kShared), shared_function_info_.object());
  __ CallBuiltin(Builtin::kGetTemplateObject);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void Abort::AllocateVreg(MaglevVregAllocationState* vreg_state) {}
void Abort::GenerateCode(MaglevAssembler* masm, const ProcessingState& state) {
  __ Push(Smi::FromInt(static_cast<int>(reason())));
  __ CallRuntime(Runtime::kAbort, 1);
  __ Trap();
}
void Abort::PrintParams(std::ostream& os,
                        MaglevGraphLabeller* graph_labeller) const {
  os << "(" << GetAbortReason(reason()) << ")";
}

namespace {
Condition ToCondition(AssertCondition cond) {
  switch (cond) {
    case AssertCondition::kLess:
      return less;
    case AssertCondition::kLessOrEqual:
      return less_equal;
    case AssertCondition::kGreater:
      return greater;
    case AssertCondition::kGeaterOrEqual:
      return greater_equal;
    case AssertCondition::kEqual:
      return equal;
    case AssertCondition::kNotEqual:
      return not_equal;
  }
}

std::ostream& operator<<(std::ostream& os, const AssertCondition cond) {
  switch (cond) {
    case AssertCondition::kLess:
      os << "Less";
      break;
    case AssertCondition::kLessOrEqual:
      os << "LessOrEqual";
      break;
    case AssertCondition::kGreater:
      os << "Greater";
      break;
    case AssertCondition::kGeaterOrEqual:
      os << "GeaterOrEqual";
      break;
    case AssertCondition::kEqual:
      os << "Equal";
      break;
    case AssertCondition::kNotEqual:
      os << "NotEqual";
      break;
  }
  return os;
}
}  // namespace

void AssertInt32::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(left_input());
  UseRegister(right_input());
}
void AssertInt32::GenerateCode(MaglevAssembler* masm,
                               const ProcessingState& state) {
  __ cmpq(ToRegister(left_input()), ToRegister(right_input()));
  __ Check(ToCondition(condition_), reason_);
}
void AssertInt32::PrintParams(std::ostream& os,
                              MaglevGraphLabeller* graph_labeller) const {
  os << "(" << condition_ << ")";
}

bool AnyMapIsHeapNumber(const ZoneHandleSet<Map>& maps) {
  return std::any_of(maps.begin(), maps.end(),
                     [](Handle<Map> map) { return map->IsHeapNumberMap(); });
}

void CheckMaps::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(receiver_input());
}
void CheckMaps::GenerateCode(MaglevAssembler* masm,
                             const ProcessingState& state) {
  Register object = ToRegister(receiver_input());

  // TODO(victorgomes): This can happen, because we do not emit an unconditional
  // deopt when we intersect the map sets.
  if (maps().is_empty()) {
    __ RegisterEagerDeopt(eager_deopt_info(), DeoptimizeReason::kWrongMap);
    __ jmp(eager_deopt_info()->deopt_entry_label());
    return;
  }

  bool maps_include_heap_number = AnyMapIsHeapNumber(maps());

  Label done;
  if (check_type_ == CheckType::kOmitHeapObjectCheck) {
    __ AssertNotSmi(object);
  } else {
    Condition is_smi = __ CheckSmi(object);
    if (maps_include_heap_number) {
      // Smis count as matching the HeapNumber map, so we're done.
      __ jmp(&done);
    } else {
      __ EmitEagerDeoptIf(is_smi, DeoptimizeReason::kWrongMap, this);
    }
  }

  size_t map_count = maps().size();
  for (size_t i = 0; i < map_count - 1; ++i) {
    Handle<Map> map = maps().at(i);
    __ Cmp(FieldOperand(object, HeapObject::kMapOffset), map);
    __ j(equal, &done, Label::kNear);
  }
  Handle<Map> last_map = maps().at(map_count - 1);
  __ Cmp(FieldOperand(object, HeapObject::kMapOffset), last_map);
  __ EmitEagerDeoptIf(not_equal, DeoptimizeReason::kWrongMap, this);
  __ bind(&done);
}
void CheckMaps::PrintParams(std::ostream& os,
                            MaglevGraphLabeller* graph_labeller) const {
  os << "(";
  size_t map_count = maps().size();
  if (map_count > 0) {
    for (size_t i = 0; i < map_count - 1; ++i) {
      os << maps().at(i) << ", ";
    }
    os << maps().at(map_count - 1);
  }
  os << ")";
}
void CheckValue::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(target_input());
}
void CheckValue::GenerateCode(MaglevAssembler* masm,
                              const ProcessingState& state) {
  Register target = ToRegister(target_input());

  __ Cmp(target, value().object());
  __ EmitEagerDeoptIf(not_equal, DeoptimizeReason::kWrongValue, this);
}
void CheckValue::PrintParams(std::ostream& os,
                             MaglevGraphLabeller* graph_labeller) const {
  os << "(" << *value().object() << ")";
}
void CheckDynamicValue::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(first_input());
  UseRegister(second_input());
}
void CheckDynamicValue::GenerateCode(MaglevAssembler* masm,
                                     const ProcessingState& state) {
  Register first = ToRegister(first_input());
  Register second = ToRegister(second_input());

  __ cmpl(first, second);
  __ EmitEagerDeoptIf(not_equal, DeoptimizeReason::kWrongValue, this);
}
void CheckDynamicValue::PrintParams(std::ostream& os,
                                    MaglevGraphLabeller* graph_labeller) const {
}

void CheckSmi::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(receiver_input());
}
void CheckSmi::GenerateCode(MaglevAssembler* masm,
                            const ProcessingState& state) {
  Register object = ToRegister(receiver_input());
  Condition is_smi = __ CheckSmi(object);
  __ EmitEagerDeoptIf(NegateCondition(is_smi), DeoptimizeReason::kNotASmi,
                      this);
}
void CheckSmi::PrintParams(std::ostream& os,
                           MaglevGraphLabeller* graph_labeller) const {}

void CheckNumber::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(receiver_input());
}
void CheckNumber::GenerateCode(MaglevAssembler* masm,
                               const ProcessingState& state) {
  Label done;
  Register value = ToRegister(receiver_input());
  // If {value} is a Smi or a HeapNumber, we're done.
  __ JumpIfSmi(value, &done);
  __ CompareRoot(FieldOperand(value, HeapObject::kMapOffset),
                 RootIndex::kHeapNumberMap);
  if (mode() == Object::Conversion::kToNumeric) {
    // Jump to done if it is a HeapNumber.
    __ j(equal, &done);
    // Check if it is a BigInt.
    __ LoadMap(kScratchRegister, value);
    __ cmpw(FieldOperand(kScratchRegister, Map::kInstanceTypeOffset),
            Immediate(BIGINT_TYPE));
  }
  __ EmitEagerDeoptIf(not_equal, DeoptimizeReason::kNotANumber, this);
  __ bind(&done);
}

void CheckHeapObject::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(receiver_input());
}
void CheckHeapObject::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  Register object = ToRegister(receiver_input());
  Condition is_smi = __ CheckSmi(object);
  __ EmitEagerDeoptIf(is_smi, DeoptimizeReason::kSmi, this);
}
void CheckHeapObject::PrintParams(std::ostream& os,
                                  MaglevGraphLabeller* graph_labeller) const {}
void CheckSymbol::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(receiver_input());
}
void CheckSymbol::GenerateCode(MaglevAssembler* masm,
                               const ProcessingState& state) {
  Register object = ToRegister(receiver_input());
  if (check_type_ == CheckType::kOmitHeapObjectCheck) {
    __ AssertNotSmi(object);
  } else {
    Condition is_smi = __ CheckSmi(object);
    __ EmitEagerDeoptIf(is_smi, DeoptimizeReason::kNotASymbol, this);
  }
  __ LoadMap(kScratchRegister, object);
  __ CmpInstanceType(kScratchRegister, SYMBOL_TYPE);
  __ EmitEagerDeoptIf(not_equal, DeoptimizeReason::kNotASymbol, this);
}
void CheckSymbol::PrintParams(std::ostream& os,
                              MaglevGraphLabeller* graph_labeller) const {}

void CheckInstanceType::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(receiver_input());
}
void CheckInstanceType::GenerateCode(MaglevAssembler* masm,
                                     const ProcessingState& state) {
  Register object = ToRegister(receiver_input());
  if (check_type_ == CheckType::kOmitHeapObjectCheck) {
    __ AssertNotSmi(object);
  } else {
    Condition is_smi = __ CheckSmi(object);
    __ EmitEagerDeoptIf(is_smi, DeoptimizeReason::kWrongInstanceType, this);
  }
  __ LoadMap(kScratchRegister, object);
  __ CmpInstanceType(kScratchRegister, instance_type());
  __ EmitEagerDeoptIf(not_equal, DeoptimizeReason::kWrongInstanceType, this);
}
void CheckInstanceType::PrintParams(std::ostream& os,
                                    MaglevGraphLabeller* graph_labeller) const {
  os << "(" << instance_type() << ")";
}

void CheckString::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(receiver_input());
}
void CheckString::GenerateCode(MaglevAssembler* masm,
                               const ProcessingState& state) {
  Register object = ToRegister(receiver_input());
  if (check_type_ == CheckType::kOmitHeapObjectCheck) {
    __ AssertNotSmi(object);
  } else {
    Condition is_smi = __ CheckSmi(object);
    __ EmitEagerDeoptIf(is_smi, DeoptimizeReason::kNotAString, this);
  }
  __ LoadMap(kScratchRegister, object);
  __ CmpInstanceTypeRange(kScratchRegister, kScratchRegister, FIRST_STRING_TYPE,
                          LAST_STRING_TYPE);
  __ EmitEagerDeoptIf(above, DeoptimizeReason::kNotAString, this);
}
void CheckString::PrintParams(std::ostream& os,
                              MaglevGraphLabeller* graph_labeller) const {}

void CheckMapsWithMigration::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(receiver_input());
}
void CheckMapsWithMigration::GenerateCode(MaglevAssembler* masm,
                                          const ProcessingState& state) {
  __ RegisterEagerDeopt(eager_deopt_info(), DeoptimizeReason::kWrongMap);

  // TODO(victorgomes): This can happen, because we do not emit an unconditional
  // deopt when we intersect the map sets.
  if (maps().is_empty()) {
    __ jmp(eager_deopt_info()->deopt_entry_label());
    return;
  }

  Register object = ToRegister(receiver_input());

  bool maps_include_heap_number = AnyMapIsHeapNumber(maps());

  ZoneLabelRef done(masm);
  if (check_type_ == CheckType::kOmitHeapObjectCheck) {
    __ AssertNotSmi(object);
  } else {
    Condition is_smi = __ CheckSmi(object);
    if (maps_include_heap_number) {
      // Smis count as matching the HeapNumber map, so we're done.
      __ jmp(*done);
    } else {
      __ j(is_smi, eager_deopt_info()->deopt_entry_label());
    }
  }

  size_t map_count = maps().size();
  for (size_t i = 0; i < map_count; ++i) {
    ZoneLabelRef continue_label(masm);
    Handle<Map> map = maps().at(i);
    __ Cmp(FieldOperand(object, HeapObject::kMapOffset), map);

    bool last_map = (i == map_count - 1);
    if (map->is_migration_target()) {
      __ JumpToDeferredIf(
          not_equal,
          [](MaglevAssembler* masm, ZoneLabelRef continue_label,
             ZoneLabelRef done, Register object, int map_index,
             CheckMapsWithMigration* node) {
            // Reload the map to avoid needing to save it on a temporary in the
            // fast path.
            __ LoadMap(kScratchRegister, object);
            // If the map is not deprecated, we fail the map check, continue to
            // the next one.
            __ movl(kScratchRegister,
                    FieldOperand(kScratchRegister, Map::kBitField3Offset));
            __ testl(kScratchRegister,
                     Immediate(Map::Bits3::IsDeprecatedBit::kMask));
            __ j(zero, *continue_label);

            // Otherwise, try migrating the object. If the migration
            // returns Smi zero, then it failed the migration.
            Register return_val = Register::no_reg();
            {
              RegisterSnapshot register_snapshot = node->register_snapshot();
              // We can eager deopt after the snapshot, so make sure the nodes
              // used by the deopt are included in it.
              // TODO(leszeks): This is a bit of a footgun -- we likely want the
              // snapshot to always include eager deopt input registers.
              AddDeoptRegistersToSnapshot(&register_snapshot,
                                          node->eager_deopt_info());
              SaveRegisterStateForCall save_register_state(masm,
                                                           register_snapshot);

              __ Push(object);
              __ Move(kContextRegister, masm->native_context().object());
              __ CallRuntime(Runtime::kTryMigrateInstance);
              save_register_state.DefineSafepoint();

              // Make sure the return value is preserved across the live
              // register restoring pop all.
              return_val = kReturnRegister0;
              if (node->register_snapshot().live_registers.has(return_val)) {
                DCHECK(!node->register_snapshot().live_registers.has(
                    kScratchRegister));
                __ movq(kScratchRegister, return_val);
                return_val = kScratchRegister;
              }
            }

            // On failure, the returned value is zero
            __ cmpl(return_val, Immediate(0));
            __ j(equal, *continue_label);

            // The migrated object is returned on success, retry the map check.
            __ Move(object, return_val);
            // Manually load the map pointer without uncompressing it.
            __ Cmp(FieldOperand(object, HeapObject::kMapOffset),
                   node->maps().at(map_index));
            __ j(equal, *done);
            __ jmp(*continue_label);
          },
          // If this is the last map to check, we should deopt if we fail.
          // This is safe to do, since {eager_deopt_info} is ZoneAllocated.
          (last_map ? ZoneLabelRef::UnsafeFromLabelPointer(
                          eager_deopt_info()->deopt_entry_label())
                    : continue_label),
          done, object, i, this);
    } else if (last_map) {
      // If it is the last map and it is not a migration target, we should deopt
      // if the check fails.
      __ j(not_equal, eager_deopt_info()->deopt_entry_label());
    }

    if (!last_map) {
      // We don't need to bind the label for the last map.
      __ j(equal, *done);
      __ bind(*continue_label);
    }
  }

  __ bind(*done);
}
void CheckMapsWithMigration::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(";
  size_t map_count = maps().size();
  if (map_count > 0) {
    for (size_t i = 0; i < map_count - 1; ++i) {
      os << maps().at(i) << ", ";
    }
    os << maps().at(map_count - 1);
  }
  os << ")";
}

void CheckJSArrayBounds::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(receiver_input());
  UseRegister(index_input());
}
void CheckJSArrayBounds::GenerateCode(MaglevAssembler* masm,
                                      const ProcessingState& state) {
  Register object = ToRegister(receiver_input());
  Register index = ToRegister(index_input());
  __ AssertNotSmi(object);

  if (v8_flags.debug_code) {
    __ CmpObjectType(object, JS_ARRAY_TYPE, kScratchRegister);
    __ Assert(equal, AbortReason::kUnexpectedValue);
  }
  __ SmiUntagField(kScratchRegister,
                   FieldOperand(object, JSArray::kLengthOffset));
  __ cmpl(index, kScratchRegister);
  __ EmitEagerDeoptIf(above_equal, DeoptimizeReason::kOutOfBounds, this);
}

namespace {
int ElementsKindSize(ElementsKind element_kind) {
  switch (element_kind) {
#define TYPED_ARRAY_CASE(Type, type, TYPE, ctype) \
  case TYPE##_ELEMENTS:                           \
    DCHECK_LE(sizeof(ctype), 8);                  \
    return sizeof(ctype);
    TYPED_ARRAYS(TYPED_ARRAY_CASE)
    default:
      UNREACHABLE();
#undef TYPED_ARRAY_CASE
  }
}

int ExternalArrayElementSize(const ExternalArrayType element_type) {
  switch (element_type) {
#define TYPED_ARRAY_CASE(Type, type, TYPE, ctype) \
  case kExternal##Type##Array:                    \
    DCHECK_LE(sizeof(ctype), 8);                  \
    return sizeof(ctype);
    TYPED_ARRAYS(TYPED_ARRAY_CASE)
    default:
      UNREACHABLE();
#undef TYPED_ARRAY_CASE
  }
}
}  // namespace

void CheckJSTypedArrayBounds::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(receiver_input());
  if (ElementsKindSize(elements_kind_) == 1) {
    UseRegister(index_input());
  } else {
    UseAndClobberRegister(index_input());
  }
}
void CheckJSTypedArrayBounds::GenerateCode(MaglevAssembler* masm,
                                           const ProcessingState& state) {
  Register object = ToRegister(receiver_input());
  Register index = ToRegister(index_input());
  Register byte_length = kScratchRegister;
  if (v8_flags.debug_code) {
    __ AssertNotSmi(object);
    __ CmpObjectType(object, JS_TYPED_ARRAY_TYPE, kScratchRegister);
    __ Assert(equal, AbortReason::kUnexpectedValue);
  }
  __ LoadBoundedSizeFromObject(byte_length, object,
                               JSTypedArray::kRawByteLengthOffset);
  int element_size = ElementsKindSize(elements_kind_);
  if (element_size > 1) {
    DCHECK(element_size == 2 || element_size == 4);
    __ shlq(index, Immediate(element_size / 2));
  }
  __ cmpq(index, byte_length);
  // We use {above_equal} which does an unsigned comparison to handle negative
  // indices as well.
  __ EmitEagerDeoptIf(above_equal, DeoptimizeReason::kOutOfBounds, this);
}

void CheckJSDataViewBounds::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(receiver_input());
  UseRegister(index_input());
}
void CheckJSDataViewBounds::GenerateCode(MaglevAssembler* masm,
                                         const ProcessingState& state) {
  Register object = ToRegister(receiver_input());
  Register index = ToRegister(index_input());
  Register byte_length = kScratchRegister;
  if (v8_flags.debug_code) {
    __ AssertNotSmi(object);
    __ CmpObjectType(object, JS_DATA_VIEW_TYPE, kScratchRegister);
    __ Assert(equal, AbortReason::kUnexpectedValue);
  }
  __ LoadBoundedSizeFromObject(byte_length, object,
                               JSDataView::kRawByteLengthOffset);
  int element_size = ExternalArrayElementSize(element_type_);
  if (element_size > 1) {
    __ subq(byte_length, Immediate(element_size - 1));
    __ EmitEagerDeoptIf(negative, DeoptimizeReason::kOutOfBounds, this);
  }
  __ cmpl(index, byte_length);
  __ EmitEagerDeoptIf(above_equal, DeoptimizeReason::kOutOfBounds, this);
}

void CheckJSObjectElementsBounds::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(receiver_input());
  UseRegister(index_input());
}
void CheckJSObjectElementsBounds::GenerateCode(MaglevAssembler* masm,
                                               const ProcessingState& state) {
  Register object = ToRegister(receiver_input());
  Register index = ToRegister(index_input());
  __ AssertNotSmi(object);

  if (v8_flags.debug_code) {
    __ CmpObjectType(object, FIRST_JS_OBJECT_TYPE, kScratchRegister);
    __ Assert(greater_equal, AbortReason::kUnexpectedValue);
  }
  __ LoadAnyTaggedField(kScratchRegister,
                        FieldOperand(object, JSObject::kElementsOffset));
  if (v8_flags.debug_code) {
    __ AssertNotSmi(kScratchRegister);
  }
  __ SmiUntagField(kScratchRegister,
                   FieldOperand(kScratchRegister, FixedArray::kLengthOffset));
  __ cmpl(index, kScratchRegister);
  __ EmitEagerDeoptIf(above_equal, DeoptimizeReason::kOutOfBounds, this);
}

void CheckInt32Condition::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(left_input());
  UseRegister(right_input());
}
void CheckInt32Condition::GenerateCode(MaglevAssembler* masm,
                                       const ProcessingState& state) {
  __ cmpq(ToRegister(left_input()), ToRegister(right_input()));
  __ EmitEagerDeoptIf(NegateCondition(ToCondition(condition_)), reason_, this);
}
void CheckInt32Condition::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << condition_ << ")";
}

void DebugBreak::AllocateVreg(MaglevVregAllocationState* vreg_state) {}
void DebugBreak::GenerateCode(MaglevAssembler* masm,
                              const ProcessingState& state) {
  __ int3();
}

void CheckedInternalizedString::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(object_input());
  set_temporaries_needed(1);
  DefineSameAsFirst(vreg_state, this);
}
void CheckedInternalizedString::GenerateCode(MaglevAssembler* masm,
                                             const ProcessingState& state) {
  Register object = ToRegister(object_input());
  RegList temps = general_temporaries();
  Register map_tmp = temps.PopFirst();

  if (check_type_ == CheckType::kOmitHeapObjectCheck) {
    __ AssertNotSmi(object);
  } else {
    Condition is_smi = __ CheckSmi(object);
    __ EmitEagerDeoptIf(is_smi, DeoptimizeReason::kWrongMap, this);
  }

  __ LoadMap(map_tmp, object);
  __ RecordComment("Test IsInternalizedString");
  // Go to the slow path if this is a non-string, or a non-internalised string.
  __ testw(FieldOperand(map_tmp, Map::kInstanceTypeOffset),
           Immediate(kIsNotStringMask | kIsNotInternalizedMask));
  static_assert((kStringTag | kInternalizedTag) == 0);
  ZoneLabelRef done(masm);
  __ JumpToDeferredIf(
      not_zero,
      [](MaglevAssembler* masm, ZoneLabelRef done, Register object,
         CheckedInternalizedString* node, EagerDeoptInfo* deopt_info,
         Register map_tmp) {
        __ RecordComment("Deferred Test IsThinString");
        __ movw(map_tmp, FieldOperand(map_tmp, Map::kInstanceTypeOffset));
        static_assert(kThinStringTagBit > 0);
        // Deopt if this isn't a string.
        __ testw(map_tmp, Immediate(kIsNotStringMask));
        __ j(not_zero, deopt_info->deopt_entry_label());
        // Deopt if this isn't a thin string.
        __ testb(map_tmp, Immediate(kThinStringTagBit));
        __ j(zero, deopt_info->deopt_entry_label());
        __ LoadTaggedPointerField(
            object, FieldOperand(object, ThinString::kActualOffset));
        if (v8_flags.debug_code) {
          __ RecordComment("DCHECK IsInternalizedString");
          __ LoadMap(map_tmp, object);
          __ testw(FieldOperand(map_tmp, Map::kInstanceTypeOffset),
                   Immediate(kIsNotStringMask | kIsNotInternalizedMask));
          static_assert((kStringTag | kInternalizedTag) == 0);
          __ Check(zero, AbortReason::kUnexpectedValue);
        }
        __ jmp(*done);
      },
      done, object, this, eager_deopt_info(), map_tmp);
  __ bind(*done);
}

void CheckedObjectToIndex::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(object_input());
  DefineAsRegister(vreg_state, this);
  set_double_temporaries_needed(1);
}
void CheckedObjectToIndex::GenerateCode(MaglevAssembler* masm,
                                        const ProcessingState& state) {
  Register object = ToRegister(object_input());
  Register result_reg = ToRegister(result());

  ZoneLabelRef done(masm);
  Condition is_smi = __ CheckSmi(object);
  __ JumpToDeferredIf(
      NegateCondition(is_smi),
      [](MaglevAssembler* masm, Register object, Register result_reg,
         ZoneLabelRef done, CheckedObjectToIndex* node) {
        Label is_string;
        __ LoadMap(kScratchRegister, object);
        __ CmpInstanceTypeRange(kScratchRegister, kScratchRegister,
                                FIRST_STRING_TYPE, LAST_STRING_TYPE);
        __ j(below_equal, &is_string);

        __ cmpl(kScratchRegister, Immediate(HEAP_NUMBER_TYPE));
        // The IC will go generic if it encounters something other than a
        // Number or String key.
        __ EmitEagerDeoptIf(not_equal, DeoptimizeReason::kNotInt32, node);

        // Heap Number.
        {
          DoubleRegister number_value = node->double_temporaries().first();
          DoubleRegister converted_back = kScratchDoubleReg;
          // Convert the input float64 value to int32.
          __ Cvttsd2si(result_reg, number_value);
          // Convert that int32 value back to float64.
          __ Cvtlsi2sd(converted_back, result_reg);
          // Check that the result of the float64->int32->float64 is equal to
          // the input (i.e. that the conversion didn't truncate.
          __ Ucomisd(number_value, converted_back);
          __ j(equal, *done);
          __ EmitEagerDeopt(node, DeoptimizeReason::kNotInt32);
        }

        // String.
        __ bind(&is_string);
        {
          RegisterSnapshot snapshot = node->register_snapshot();
          snapshot.live_registers.clear(result_reg);
          DCHECK(!snapshot.live_tagged_registers.has(result_reg));
          {
            SaveRegisterStateForCall save_register_state(masm, snapshot);
            AllowExternalCallThatCantCauseGC scope(masm);
            __ PrepareCallCFunction(1);
            __ Move(arg_reg_1, object);
            __ CallCFunction(
                ExternalReference::string_to_array_index_function(), 1);
            // No need for safepoint since this is a fast C call.
            __ Move(result_reg, kReturnRegister0);
          }
          __ cmpl(result_reg, Immediate(0));
          __ j(greater_equal, *done);
          __ EmitEagerDeopt(node, DeoptimizeReason::kNotInt32);
        }
      },
      object, result_reg, done, this);

  // If we didn't enter the deferred block, we're a Smi.
  if (result_reg == object) {
    __ SmiUntag(object);
  } else {
    __ SmiUntag(result_reg, object);
  }

  __ bind(*done);
}

void BuiltinStringFromCharCode::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  if (code_input().node()->Is<Int32Constant>()) {
    UseAny(code_input());
  } else {
    UseAndClobberRegister(code_input());
    set_temporaries_needed(1);
  }
  DefineAsRegister(vreg_state, this);
}
void BuiltinStringFromCharCode::GenerateCode(MaglevAssembler* masm,
                                             const ProcessingState& state) {
  Register result_string = ToRegister(result());
  if (Int32Constant* constant = code_input().node()->TryCast<Int32Constant>()) {
    int32_t char_code = constant->value();
    if (0 <= char_code && char_code < String::kMaxOneByteCharCode) {
      __ LoadSingleCharacterString(result_string, char_code);
    } else {
      __ AllocateTwoByteString(register_snapshot(), result_string, 1);
      __ movw(FieldOperand(result_string, SeqTwoByteString::kHeaderSize),
              Immediate(char_code & 0xFFFF));
    }
  } else {
    Register char_code = ToRegister(code_input());
    // We only need a scratch here if {char_code} alias with {result}.
    // TODO(victorgomes): Add a constraint in the register allocator for this
    // use case?
    Register scratch = general_temporaries().PopFirst();
    __ StringFromCharCode(register_snapshot(), nullptr, result_string,
                          char_code, scratch);
  }
}

void BuiltinStringPrototypeCharCodeAt::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseAndClobberRegister(string_input());
  UseAndClobberRegister(index_input());
  DefineAsRegister(vreg_state, this);
  set_temporaries_needed(1);
}

void BuiltinStringPrototypeCharCodeAt::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  Register string = ToRegister(string_input());
  Register index = ToRegister(index_input());
  Register scratch = general_temporaries().PopFirst();
  ZoneLabelRef done(masm);
  RegisterSnapshot save_registers = register_snapshot();
  __ StringCharCodeAt(save_registers, ToRegister(result()), string, index,
                      scratch, *done);
  __ bind(*done);
}

void LoadTaggedField::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(object_input());
  DefineAsRegister(vreg_state, this);
}
void LoadTaggedField::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  Register object = ToRegister(object_input());
  __ AssertNotSmi(object);
  __ DecompressAnyTagged(ToRegister(result()), FieldOperand(object, offset()));
}
void LoadTaggedField::PrintParams(std::ostream& os,
                                  MaglevGraphLabeller* graph_labeller) const {
  os << "(0x" << std::hex << offset() << std::dec << ")";
}

void LoadDoubleField::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(object_input());
  DefineAsRegister(vreg_state, this);
  set_temporaries_needed(1);
}
void LoadDoubleField::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  Register tmp = general_temporaries().PopFirst();
  Register object = ToRegister(object_input());
  __ AssertNotSmi(object);
  __ DecompressAnyTagged(tmp, FieldOperand(object, offset()));
  __ AssertNotSmi(tmp);
  __ Movsd(ToDoubleRegister(result()),
           FieldOperand(tmp, HeapNumber::kValueOffset));
}
void LoadDoubleField::PrintParams(std::ostream& os,
                                  MaglevGraphLabeller* graph_labeller) const {
  os << "(0x" << std::hex << offset() << std::dec << ")";
}

void LoadTaggedElement::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(object_input());
  UseRegister(index_input());
  DefineAsRegister(vreg_state, this);
}
void LoadTaggedElement::GenerateCode(MaglevAssembler* masm,
                                     const ProcessingState& state) {
  Register object = ToRegister(object_input());
  Register index = ToRegister(index_input());
  Register result_reg = ToRegister(result());
  __ AssertNotSmi(object);
  if (v8_flags.debug_code) {
    __ CmpObjectType(object, JS_OBJECT_TYPE, kScratchRegister);
    __ Assert(above_equal, AbortReason::kUnexpectedValue);
  }
  __ DecompressAnyTagged(kScratchRegister,
                         FieldOperand(object, JSObject::kElementsOffset));
  if (v8_flags.debug_code) {
    __ CmpObjectType(kScratchRegister, FIXED_ARRAY_TYPE, kScratchRegister);
    __ Assert(equal, AbortReason::kUnexpectedValue);
    // Reload since CmpObjectType clobbered the scratch register.
    __ DecompressAnyTagged(kScratchRegister,
                           FieldOperand(object, JSObject::kElementsOffset));
  }
  __ DecompressAnyTagged(
      result_reg, FieldOperand(kScratchRegister, index, times_tagged_size,
                               FixedArray::kHeaderSize));
}

void LoadDoubleElement::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(object_input());
  UseRegister(index_input());
  DefineAsRegister(vreg_state, this);
}
void LoadDoubleElement::GenerateCode(MaglevAssembler* masm,
                                     const ProcessingState& state) {
  Register object = ToRegister(object_input());
  Register index = ToRegister(index_input());
  DoubleRegister result_reg = ToDoubleRegister(result());
  __ AssertNotSmi(object);
  if (v8_flags.debug_code) {
    __ CmpObjectType(object, JS_OBJECT_TYPE, kScratchRegister);
    __ Assert(above_equal, AbortReason::kUnexpectedValue);
  }
  __ DecompressAnyTagged(kScratchRegister,
                         FieldOperand(object, JSObject::kElementsOffset));
  if (v8_flags.debug_code) {
    __ CmpObjectType(kScratchRegister, FIXED_DOUBLE_ARRAY_TYPE,
                     kScratchRegister);
    __ Assert(equal, AbortReason::kUnexpectedValue);
    // Reload since CmpObjectType clobbered the scratch register.
    __ DecompressAnyTagged(kScratchRegister,
                           FieldOperand(object, JSObject::kElementsOffset));
  }
  __ Movsd(result_reg, FieldOperand(kScratchRegister, index, times_8,
                                    FixedDoubleArray::kHeaderSize));
}

namespace {
bool FromConstantToBool(MaglevAssembler* masm, ValueNode* node) {
  DCHECK(IsConstantNode(node->opcode()));
  LocalIsolate* local_isolate = masm->isolate()->AsLocalIsolate();
  switch (node->opcode()) {
#define CASE(Name)                                       \
  case Opcode::k##Name: {                                \
    return node->Cast<Name>()->ToBoolean(local_isolate); \
  }
    CONSTANT_VALUE_NODE_LIST(CASE)
#undef CASE
    default:
      UNREACHABLE();
  }
}
}  // namespace

void LoadSignedIntDataViewElement::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(object_input());
  UseRegister(index_input());
  if (is_little_endian_constant() ||
      type_ == ExternalArrayType::kExternalInt8Array) {
    UseAny(is_little_endian_input());
  } else {
    UseRegister(is_little_endian_input());
  }
  set_temporaries_needed(1);
  DefineAsRegister(vreg_state, this);
}
void LoadSignedIntDataViewElement::GenerateCode(MaglevAssembler* masm,
                                                const ProcessingState& state) {
  Register object = ToRegister(object_input());
  Register index = ToRegister(index_input());
  Register result_reg = ToRegister(result());
  Register data_pointer = general_temporaries().PopFirst();

  __ AssertNotSmi(object);
  if (v8_flags.debug_code) {
    __ CmpObjectType(object, JS_DATA_VIEW_TYPE, kScratchRegister);
    __ Assert(above_equal, AbortReason::kUnexpectedValue);
  }

  // Load data pointer.
  __ LoadExternalPointerField(
      data_pointer, FieldOperand(object, JSDataView::kDataPointerOffset));

  int element_size = ExternalArrayElementSize(type_);
  __ LoadSignedField(result_reg, Operand(data_pointer, index, times_1, 0),
                     element_size);

  // We ignore little endian argument if type is a byte size.
  if (type_ != ExternalArrayType::kExternalInt8Array) {
    if (is_little_endian_constant()) {
      if (!FromConstantToBool(masm, is_little_endian_input().node())) {
        __ ReverseByteOrder(result_reg, element_size);
      }
    } else {
      ZoneLabelRef is_little_endian(masm), is_big_endian(masm);
      __ ToBoolean(ToRegister(is_little_endian_input()), is_little_endian,
                   is_big_endian, false);
      __ bind(*is_big_endian);
      __ ReverseByteOrder(result_reg, element_size);
      __ bind(*is_little_endian);
      // x64 is little endian.
      static_assert(V8_TARGET_LITTLE_ENDIAN == 1);
    }
  }
}

void StoreSignedIntDataViewElement::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(object_input());
  UseRegister(index_input());
  if (ExternalArrayElementSize(type_) > 1) {
    UseAndClobberRegister(value_input());
  } else {
    UseRegister(value_input());
  }
  if (is_little_endian_constant() ||
      type_ == ExternalArrayType::kExternalInt8Array) {
    UseAny(is_little_endian_input());
  } else {
    UseRegister(is_little_endian_input());
  }
  set_temporaries_needed(1);
}
void StoreSignedIntDataViewElement::GenerateCode(MaglevAssembler* masm,
                                                 const ProcessingState& state) {
  Register object = ToRegister(object_input());
  Register index = ToRegister(index_input());
  Register value = ToRegister(value_input());
  Register data_pointer = general_temporaries().PopFirst();

  __ AssertNotSmi(object);
  if (v8_flags.debug_code) {
    __ CmpObjectType(object, JS_DATA_VIEW_TYPE, kScratchRegister);
    __ Assert(above_equal, AbortReason::kUnexpectedValue);
  }

  // Load data pointer.
  __ LoadExternalPointerField(
      data_pointer, FieldOperand(object, JSDataView::kDataPointerOffset));

  int element_size = ExternalArrayElementSize(type_);

  // We ignore little endian argument if type is a byte size.
  if (element_size > 1) {
    if (is_little_endian_constant()) {
      if (!FromConstantToBool(masm, is_little_endian_input().node())) {
        __ ReverseByteOrder(value, element_size);
      }
    } else {
      ZoneLabelRef is_little_endian(masm), is_big_endian(masm);
      __ ToBoolean(ToRegister(is_little_endian_input()), is_little_endian,
                   is_big_endian, false);
      __ bind(*is_big_endian);
      __ ReverseByteOrder(value, element_size);
      __ bind(*is_little_endian);
      // x64 is little endian.
      static_assert(V8_TARGET_LITTLE_ENDIAN == 1);
    }
  }

  __ StoreField(Operand(data_pointer, index, times_1, 0), value, element_size);
}

void LoadDoubleDataViewElement::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(object_input());
  UseRegister(index_input());
  if (is_little_endian_constant()) {
    UseAny(is_little_endian_input());
  } else {
    UseRegister(is_little_endian_input());
  }
  set_temporaries_needed(1);
  DefineAsRegister(vreg_state, this);
}
void LoadDoubleDataViewElement::GenerateCode(MaglevAssembler* masm,
                                             const ProcessingState& state) {
  Register object = ToRegister(object_input());
  Register index = ToRegister(index_input());
  DoubleRegister result_reg = ToDoubleRegister(result());
  Register data_pointer = general_temporaries().PopFirst();

  __ AssertNotSmi(object);
  if (v8_flags.debug_code) {
    __ CmpObjectType(object, JS_DATA_VIEW_TYPE, kScratchRegister);
    __ Assert(above_equal, AbortReason::kUnexpectedValue);
  }

  // Load data pointer.
  __ LoadExternalPointerField(
      data_pointer, FieldOperand(object, JSDataView::kDataPointerOffset));

  if (is_little_endian_constant()) {
    if (FromConstantToBool(masm, is_little_endian_input().node())) {
      __ Movsd(result_reg, Operand(data_pointer, index, times_1, 0));
    } else {
      __ movq(kScratchRegister, Operand(data_pointer, index, times_1, 0));
      __ bswapq(kScratchRegister);
      __ Movq(result_reg, kScratchRegister);
    }
  } else {
    Label done;
    ZoneLabelRef is_little_endian(masm), is_big_endian(masm);
    // TODO(leszeks): We're likely to be calling this on an existing boolean --
    // maybe that's a case we should fast-path here and re-use that boolean
    // value?
    __ ToBoolean(ToRegister(is_little_endian_input()), is_little_endian,
                 is_big_endian, true);
    // x64 is little endian.
    static_assert(V8_TARGET_LITTLE_ENDIAN == 1);
    __ bind(*is_little_endian);
    __ Movsd(result_reg, Operand(data_pointer, index, times_1, 0));
    __ jmp(&done);
    // We should swap the bytes if big endian.
    __ bind(*is_big_endian);
    __ movq(kScratchRegister, Operand(data_pointer, index, times_1, 0));
    __ bswapq(kScratchRegister);
    __ Movq(result_reg, kScratchRegister);
    __ bind(&done);
  }
}

void StoreDoubleDataViewElement::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(object_input());
  UseRegister(index_input());
  UseRegister(value_input());
  if (is_little_endian_constant()) {
    UseAny(is_little_endian_input());
  } else {
    UseRegister(is_little_endian_input());
  }
  set_temporaries_needed(1);
}
void StoreDoubleDataViewElement::GenerateCode(MaglevAssembler* masm,
                                              const ProcessingState& state) {
  Register object = ToRegister(object_input());
  Register index = ToRegister(index_input());
  DoubleRegister value = ToDoubleRegister(value_input());
  Register data_pointer = general_temporaries().PopFirst();

  __ AssertNotSmi(object);
  if (v8_flags.debug_code) {
    __ CmpObjectType(object, JS_DATA_VIEW_TYPE, kScratchRegister);
    __ Assert(above_equal, AbortReason::kUnexpectedValue);
  }

  // Load data pointer.
  __ LoadExternalPointerField(
      data_pointer, FieldOperand(object, JSDataView::kDataPointerOffset));

  if (is_little_endian_constant()) {
    if (FromConstantToBool(masm, is_little_endian_input().node())) {
      __ Movsd(Operand(data_pointer, index, times_1, 0), value);
    } else {
      __ Movq(kScratchRegister, value);
      __ bswapq(kScratchRegister);
      __ movq(Operand(data_pointer, index, times_1, 0), kScratchRegister);
    }
  } else {
    Label done;
    ZoneLabelRef is_little_endian(masm), is_big_endian(masm);
    // TODO(leszeks): We're likely to be calling this on an existing boolean --
    // maybe that's a case we should fast-path here and re-use that boolean
    // value?
    __ ToBoolean(ToRegister(is_little_endian_input()), is_little_endian,
                 is_big_endian, true);
    // x64 is little endian.
    static_assert(V8_TARGET_LITTLE_ENDIAN == 1);
    __ bind(*is_little_endian);
    __ Movsd(Operand(data_pointer, index, times_1, 0), value);
    __ jmp(&done);
    // We should swap the bytes if big endian.
    __ bind(*is_big_endian);
    __ Movq(kScratchRegister, value);
    __ bswapq(kScratchRegister);
    __ movq(Operand(data_pointer, index, times_1, 0), kScratchRegister);
    __ bind(&done);
  }
}

namespace {

ScaleFactor ScaleFactorFromInt(int n) {
  switch (n) {
    case 1:
      return times_1;
    case 2:
      return times_2;
    case 4:
      return times_4;
    default:
      UNREACHABLE();
  }
}

}  // namespace

void LoadSignedIntTypedArrayElement::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(object_input());
  UseRegister(index_input());
  DefineAsRegister(vreg_state, this);
}
void LoadSignedIntTypedArrayElement::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  Register object = ToRegister(object_input());
  Register index = ToRegister(index_input());
  Register result_reg = ToRegister(result());
  Register data_pointer = result_reg;
  __ AssertNotSmi(object);
  if (v8_flags.debug_code) {
    __ CmpObjectType(object, JS_TYPED_ARRAY_TYPE, kScratchRegister);
    __ Assert(equal, AbortReason::kUnexpectedValue);
  }
  int element_size = ElementsKindSize(elements_kind_);
  __ LoadExternalPointerField(
      data_pointer, FieldOperand(object, JSTypedArray::kExternalPointerOffset));
  __ LoadSignedField(
      result_reg,
      Operand(data_pointer, index, ScaleFactorFromInt(element_size), 0),
      element_size);
}

void LoadUnsignedIntTypedArrayElement::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(object_input());
  UseRegister(index_input());
  DefineAsRegister(vreg_state, this);
}
void LoadUnsignedIntTypedArrayElement::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  Register object = ToRegister(object_input());
  Register index = ToRegister(index_input());
  Register result_reg = ToRegister(result());
  Register data_pointer = result_reg;
  __ AssertNotSmi(object);
  if (v8_flags.debug_code) {
    __ CmpObjectType(object, JS_TYPED_ARRAY_TYPE, kScratchRegister);
    __ Assert(equal, AbortReason::kUnexpectedValue);
  }
  int element_size = ElementsKindSize(elements_kind_);
  __ LoadExternalPointerField(
      data_pointer, FieldOperand(object, JSTypedArray::kExternalPointerOffset));
  __ LoadUnsignedField(
      result_reg,
      Operand(data_pointer, index, ScaleFactorFromInt(element_size), 0),
      element_size);
}

void LoadDoubleTypedArrayElement::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(object_input());
  UseRegister(index_input());
  DefineAsRegister(vreg_state, this);
}
void LoadDoubleTypedArrayElement::GenerateCode(MaglevAssembler* masm,
                                               const ProcessingState& state) {
  Register object = ToRegister(object_input());
  Register index = ToRegister(index_input());
  DoubleRegister result_reg = ToDoubleRegister(result());
  Register data_pointer = kScratchRegister;
  __ AssertNotSmi(object);
  if (v8_flags.debug_code) {
    __ CmpObjectType(object, JS_TYPED_ARRAY_TYPE, kScratchRegister);
    __ Assert(equal, AbortReason::kUnexpectedValue);
  }
  __ LoadExternalPointerField(
      data_pointer, FieldOperand(object, JSTypedArray::kExternalPointerOffset));
  switch (elements_kind_) {
    case FLOAT32_ELEMENTS:
      __ Movss(result_reg, Operand(data_pointer, index, times_4, 0));
      __ Cvtss2sd(result_reg, result_reg);
      break;
    case FLOAT64_ELEMENTS:
      __ Movsd(result_reg, Operand(data_pointer, index, times_8, 0));
      break;
    default:
      UNREACHABLE();
  }
}

void StoreDoubleField::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(object_input());
  UseRegister(value_input());
  set_temporaries_needed(1);
}
void StoreDoubleField::GenerateCode(MaglevAssembler* masm,
                                    const ProcessingState& state) {
  Register tmp = general_temporaries().PopFirst();
  Register object = ToRegister(object_input());
  DoubleRegister value = ToDoubleRegister(value_input());

  __ AssertNotSmi(object);
  __ DecompressAnyTagged(tmp, FieldOperand(object, offset()));
  __ AssertNotSmi(tmp);
  __ Movsd(FieldOperand(tmp, HeapNumber::kValueOffset), value);
}
void StoreDoubleField::PrintParams(std::ostream& os,
                                   MaglevGraphLabeller* graph_labeller) const {
  os << "(" << std::hex << offset() << std::dec << ")";
}
void StoreTaggedFieldNoWriteBarrier::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(object_input());
  UseRegister(value_input());
}
void StoreTaggedFieldNoWriteBarrier::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  Register object = ToRegister(object_input());
  Register value = ToRegister(value_input());

  __ AssertNotSmi(object);
  __ StoreTaggedField(FieldOperand(object, offset()), value);
}
void StoreTaggedFieldNoWriteBarrier::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << std::hex << offset() << std::dec << ")";
}

void StoreMap::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseFixed(object_input(), WriteBarrierDescriptor::ObjectRegister());
}
void StoreMap::GenerateCode(MaglevAssembler* masm,
                            const ProcessingState& state) {
  // TODO(leszeks): Consider making this an arbitrary register and push/popping
  // in the deferred path.
  Register object = WriteBarrierDescriptor::ObjectRegister();
  DCHECK_EQ(object, ToRegister(object_input()));

  __ AssertNotSmi(object);
  Register value = kScratchRegister;
  __ Move(value, map_.object());
  __ StoreTaggedField(FieldOperand(object, HeapObject::kMapOffset),
                      kScratchRegister);

  ZoneLabelRef done(masm);
  DeferredCodeInfo* deferred_write_barrier = __ PushDeferredCode(
      [](MaglevAssembler* masm, ZoneLabelRef done, Register value,
         Register object, StoreMap* node) {
        ASM_CODE_COMMENT_STRING(masm, "Write barrier slow path");
        __ CheckPageFlag(
            value, kScratchRegister,
            MemoryChunk::kPointersToHereAreInterestingOrInSharedHeapMask, zero,
            *done);

        Register slot_reg = WriteBarrierDescriptor::SlotAddressRegister();
        RegList saved;
        if (node->register_snapshot().live_registers.has(slot_reg)) {
          saved.set(slot_reg);
        }

        __ PushAll(saved);
        __ leaq(slot_reg, FieldOperand(object, HeapObject::kMapOffset));

        SaveFPRegsMode const save_fp_mode =
            !node->register_snapshot().live_double_registers.is_empty()
                ? SaveFPRegsMode::kSave
                : SaveFPRegsMode::kIgnore;

        __ CallRecordWriteStub(object, slot_reg, save_fp_mode);

        __ PopAll(saved);
        __ jmp(*done);
      },
      done, value, object, this);

  __ JumpIfSmi(value, *done);
  __ CheckPageFlag(object, kScratchRegister,
                   MemoryChunk::kPointersFromHereAreInterestingMask, not_zero,
                   &deferred_write_barrier->deferred_code_label);
  __ bind(*done);
}
void StoreMap::PrintParams(std::ostream& os,
                           MaglevGraphLabeller* graph_labeller) const {
  os << "(" << *map_.object() << ")";
}

void StoreTaggedFieldWithWriteBarrier::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseFixed(object_input(), WriteBarrierDescriptor::ObjectRegister());
  UseRegister(value_input());
}
void StoreTaggedFieldWithWriteBarrier::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  // TODO(leszeks): Consider making this an arbitrary register and push/popping
  // in the deferred path.
  Register object = WriteBarrierDescriptor::ObjectRegister();
  DCHECK_EQ(object, ToRegister(object_input()));

  Register value = ToRegister(value_input());

  __ AssertNotSmi(object);
  __ StoreTaggedField(FieldOperand(object, offset()), value);

  ZoneLabelRef done(masm);
  DeferredCodeInfo* deferred_write_barrier = __ PushDeferredCode(
      [](MaglevAssembler* masm, ZoneLabelRef done, Register value,
         Register object, StoreTaggedFieldWithWriteBarrier* node) {
        ASM_CODE_COMMENT_STRING(masm, "Write barrier slow path");
        __ CheckPageFlag(
            value, kScratchRegister,
            MemoryChunk::kPointersToHereAreInterestingOrInSharedHeapMask, zero,
            *done);

        Register slot_reg = WriteBarrierDescriptor::SlotAddressRegister();
        RegList saved;
        if (node->register_snapshot().live_registers.has(slot_reg)) {
          saved.set(slot_reg);
        }

        __ PushAll(saved);
        __ leaq(slot_reg, FieldOperand(object, node->offset()));

        SaveFPRegsMode const save_fp_mode =
            !node->register_snapshot().live_double_registers.is_empty()
                ? SaveFPRegsMode::kSave
                : SaveFPRegsMode::kIgnore;

        __ CallRecordWriteStub(object, slot_reg, save_fp_mode);

        __ PopAll(saved);
        __ jmp(*done);
      },
      done, value, object, this);

  __ JumpIfSmi(value, *done);
  __ CheckPageFlag(object, kScratchRegister,
                   MemoryChunk::kPointersFromHereAreInterestingMask, not_zero,
                   &deferred_write_barrier->deferred_code_label);
  __ bind(*done);
}
void StoreTaggedFieldWithWriteBarrier::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << std::hex << offset() << std::dec << ")";
}

void LoadNamedGeneric::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  using D = LoadWithVectorDescriptor;
  UseFixed(context(), kContextRegister);
  UseFixed(object_input(), D::GetRegisterParameter(D::kReceiver));
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void LoadNamedGeneric::GenerateCode(MaglevAssembler* masm,
                                    const ProcessingState& state) {
  using D = LoadWithVectorDescriptor;
  DCHECK_EQ(ToRegister(context()), kContextRegister);
  DCHECK_EQ(ToRegister(object_input()), D::GetRegisterParameter(D::kReceiver));
  __ Move(D::GetRegisterParameter(D::kName), name().object());
  __ Move(D::GetRegisterParameter(D::kSlot),
          Smi::FromInt(feedback().slot.ToInt()));
  __ Move(D::GetRegisterParameter(D::kVector), feedback().vector);
  __ CallBuiltin(Builtin::kLoadIC);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}
void LoadNamedGeneric::PrintParams(std::ostream& os,
                                   MaglevGraphLabeller* graph_labeller) const {
  os << "(" << name_ << ")";
}

void LoadNamedFromSuperGeneric::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  using D = LoadWithReceiverAndVectorDescriptor;
  UseFixed(context(), kContextRegister);
  UseFixed(receiver(), D::GetRegisterParameter(D::kReceiver));
  UseFixed(lookup_start_object(),
           D::GetRegisterParameter(D::kLookupStartObject));
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void LoadNamedFromSuperGeneric::GenerateCode(MaglevAssembler* masm,
                                             const ProcessingState& state) {
  using D = LoadWithReceiverAndVectorDescriptor;
  DCHECK_EQ(ToRegister(context()), kContextRegister);
  DCHECK_EQ(ToRegister(receiver()), D::GetRegisterParameter(D::kReceiver));
  DCHECK_EQ(ToRegister(lookup_start_object()),
            D::GetRegisterParameter(D::kLookupStartObject));
  __ Move(D::GetRegisterParameter(D::kName), name().object());
  __ Move(D::GetRegisterParameter(D::kSlot),
          Smi::FromInt(feedback().slot.ToInt()));
  __ Move(D::GetRegisterParameter(D::kVector), feedback().vector);
  __ CallBuiltin(Builtin::kLoadSuperIC);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}
void LoadNamedFromSuperGeneric::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << name_ << ")";
}

void SetNamedGeneric::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  using D = CallInterfaceDescriptorFor<Builtin::kStoreIC>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(object_input(), D::GetRegisterParameter(D::kReceiver));
  UseFixed(value_input(), D::GetRegisterParameter(D::kValue));
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void SetNamedGeneric::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  using D = CallInterfaceDescriptorFor<Builtin::kStoreIC>::type;
  DCHECK_EQ(ToRegister(context()), kContextRegister);
  DCHECK_EQ(ToRegister(object_input()), D::GetRegisterParameter(D::kReceiver));
  DCHECK_EQ(ToRegister(value_input()), D::GetRegisterParameter(D::kValue));
  __ Move(D::GetRegisterParameter(D::kName), name().object());
  __ Move(D::GetRegisterParameter(D::kSlot),
          TaggedIndex::FromIntptr(feedback().index()));
  __ Move(D::GetRegisterParameter(D::kVector), feedback().vector);
  __ CallBuiltin(Builtin::kStoreIC);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}
void SetNamedGeneric::PrintParams(std::ostream& os,
                                  MaglevGraphLabeller* graph_labeller) const {
  os << "(" << name_ << ")";
}

void StringLength::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(object_input());
  DefineAsRegister(vreg_state, this);
}
void StringLength::GenerateCode(MaglevAssembler* masm,
                                const ProcessingState& state) {
  Register object = ToRegister(object_input());
  if (v8_flags.debug_code) {
    // Use return register as temporary. Push it in case it aliases the object
    // register.
    Register tmp = ToRegister(result());
    __ Push(tmp);
    // Check if {object} is a string.
    __ AssertNotSmi(object);
    __ LoadMap(tmp, object);
    __ CmpInstanceTypeRange(tmp, tmp, FIRST_STRING_TYPE, LAST_STRING_TYPE);
    __ Check(below_equal, AbortReason::kUnexpectedValue);
    __ Pop(tmp);
  }
  __ movl(ToRegister(result()), FieldOperand(object, String::kLengthOffset));
}

void StringAt::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseAndClobberRegister(string_input());
  UseAndClobberRegister(index_input());
  DefineAsRegister(vreg_state, this);
  set_temporaries_needed(1);
}
void StringAt::GenerateCode(MaglevAssembler* masm,
                            const ProcessingState& state) {
  Register result_string = ToRegister(result());
  Register string = ToRegister(string_input());
  Register index = ToRegister(index_input());
  Register scratch = general_temporaries().PopFirst();
  Register char_code = string;

  ZoneLabelRef done(masm);
  Label cached_one_byte_string;

  RegisterSnapshot save_registers = register_snapshot();
  __ StringCharCodeAt(save_registers, char_code, string, index, scratch,
                      &cached_one_byte_string);
  __ StringFromCharCode(save_registers, &cached_one_byte_string, result_string,
                        char_code, scratch);
}

void DefineNamedOwnGeneric::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  using D = CallInterfaceDescriptorFor<Builtin::kDefineNamedOwnIC>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(object_input(), D::GetRegisterParameter(D::kReceiver));
  UseFixed(value_input(), D::GetRegisterParameter(D::kValue));
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void DefineNamedOwnGeneric::GenerateCode(MaglevAssembler* masm,
                                         const ProcessingState& state) {
  using D = CallInterfaceDescriptorFor<Builtin::kDefineNamedOwnIC>::type;
  DCHECK_EQ(ToRegister(context()), kContextRegister);
  DCHECK_EQ(ToRegister(object_input()), D::GetRegisterParameter(D::kReceiver));
  DCHECK_EQ(ToRegister(value_input()), D::GetRegisterParameter(D::kValue));
  __ Move(D::GetRegisterParameter(D::kName), name().object());
  __ Move(D::GetRegisterParameter(D::kSlot),
          TaggedIndex::FromIntptr(feedback().index()));
  __ Move(D::GetRegisterParameter(D::kVector), feedback().vector);
  __ CallBuiltin(Builtin::kDefineNamedOwnIC);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}
void DefineNamedOwnGeneric::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << name_ << ")";
}

void SetKeyedGeneric::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  using D = CallInterfaceDescriptorFor<Builtin::kKeyedStoreIC>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(object_input(), D::GetRegisterParameter(D::kReceiver));
  UseFixed(key_input(), D::GetRegisterParameter(D::kName));
  UseFixed(value_input(), D::GetRegisterParameter(D::kValue));
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void SetKeyedGeneric::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  using D = CallInterfaceDescriptorFor<Builtin::kKeyedStoreIC>::type;
  DCHECK_EQ(ToRegister(context()), kContextRegister);
  DCHECK_EQ(ToRegister(object_input()), D::GetRegisterParameter(D::kReceiver));
  DCHECK_EQ(ToRegister(key_input()), D::GetRegisterParameter(D::kName));
  DCHECK_EQ(ToRegister(value_input()), D::GetRegisterParameter(D::kValue));
  __ Move(D::GetRegisterParameter(D::kSlot),
          TaggedIndex::FromIntptr(feedback().index()));
  __ Move(D::GetRegisterParameter(D::kVector), feedback().vector);
  __ CallBuiltin(Builtin::kKeyedStoreIC);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void DefineKeyedOwnGeneric::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  using D = CallInterfaceDescriptorFor<Builtin::kKeyedStoreIC>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(object_input(), D::GetRegisterParameter(D::kReceiver));
  UseFixed(key_input(), D::GetRegisterParameter(D::kName));
  UseFixed(value_input(), D::GetRegisterParameter(D::kValue));
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void DefineKeyedOwnGeneric::GenerateCode(MaglevAssembler* masm,
                                         const ProcessingState& state) {
  using D = CallInterfaceDescriptorFor<Builtin::kDefineKeyedOwnIC>::type;
  DCHECK_EQ(ToRegister(context()), kContextRegister);
  DCHECK_EQ(ToRegister(object_input()), D::GetRegisterParameter(D::kReceiver));
  DCHECK_EQ(ToRegister(key_input()), D::GetRegisterParameter(D::kName));
  DCHECK_EQ(ToRegister(value_input()), D::GetRegisterParameter(D::kValue));
  __ Move(D::GetRegisterParameter(D::kSlot),
          TaggedIndex::FromIntptr(feedback().index()));
  __ Move(D::GetRegisterParameter(D::kVector), feedback().vector);
  __ CallBuiltin(Builtin::kDefineKeyedOwnIC);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void StoreInArrayLiteralGeneric::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  using D = CallInterfaceDescriptorFor<Builtin::kStoreInArrayLiteralIC>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(object_input(), D::GetRegisterParameter(D::kReceiver));
  UseFixed(name_input(), D::GetRegisterParameter(D::kName));
  UseFixed(value_input(), D::GetRegisterParameter(D::kValue));
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void StoreInArrayLiteralGeneric::GenerateCode(MaglevAssembler* masm,
                                              const ProcessingState& state) {
  using D = CallInterfaceDescriptorFor<Builtin::kStoreInArrayLiteralIC>::type;
  DCHECK_EQ(ToRegister(context()), kContextRegister);
  DCHECK_EQ(ToRegister(object_input()), D::GetRegisterParameter(D::kReceiver));
  DCHECK_EQ(ToRegister(value_input()), D::GetRegisterParameter(D::kValue));
  DCHECK_EQ(ToRegister(name_input()), D::GetRegisterParameter(D::kName));
  __ Move(D::GetRegisterParameter(D::kSlot),
          TaggedIndex::FromIntptr(feedback().index()));
  __ Move(D::GetRegisterParameter(D::kVector), feedback().vector);
  __ CallBuiltin(Builtin::kStoreInArrayLiteralIC);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void GetKeyedGeneric::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  using D = CallInterfaceDescriptorFor<Builtin::kKeyedLoadIC>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(object_input(), D::GetRegisterParameter(D::kReceiver));
  UseFixed(key_input(), D::GetRegisterParameter(D::kName));
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void GetKeyedGeneric::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  using D = CallInterfaceDescriptorFor<Builtin::kKeyedLoadIC>::type;
  DCHECK_EQ(ToRegister(context()), kContextRegister);
  DCHECK_EQ(ToRegister(object_input()), D::GetRegisterParameter(D::kReceiver));
  DCHECK_EQ(ToRegister(key_input()), D::GetRegisterParameter(D::kName));
  __ Move(D::GetRegisterParameter(D::kSlot),
          TaggedIndex::FromIntptr(feedback().slot.ToInt()));
  __ Move(D::GetRegisterParameter(D::kVector), feedback().vector);
  __ CallBuiltin(Builtin::kKeyedLoadIC);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void GapMove::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UNREACHABLE();
}
void GapMove::GenerateCode(MaglevAssembler* masm,
                           const ProcessingState& state) {
  DCHECK_EQ(source().representation(), target().representation());
  MachineRepresentation repr = source().representation();
  if (source().IsRegister()) {
    Register source_reg = ToRegister(source());
    if (target().IsAnyRegister()) {
      DCHECK(target().IsRegister());
      __ MoveRepr(repr, ToRegister(target()), source_reg);
    } else {
      __ MoveRepr(repr, masm->ToMemOperand(target()), source_reg);
    }
  } else if (source().IsDoubleRegister()) {
    DoubleRegister source_reg = ToDoubleRegister(source());
    if (target().IsAnyRegister()) {
      DCHECK(target().IsDoubleRegister());
      __ Movsd(ToDoubleRegister(target()), source_reg);
    } else {
      __ Movsd(masm->ToMemOperand(target()), source_reg);
    }
  } else {
    DCHECK(source().IsAnyStackSlot());
    MemOperand source_op = masm->ToMemOperand(source());
    if (target().IsRegister()) {
      __ MoveRepr(repr, ToRegister(target()), source_op);
    } else if (target().IsDoubleRegister()) {
      __ Movsd(ToDoubleRegister(target()), source_op);
    } else {
      DCHECK(target().IsAnyStackSlot());
      __ MoveRepr(repr, kScratchRegister, source_op);
      __ MoveRepr(repr, masm->ToMemOperand(target()), kScratchRegister);
    }
  }
}
void GapMove::PrintParams(std::ostream& os,
                          MaglevGraphLabeller* graph_labeller) const {
  os << "(" << source() << " → " << target() << ")";
}
void ConstantGapMove::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UNREACHABLE();
}

namespace {
template <typename T>
struct GetRegister;
template <>
struct GetRegister<Register> {
  static Register Get(compiler::AllocatedOperand target) {
    return target.GetRegister();
  }
};
template <>
struct GetRegister<DoubleRegister> {
  static DoubleRegister Get(compiler::AllocatedOperand target) {
    return target.GetDoubleRegister();
  }
};
}  // namespace
void ConstantGapMove::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  switch (node_->opcode()) {
#define CASE(Name)                                \
  case Opcode::k##Name:                           \
    return node_->Cast<Name>()->DoLoadToRegister( \
        masm, GetRegister<Name::OutputRegister>::Get(target()));
    CONSTANT_VALUE_NODE_LIST(CASE)
#undef CASE
    default:
      UNREACHABLE();
  }
}
void ConstantGapMove::PrintParams(std::ostream& os,
                                  MaglevGraphLabeller* graph_labeller) const {
  os << "(";
  graph_labeller->PrintNodeLabel(os, node_);
  os << " → " << target() << ")";
}

namespace {

constexpr Builtin BuiltinFor(Operation operation) {
  switch (operation) {
#define CASE(name)         \
  case Operation::k##name: \
    return Builtin::k##name##_WithFeedback;
    OPERATION_LIST(CASE)
#undef CASE
  }
}

}  // namespace

template <class Derived, Operation kOperation>
void UnaryWithFeedbackNode<Derived, kOperation>::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  using D = UnaryOp_WithFeedbackDescriptor;
  UseFixed(operand_input(), D::GetRegisterParameter(D::kValue));
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}

template <class Derived, Operation kOperation>
void UnaryWithFeedbackNode<Derived, kOperation>::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  using D = UnaryOp_WithFeedbackDescriptor;
  DCHECK_EQ(ToRegister(operand_input()), D::GetRegisterParameter(D::kValue));
  __ Move(kContextRegister, masm->native_context().object());
  __ Move(D::GetRegisterParameter(D::kSlot), Immediate(feedback().index()));
  __ Move(D::GetRegisterParameter(D::kFeedbackVector), feedback().vector);
  __ CallBuiltin(BuiltinFor(kOperation));
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

template <class Derived, Operation kOperation>
void BinaryWithFeedbackNode<Derived, kOperation>::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  using D = BinaryOp_WithFeedbackDescriptor;
  UseFixed(left_input(), D::GetRegisterParameter(D::kLeft));
  UseFixed(right_input(), D::GetRegisterParameter(D::kRight));
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}

template <class Derived, Operation kOperation>
void BinaryWithFeedbackNode<Derived, kOperation>::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  using D = BinaryOp_WithFeedbackDescriptor;
  DCHECK_EQ(ToRegister(left_input()), D::GetRegisterParameter(D::kLeft));
  DCHECK_EQ(ToRegister(right_input()), D::GetRegisterParameter(D::kRight));
  __ Move(kContextRegister, masm->native_context().object());
  __ Move(D::GetRegisterParameter(D::kSlot), Immediate(feedback().index()));
  __ Move(D::GetRegisterParameter(D::kFeedbackVector), feedback().vector);
  __ CallBuiltin(BuiltinFor(kOperation));
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

#define DEF_OPERATION(Name)                                        \
  void Name::AllocateVreg(MaglevVregAllocationState* vreg_state) { \
    Base::AllocateVreg(vreg_state);                                \
  }                                                                \
  void Name::GenerateCode(MaglevAssembler* masm,                   \
                          const ProcessingState& state) {          \
    Base::GenerateCode(masm, state);                               \
  }
GENERIC_OPERATIONS_NODE_LIST(DEF_OPERATION)
#undef DEF_OPERATION

void Int32AddWithOverflow::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(left_input());
  UseRegister(right_input());
  DefineSameAsFirst(vreg_state, this);
}

void Int32AddWithOverflow::GenerateCode(MaglevAssembler* masm,
                                        const ProcessingState& state) {
  Register left = ToRegister(left_input());
  Register right = ToRegister(right_input());
  __ addl(left, right);
  // None of the mutated input registers should be a register input into the
  // eager deopt info.
  DCHECK_REGLIST_EMPTY(RegList{left} &
                       GetGeneralRegistersUsedAsInputs(eager_deopt_info()));
  __ EmitEagerDeoptIf(overflow, DeoptimizeReason::kOverflow, this);
}

void Int32SubtractWithOverflow::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(left_input());
  UseRegister(right_input());
  DefineSameAsFirst(vreg_state, this);
}

void Int32SubtractWithOverflow::GenerateCode(MaglevAssembler* masm,
                                             const ProcessingState& state) {
  Register left = ToRegister(left_input());
  Register right = ToRegister(right_input());
  __ subl(left, right);
  // None of the mutated input registers should be a register input into the
  // eager deopt info.
  DCHECK_REGLIST_EMPTY(RegList{left} &
                       GetGeneralRegistersUsedAsInputs(eager_deopt_info()));
  __ EmitEagerDeoptIf(overflow, DeoptimizeReason::kOverflow, this);
}

void Int32MultiplyWithOverflow::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(left_input());
  UseRegister(right_input());
  DefineSameAsFirst(vreg_state, this);
  set_temporaries_needed(1);
}

void Int32MultiplyWithOverflow::GenerateCode(MaglevAssembler* masm,
                                             const ProcessingState& state) {
  Register result = ToRegister(this->result());
  Register right = ToRegister(right_input());
  DCHECK_EQ(result, ToRegister(left_input()));

  Register saved_left = general_temporaries().first();
  __ movl(saved_left, result);
  // TODO(leszeks): peephole optimise multiplication by a constant.
  __ imull(result, right);
  // None of the mutated input registers should be a register input into the
  // eager deopt info.
  DCHECK_REGLIST_EMPTY(RegList{saved_left, result} &
                       GetGeneralRegistersUsedAsInputs(eager_deopt_info()));
  __ EmitEagerDeoptIf(overflow, DeoptimizeReason::kOverflow, this);

  // If the result is zero, check if either lhs or rhs is negative.
  Label end;
  __ cmpl(result, Immediate(0));
  __ j(not_zero, &end);
  {
    __ orl(saved_left, right);
    __ cmpl(saved_left, Immediate(0));
    // If one of them is negative, we must have a -0 result, which is non-int32,
    // so deopt.
    // TODO(leszeks): Consider splitting these deopts to have distinct deopt
    // reasons. Otherwise, the reason has to match the above.
    __ EmitEagerDeoptIf(less, DeoptimizeReason::kOverflow, this);
  }
  __ bind(&end);
}

void Int32ModulusWithOverflow::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(left_input());
  UseRegister(right_input());
  DefineAsFixed(vreg_state, this, rdx);
  // rax,rdx are clobbered by div.
  RequireSpecificTemporary(rax);
  RequireSpecificTemporary(rdx);
}

void Int32ModulusWithOverflow::GenerateCode(MaglevAssembler* masm,
                                            const ProcessingState& state) {
  // Using same algorithm as in EffectControlLinearizer:
  //   if rhs <= 0 then
  //     rhs = -rhs
  //     deopt if rhs == 0
  //   if lhs < 0 then
  //     let lhs_abs = -lsh in
  //     let res = lhs_abs % rhs in
  //     deopt if res == 0
  //     -res
  //   else
  //     let msk = rhs - 1 in
  //     if rhs & msk == 0 then
  //       lhs & msk
  //     else
  //       lhs % rhs

  DCHECK(general_temporaries().has(rax));
  DCHECK(general_temporaries().has(rdx));
  Register left = ToRegister(left_input());
  Register right = ToRegister(right_input());

  ZoneLabelRef done(masm);
  ZoneLabelRef rhs_checked(masm);

  __ cmpl(right, Immediate(0));
  __ JumpToDeferredIf(
      less_equal,
      [](MaglevAssembler* masm, ZoneLabelRef rhs_checked, Register right,
         Int32ModulusWithOverflow* node) {
        __ negl(right);
        __ testl(right, right);
        __ EmitEagerDeoptIf(equal, DeoptimizeReason::kDivisionByZero, node);
        __ jmp(*rhs_checked);
      },
      rhs_checked, right, this);
  __ bind(*rhs_checked);

  __ cmpl(left, Immediate(0));
  __ JumpToDeferredIf(
      less,
      [](MaglevAssembler* masm, ZoneLabelRef done, Register left,
         Register right, Int32ModulusWithOverflow* node) {
        __ negl(left);
        __ movl(rax, left);
        __ xorl(rdx, rdx);
        __ divl(right);
        __ testl(rdx, rdx);
        // TODO(victorgomes): This ideally should be kMinusZero, but Maglev only
        // allows one deopt reason per IR.
        __ EmitEagerDeoptIf(equal, DeoptimizeReason::kDivisionByZero, node);
        __ negl(rdx);
        __ jmp(*done);
      },
      done, left, right, this);

  Label right_not_power_of_2;
  Register mask = rax;
  __ leal(mask, Operand(right, -1));
  __ testl(right, mask);
  __ j(not_zero, &right_not_power_of_2, Label::kNear);

  // {right} is power of 2.
  __ andl(mask, left);
  __ movl(ToRegister(result()), mask);
  __ jmp(*done, Label::kNear);

  __ bind(&right_not_power_of_2);
  __ movl(rax, left);
  __ xorl(rdx, rdx);
  __ divl(right);
  // Result is implicitly written to rdx.
  DCHECK_EQ(ToRegister(result()), rdx);

  __ bind(*done);
}

void Int32DivideWithOverflow::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(left_input());
  UseRegister(right_input());
  DefineAsFixed(vreg_state, this, rax);
  // rax,rdx are clobbered by idiv.
  RequireSpecificTemporary(rax);
  RequireSpecificTemporary(rdx);
}

void Int32DivideWithOverflow::GenerateCode(MaglevAssembler* masm,
                                           const ProcessingState& state) {
  DCHECK(general_temporaries().has(rax));
  DCHECK(general_temporaries().has(rdx));
  Register left = ToRegister(left_input());
  Register right = ToRegister(right_input());
  __ movl(rax, left);

  // TODO(leszeks): peephole optimise division by a constant.

  // Sign extend eax into edx.
  __ cdq();

  // Pre-check for overflow, since idiv throws a division exception on overflow
  // rather than setting the overflow flag. Logic copied from
  // effect-control-linearizer.cc

  // Check if {right} is positive (and not zero).
  __ cmpl(right, Immediate(0));
  ZoneLabelRef done(masm);
  __ JumpToDeferredIf(
      less_equal,
      [](MaglevAssembler* masm, ZoneLabelRef done, Register right,
         Int32DivideWithOverflow* node) {
        // {right} is negative or zero.

        // Check if {right} is zero.
        // We've already done the compare and flags won't be cleared yet.
        // TODO(leszeks): Using kNotInt32 here, but kDivisionByZero would be
        // better. Right now all eager deopts in a node have to be the same --
        // we should allow a node to emit multiple eager deopts with different
        // reasons.
        __ EmitEagerDeoptIf(equal, DeoptimizeReason::kNotInt32, node);

        // Check if {left} is zero, as that would produce minus zero. Left is in
        // rax already.
        __ cmpl(rax, Immediate(0));
        // TODO(leszeks): Better DeoptimizeReason = kMinusZero.
        __ EmitEagerDeoptIf(equal, DeoptimizeReason::kNotInt32, node);

        // Check if {left} is kMinInt and {right} is -1, in which case we'd have
        // to return -kMinInt, which is not representable as Int32.
        __ cmpl(rax, Immediate(kMinInt));
        __ j(not_equal, *done);
        __ cmpl(right, Immediate(-1));
        __ j(not_equal, *done);
        // TODO(leszeks): Better DeoptimizeReason = kOverflow, but
        // eager_deopt_info is already configured as kNotInt32.
        __ EmitEagerDeopt(node, DeoptimizeReason::kNotInt32);
      },
      done, right, this);
  __ bind(*done);

  // Perform the actual integer division.
  __ idivl(right);

  // Check that the remainder is zero.
  __ cmpl(rdx, Immediate(0));
  // None of the mutated input registers should be a register input into the
  // eager deopt info.
  DCHECK_REGLIST_EMPTY(RegList{rax, rdx} &
                       GetGeneralRegistersUsedAsInputs(eager_deopt_info()));
  __ EmitEagerDeoptIf(not_equal, DeoptimizeReason::kNotInt32, this);
  DCHECK_EQ(ToRegister(result()), rax);
}

void Int32BitwiseAnd::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(left_input());
  UseRegister(right_input());
  DefineSameAsFirst(vreg_state, this);
}

void Int32BitwiseAnd::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  Register left = ToRegister(left_input());
  Register right = ToRegister(right_input());
  __ andl(left, right);
}

void Int32BitwiseOr::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(left_input());
  UseRegister(right_input());
  DefineSameAsFirst(vreg_state, this);
}

void Int32BitwiseOr::GenerateCode(MaglevAssembler* masm,
                                  const ProcessingState& state) {
  Register left = ToRegister(left_input());
  Register right = ToRegister(right_input());
  __ orl(left, right);
}

void Int32BitwiseXor::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(left_input());
  UseRegister(right_input());
  DefineSameAsFirst(vreg_state, this);
}

void Int32BitwiseXor::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  Register left = ToRegister(left_input());
  Register right = ToRegister(right_input());
  __ xorl(left, right);
}

void Int32ShiftLeft::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(left_input());
  // Use the "shift by cl" variant of shl.
  // TODO(leszeks): peephole optimise shifts by a constant.
  UseFixed(right_input(), rcx);
  DefineSameAsFirst(vreg_state, this);
}

void Int32ShiftLeft::GenerateCode(MaglevAssembler* masm,
                                  const ProcessingState& state) {
  Register left = ToRegister(left_input());
  DCHECK_EQ(rcx, ToRegister(right_input()));
  __ shll_cl(left);
}

void Int32ShiftRight::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(left_input());
  // Use the "shift by cl" variant of sar.
  // TODO(leszeks): peephole optimise shifts by a constant.
  UseFixed(right_input(), rcx);
  DefineSameAsFirst(vreg_state, this);
}

void Int32ShiftRight::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  Register left = ToRegister(left_input());
  DCHECK_EQ(rcx, ToRegister(right_input()));
  __ sarl_cl(left);
}

void Int32ShiftRightLogical::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(left_input());
  // Use the "shift by cl" variant of shr.
  // TODO(leszeks): peephole optimise shifts by a constant.
  UseFixed(right_input(), rcx);
  DefineSameAsFirst(vreg_state, this);
}

void Int32ShiftRightLogical::GenerateCode(MaglevAssembler* masm,
                                          const ProcessingState& state) {
  Register left = ToRegister(left_input());
  DCHECK_EQ(rcx, ToRegister(right_input()));
  __ shrl_cl(left);
}

namespace {

constexpr Condition ConditionFor(Operation operation) {
  switch (operation) {
    case Operation::kEqual:
    case Operation::kStrictEqual:
      return equal;
    case Operation::kLessThan:
      return less;
    case Operation::kLessThanOrEqual:
      return less_equal;
    case Operation::kGreaterThan:
      return greater;
    case Operation::kGreaterThanOrEqual:
      return greater_equal;
    default:
      UNREACHABLE();
  }
}

}  // namespace

template <class Derived, Operation kOperation>
void Int32CompareNode<Derived, kOperation>::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(left_input());
  UseRegister(right_input());
  DefineAsRegister(vreg_state, this);
}

template <class Derived, Operation kOperation>
void Int32CompareNode<Derived, kOperation>::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  Register left = ToRegister(left_input());
  Register right = ToRegister(right_input());
  Register result = ToRegister(this->result());
  Label is_true, end;
  __ cmpl(left, right);
  // TODO(leszeks): Investigate using cmov here.
  __ j(ConditionFor(kOperation), &is_true);
  // TODO(leszeks): Investigate loading existing materialisations of roots here,
  // if available.
  __ LoadRoot(result, RootIndex::kFalseValue);
  __ jmp(&end);
  {
    __ bind(&is_true);
    __ LoadRoot(result, RootIndex::kTrueValue);
  }
  __ bind(&end);
}

#define DEF_OPERATION(Name)                                        \
  void Name::AllocateVreg(MaglevVregAllocationState* vreg_state) { \
    Base::AllocateVreg(vreg_state);                                \
  }                                                                \
  void Name::GenerateCode(MaglevAssembler* masm,                   \
                          const ProcessingState& state) {          \
    Base::GenerateCode(masm, state);                               \
  }
DEF_OPERATION(Int32Equal)
DEF_OPERATION(Int32StrictEqual)
DEF_OPERATION(Int32LessThan)
DEF_OPERATION(Int32LessThanOrEqual)
DEF_OPERATION(Int32GreaterThan)
DEF_OPERATION(Int32GreaterThanOrEqual)
#undef DEF_OPERATION

void Float64Add::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(left_input());
  UseRegister(right_input());
  DefineSameAsFirst(vreg_state, this);
}

void Float64Add::GenerateCode(MaglevAssembler* masm,
                              const ProcessingState& state) {
  DoubleRegister left = ToDoubleRegister(left_input());
  DoubleRegister right = ToDoubleRegister(right_input());
  __ Addsd(left, right);
}

void Float64Subtract::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(left_input());
  UseRegister(right_input());
  DefineSameAsFirst(vreg_state, this);
}

void Float64Subtract::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  DoubleRegister left = ToDoubleRegister(left_input());
  DoubleRegister right = ToDoubleRegister(right_input());
  __ Subsd(left, right);
}

void Float64Multiply::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(left_input());
  UseRegister(right_input());
  DefineSameAsFirst(vreg_state, this);
}

void Float64Multiply::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  DoubleRegister left = ToDoubleRegister(left_input());
  DoubleRegister right = ToDoubleRegister(right_input());
  __ Mulsd(left, right);
}

void Float64Divide::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(left_input());
  UseRegister(right_input());
  DefineSameAsFirst(vreg_state, this);
}

void Float64Divide::GenerateCode(MaglevAssembler* masm,
                                 const ProcessingState& state) {
  DoubleRegister left = ToDoubleRegister(left_input());
  DoubleRegister right = ToDoubleRegister(right_input());
  __ Divsd(left, right);
}

template <class Derived, Operation kOperation>
void Float64CompareNode<Derived, kOperation>::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(left_input());
  UseRegister(right_input());
  DefineAsRegister(vreg_state, this);
}

template <class Derived, Operation kOperation>
void Float64CompareNode<Derived, kOperation>::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  DoubleRegister left = ToDoubleRegister(left_input());
  DoubleRegister right = ToDoubleRegister(right_input());
  Register result = ToRegister(this->result());
  Label is_true, end;
  __ Ucomisd(left, right);
  // TODO(leszeks): Investigate using cmov here.
  __ j(ConditionFor(kOperation), &is_true);
  // TODO(leszeks): Investigate loading existing materialisations of roots here,
  // if available.
  __ LoadRoot(result, RootIndex::kFalseValue);
  __ jmp(&end);
  {
    __ bind(&is_true);
    __ LoadRoot(result, RootIndex::kTrueValue);
  }
  __ bind(&end);
}

#define DEF_OPERATION(Name)                                        \
  void Name::AllocateVreg(MaglevVregAllocationState* vreg_state) { \
    Base::AllocateVreg(vreg_state);                                \
  }                                                                \
  void Name::GenerateCode(MaglevAssembler* masm,                   \
                          const ProcessingState& state) {          \
    Base::GenerateCode(masm, state);                               \
  }
DEF_OPERATION(Float64Equal)
DEF_OPERATION(Float64StrictEqual)
DEF_OPERATION(Float64LessThan)
DEF_OPERATION(Float64LessThanOrEqual)
DEF_OPERATION(Float64GreaterThan)
DEF_OPERATION(Float64GreaterThanOrEqual)
#undef DEF_OPERATION

void CheckedSmiUntag::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(input());
  DefineSameAsFirst(vreg_state, this);
}

void CheckedSmiUntag::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  Register value = ToRegister(input());
  // TODO(leszeks): Consider optimizing away this test and using the carry bit
  // of the `sarl` for cases where the deopt uses the value from a different
  // register.
  Condition is_smi = __ CheckSmi(value);
  __ EmitEagerDeoptIf(NegateCondition(is_smi), DeoptimizeReason::kNotASmi,
                      this);
  __ SmiToInt32(value);
}

void UnsafeSmiUntag::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(input());
  DefineSameAsFirst(vreg_state, this);
}

void UnsafeSmiUntag::GenerateCode(MaglevAssembler* masm,
                                  const ProcessingState& state) {
  Register value = ToRegister(input());
  __ AssertSmi(value);
  __ SmiToInt32(value);
}

void CheckInt32IsSmi::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(input());
}
void CheckInt32IsSmi::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  // TODO(leszeks): This basically does a SmiTag and throws the result away.
  // Don't throw the result away if we want to actually use it.
  Register reg = ToRegister(input());
  __ movl(kScratchRegister, reg);
  __ addl(kScratchRegister, kScratchRegister);
  DCHECK_REGLIST_EMPTY(RegList{reg} &
                       GetGeneralRegistersUsedAsInputs(eager_deopt_info()));
  __ EmitEagerDeoptIf(overflow, DeoptimizeReason::kNotASmi, this);
}

void CheckUint32IsSmi::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(input());
}
void CheckUint32IsSmi::GenerateCode(MaglevAssembler* masm,
                                    const ProcessingState& state) {
  Register reg = ToRegister(input());
  // Perform an unsigned comparison against Smi::kMaxValue.
  __ cmpl(reg, Immediate(Smi::kMaxValue));
  __ EmitEagerDeoptIf(above, DeoptimizeReason::kNotASmi, this);
}

void CheckedSmiTagInt32::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseAndClobberRegister(input());
  DefineSameAsFirst(vreg_state, this);
}
void CheckedSmiTagInt32::GenerateCode(MaglevAssembler* masm,
                                      const ProcessingState& state) {
  Register reg = ToRegister(input());
  __ addl(reg, reg);
  // None of the mutated input registers should be a register input into the
  // eager deopt info.
  DCHECK_REGLIST_EMPTY(RegList{reg} &
                       GetGeneralRegistersUsedAsInputs(eager_deopt_info()));
  __ EmitEagerDeoptIf(overflow, DeoptimizeReason::kOverflow, this);
}

void CheckedSmiTagUint32::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(input());
  DefineSameAsFirst(vreg_state, this);
}
void CheckedSmiTagUint32::GenerateCode(MaglevAssembler* masm,
                                       const ProcessingState& state) {
  Register reg = ToRegister(input());
  // Perform an unsigned comparison against Smi::kMaxValue.
  __ cmpl(reg, Immediate(Smi::kMaxValue));
  __ EmitEagerDeoptIf(above, DeoptimizeReason::kOverflow, this);
  __ addl(reg, reg);
  __ Assert(no_overflow, AbortReason::kInputDoesNotFitSmi);
}

void UnsafeSmiTag::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(input());
  DefineSameAsFirst(vreg_state, this);
}
void UnsafeSmiTag::GenerateCode(MaglevAssembler* masm,
                                const ProcessingState& state) {
  Register reg = ToRegister(input());
  if (v8_flags.debug_code) {
    if (input().node()->properties().value_representation() ==
        ValueRepresentation::kUint32) {
      __ cmpl(reg, Immediate(Smi::kMaxValue));
      __ Check(below_equal, AbortReason::kInputDoesNotFitSmi);
    }
  }
  __ addl(reg, reg);
  if (v8_flags.debug_code) {
    __ Check(no_overflow, AbortReason::kInputDoesNotFitSmi);
  }
}

void Int32Constant::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  DefineAsConstant(vreg_state, this);
}
void Int32Constant::GenerateCode(MaglevAssembler* masm,
                                 const ProcessingState& state) {}
void Int32Constant::DoLoadToRegister(MaglevAssembler* masm, Register reg) {
  __ Move(reg, Immediate(value()));
}
Handle<Object> Int32Constant::DoReify(LocalIsolate* isolate) {
  return isolate->factory()->NewNumber<AllocationType::kOld>(value());
}
void Int32Constant::PrintParams(std::ostream& os,
                                MaglevGraphLabeller* graph_labeller) const {
  os << "(" << value() << ")";
}

void Int32ToNumber::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(input());
  DefineAsRegister(vreg_state, this);
}
void Int32ToNumber::GenerateCode(MaglevAssembler* masm,
                                 const ProcessingState& state) {
  ZoneLabelRef done(masm);
  Register value = ToRegister(input());
  Register object = ToRegister(result());
  __ movl(kScratchRegister, value);
  __ addl(kScratchRegister, kScratchRegister);
  __ JumpToDeferredIf(
      overflow,
      [](MaglevAssembler* masm, Register object, Register value,
         ZoneLabelRef done, Int32ToNumber* node) {
        DoubleRegister double_value = kScratchDoubleReg;
        __ Cvtlsi2sd(double_value, value);
        __ AllocateHeapNumber(node->register_snapshot(), object, double_value);
        __ jmp(*done);
      },
      object, value, done, this);
  __ Move(object, kScratchRegister);
  __ bind(*done);
}

void Uint32ToNumber::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(input());
  DefineSameAsFirst(vreg_state, this);
}
void Uint32ToNumber::GenerateCode(MaglevAssembler* masm,
                                  const ProcessingState& state) {
  ZoneLabelRef done(masm);
  Register value = ToRegister(input());
  Register object = ToRegister(result());
  __ cmpl(value, Immediate(Smi::kMaxValue));
  __ JumpToDeferredIf(
      above,
      [](MaglevAssembler* masm, Register object, Register value,
         ZoneLabelRef done, Uint32ToNumber* node) {
        DoubleRegister double_value = kScratchDoubleReg;
        __ Cvtlui2sd(double_value, value);
        __ AllocateHeapNumber(node->register_snapshot(), object, double_value);
        __ jmp(*done);
      },
      object, value, done, this);
  __ addl(value, value);
  DCHECK_EQ(object, value);
  __ bind(*done);
}

void Float64Box::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(input());
  DefineAsRegister(vreg_state, this);
}
void Float64Box::GenerateCode(MaglevAssembler* masm,
                              const ProcessingState& state) {
  DoubleRegister value = ToDoubleRegister(input());
  Register object = ToRegister(result());
  __ AllocateHeapNumber(register_snapshot(), object, value);
}

void HoleyFloat64Box::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(input());
  DefineAsRegister(vreg_state, this);
}
void HoleyFloat64Box::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  ZoneLabelRef done(masm);
  DoubleRegister value = ToDoubleRegister(input());
  Register object = ToRegister(result());
  __ movq(object, value);
  __ Move(kScratchRegister, kHoleNanInt64);
  __ cmpq(object, kScratchRegister);
  __ JumpToDeferredIf(
      equal,
      [](MaglevAssembler* masm, Register object, ZoneLabelRef done) {
        __ LoadRoot(object, RootIndex::kUndefinedValue);
        __ jmp(*done);
      },
      object, done);
  __ AllocateHeapNumber(register_snapshot(), object, value);
  __ bind(*done);
}

void CheckedFloat64Unbox::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(input());
  DefineAsRegister(vreg_state, this);
}
void CheckedFloat64Unbox::GenerateCode(MaglevAssembler* masm,
                                       const ProcessingState& state) {
  Register value = ToRegister(input());
  Label is_not_smi, done;
  // Check if Smi.
  __ JumpIfNotSmi(value, &is_not_smi, Label::kNear);
  // If Smi, convert to Float64.
  __ SmiToInt32(value);
  __ Cvtlsi2sd(ToDoubleRegister(result()), value);
  // TODO(v8:7700): Add a constraint to the register allocator to indicate that
  // the value in the input register is "trashed" by this node. Currently we
  // have the invariant that the input register should not be mutated when it is
  // not the same as the output register or the function does not call a
  // builtin. So, we recover the Smi value here.
  __ SmiTag(value);
  __ jmp(&done, Label::kNear);
  __ bind(&is_not_smi);
  // Check if HeapNumber, deopt otherwise.
  __ CompareRoot(FieldOperand(value, HeapObject::kMapOffset),
                 RootIndex::kHeapNumberMap);
  __ EmitEagerDeoptIf(not_equal, DeoptimizeReason::kNotANumber, this);
  __ Movsd(ToDoubleRegister(result()),
           FieldOperand(value, HeapNumber::kValueOffset));
  __ bind(&done);
}

void CheckedTruncateNumberToInt32::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(input());
  DefineSameAsFirst(vreg_state, this);
}
void CheckedTruncateNumberToInt32::GenerateCode(MaglevAssembler* masm,
                                                const ProcessingState& state) {
  Register value = ToRegister(input());
  Register result_reg = ToRegister(result());
  DCHECK_EQ(value, result_reg);
  Label is_not_smi, done;
  // Check if Smi.
  __ JumpIfNotSmi(value, &is_not_smi, Label::kNear);
  // If Smi, convert to Int32.
  __ SmiToInt32(value);
  __ jmp(&done, Label::kNear);
  __ bind(&is_not_smi);
  // Check if HeapNumber, deopt otherwise.
  __ CompareRoot(FieldOperand(value, HeapObject::kMapOffset),
                 RootIndex::kHeapNumberMap);
  __ EmitEagerDeoptIf(not_equal, DeoptimizeReason::kNotANumber, this);
  auto double_value = kScratchDoubleReg;
  __ Movsd(double_value, FieldOperand(value, HeapNumber::kValueOffset));
  __ TruncateDoubleToInt32(result_reg, double_value);
  __ bind(&done);
}

void LogicalNot::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(value());
  DefineAsRegister(vreg_state, this);
}
void LogicalNot::GenerateCode(MaglevAssembler* masm,
                              const ProcessingState& state) {
  Register object = ToRegister(value());
  Register return_value = ToRegister(result());

  if (v8_flags.debug_code) {
    // LogicalNot expects either TrueValue or FalseValue.
    Label next;
    __ CompareRoot(object, RootIndex::kFalseValue);
    __ j(equal, &next);
    __ CompareRoot(object, RootIndex::kTrueValue);
    __ Check(equal, AbortReason::kUnexpectedValue);
    __ bind(&next);
  }

  Label return_false, done;
  __ CompareRoot(object, RootIndex::kTrueValue);
  __ j(equal, &return_false, Label::kNear);
  __ LoadRoot(return_value, RootIndex::kTrueValue);
  __ jmp(&done, Label::kNear);

  __ bind(&return_false);
  __ LoadRoot(return_value, RootIndex::kFalseValue);

  __ bind(&done);
}

void SetPendingMessage::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(value());
  set_temporaries_needed(1);
  DefineAsRegister(vreg_state, this);
}

void SetPendingMessage::GenerateCode(MaglevAssembler* masm,
                                     const ProcessingState& state) {
  Register new_message = ToRegister(value());
  Register return_value = ToRegister(result());

  MemOperand pending_message_operand = __ ExternalReferenceAsOperand(
      ExternalReference::address_of_pending_message(masm->isolate()),
      kScratchRegister);

  if (new_message != return_value) {
    __ Move(return_value, pending_message_operand);
    __ movq(pending_message_operand, new_message);
  } else {
    Register scratch = general_temporaries().PopFirst();
    __ Move(scratch, pending_message_operand);
    __ movq(pending_message_operand, new_message);
    __ Move(return_value, scratch);
  }
}

void ToBoolean::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(value());
  DefineAsRegister(vreg_state, this);
}
void ToBoolean::GenerateCode(MaglevAssembler* masm,
                             const ProcessingState& state) {
  Register object = ToRegister(value());
  Register return_value = ToRegister(result());
  Label done;
  ZoneLabelRef object_is_true(masm), object_is_false(masm);
  // TODO(leszeks): We're likely to be calling this on an existing boolean --
  // maybe that's a case we should fast-path here and re-use that boolean value?
  __ ToBoolean(object, object_is_true, object_is_false, true);
  __ bind(*object_is_true);
  __ LoadRoot(return_value, RootIndex::kTrueValue);
  __ jmp(&done, Label::kNear);
  __ bind(*object_is_false);
  __ LoadRoot(return_value, RootIndex::kFalseValue);
  __ bind(&done);
}

void ToBooleanLogicalNot::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(value());
  DefineAsRegister(vreg_state, this);
}
void ToBooleanLogicalNot::GenerateCode(MaglevAssembler* masm,
                                       const ProcessingState& state) {
  Register object = ToRegister(value());
  Register return_value = ToRegister(result());
  Label done;
  ZoneLabelRef object_is_true(masm), object_is_false(masm);
  __ ToBoolean(object, object_is_true, object_is_false, true);
  __ bind(*object_is_true);
  __ LoadRoot(return_value, RootIndex::kFalseValue);
  __ jmp(&done, Label::kNear);
  __ bind(*object_is_false);
  __ LoadRoot(return_value, RootIndex::kTrueValue);
  __ bind(&done);
}

void TaggedEqual::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(lhs());
  UseRegister(rhs());
  DefineAsRegister(vreg_state, this);
}
void TaggedEqual::GenerateCode(MaglevAssembler* masm,
                               const ProcessingState& state) {
  Label done, if_equal;
  __ cmp_tagged(ToRegister(lhs()), ToRegister(rhs()));
  __ j(equal, &if_equal, Label::kNear);
  __ LoadRoot(ToRegister(result()), RootIndex::kFalseValue);
  __ jmp(&done, Label::kNear);
  __ bind(&if_equal);
  __ LoadRoot(ToRegister(result()), RootIndex::kTrueValue);
  __ bind(&done);
}

void TaggedNotEqual::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(lhs());
  UseRegister(rhs());
  DefineAsRegister(vreg_state, this);
}
void TaggedNotEqual::GenerateCode(MaglevAssembler* masm,
                                  const ProcessingState& state) {
  Label done, if_equal;
  __ cmp_tagged(ToRegister(lhs()), ToRegister(rhs()));
  __ j(equal, &if_equal, Label::kNear);
  __ LoadRoot(ToRegister(result()), RootIndex::kTrueValue);
  __ jmp(&done, Label::kNear);
  __ bind(&if_equal);
  __ LoadRoot(ToRegister(result()), RootIndex::kFalseValue);
  __ bind(&done);
}

void TestInstanceOf::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  using D = CallInterfaceDescriptorFor<Builtin::kInstanceOf_WithFeedback>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(object(), D::GetRegisterParameter(D::kLeft));
  UseFixed(callable(), D::GetRegisterParameter(D::kRight));
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void TestInstanceOf::GenerateCode(MaglevAssembler* masm,
                                  const ProcessingState& state) {
  using D = CallInterfaceDescriptorFor<Builtin::kInstanceOf_WithFeedback>::type;
#ifdef DEBUG
  DCHECK_EQ(ToRegister(context()), kContextRegister);
  DCHECK_EQ(ToRegister(object()), D::GetRegisterParameter(D::kLeft));
  DCHECK_EQ(ToRegister(callable()), D::GetRegisterParameter(D::kRight));
#endif
  __ Move(D::GetRegisterParameter(D::kFeedbackVector), feedback().vector);
  __ Move(D::GetRegisterParameter(D::kSlot), Immediate(feedback().index()));
  __ CallBuiltin(Builtin::kInstanceOf_WithFeedback);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void TestUndetectable::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(value());
  set_temporaries_needed(1);
  DefineAsRegister(vreg_state, this);
}
void TestUndetectable::GenerateCode(MaglevAssembler* masm,
                                    const ProcessingState& state) {
  Register object = ToRegister(value());
  Register return_value = ToRegister(result());
  Register scratch = general_temporaries().PopFirst();

  Label return_false, done;
  __ JumpIfSmi(object, &return_false, Label::kNear);
  // For heap objects, check the map's undetectable bit.
  __ LoadMap(scratch, object);
  __ testl(FieldOperand(scratch, Map::kBitFieldOffset),
           Immediate(Map::Bits1::IsUndetectableBit::kMask));
  __ j(zero, &return_false, Label::kNear);

  __ LoadRoot(return_value, RootIndex::kTrueValue);
  __ jmp(&done, Label::kNear);

  __ bind(&return_false);
  __ LoadRoot(return_value, RootIndex::kFalseValue);

  __ bind(&done);
}

void TestTypeOf::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(value());
  DefineAsRegister(vreg_state, this);
}
void TestTypeOf::GenerateCode(MaglevAssembler* masm,
                              const ProcessingState& state) {
  using LiteralFlag = interpreter::TestTypeOfFlags::LiteralFlag;
  Register object = ToRegister(value());
  // Use return register as temporary if needed.
  Register tmp = ToRegister(result());
  Label is_true, is_false, done;
  switch (literal_) {
    case LiteralFlag::kNumber:
      __ JumpIfSmi(object, &is_true, Label::kNear);
      __ CompareRoot(FieldOperand(object, HeapObject::kMapOffset),
                     RootIndex::kHeapNumberMap);
      __ j(not_equal, &is_false, Label::kNear);
      break;
    case LiteralFlag::kString:
      __ JumpIfSmi(object, &is_false, Label::kNear);
      __ LoadMap(tmp, object);
      __ cmpw(FieldOperand(tmp, Map::kInstanceTypeOffset),
              Immediate(FIRST_NONSTRING_TYPE));
      __ j(greater_equal, &is_false, Label::kNear);
      break;
    case LiteralFlag::kSymbol:
      __ JumpIfSmi(object, &is_false, Label::kNear);
      __ LoadMap(tmp, object);
      __ cmpw(FieldOperand(tmp, Map::kInstanceTypeOffset),
              Immediate(SYMBOL_TYPE));
      __ j(not_equal, &is_false, Label::kNear);
      break;
    case LiteralFlag::kBoolean:
      __ CompareRoot(object, RootIndex::kTrueValue);
      __ j(equal, &is_true, Label::kNear);
      __ CompareRoot(object, RootIndex::kFalseValue);
      __ j(not_equal, &is_false, Label::kNear);
      break;
    case LiteralFlag::kBigInt:
      __ JumpIfSmi(object, &is_false, Label::kNear);
      __ LoadMap(tmp, object);
      __ cmpw(FieldOperand(tmp, Map::kInstanceTypeOffset),
              Immediate(BIGINT_TYPE));
      __ j(not_equal, &is_false, Label::kNear);
      break;
    case LiteralFlag::kUndefined:
      __ JumpIfSmi(object, &is_false, Label::kNear);
      // Check it has the undetectable bit set and it is not null.
      __ LoadMap(tmp, object);
      __ testl(FieldOperand(tmp, Map::kBitFieldOffset),
               Immediate(Map::Bits1::IsUndetectableBit::kMask));
      __ j(zero, &is_false, Label::kNear);
      __ CompareRoot(object, RootIndex::kNullValue);
      __ j(equal, &is_false, Label::kNear);
      break;
    case LiteralFlag::kFunction:
      __ JumpIfSmi(object, &is_false, Label::kNear);
      // Check if callable bit is set and not undetectable.
      __ LoadMap(tmp, object);
      __ movl(tmp, FieldOperand(tmp, Map::kBitFieldOffset));
      __ andl(tmp, Immediate(Map::Bits1::IsUndetectableBit::kMask |
                             Map::Bits1::IsCallableBit::kMask));
      __ cmpl(tmp, Immediate(Map::Bits1::IsCallableBit::kMask));
      __ j(not_equal, &is_false, Label::kNear);
      break;
    case LiteralFlag::kObject:
      __ JumpIfSmi(object, &is_false, Label::kNear);
      // If the object is null then return true.
      __ CompareRoot(object, RootIndex::kNullValue);
      __ j(equal, &is_true, Label::kNear);
      // Check if the object is a receiver type,
      __ LoadMap(tmp, object);
      __ cmpw(FieldOperand(tmp, Map::kInstanceTypeOffset),
              Immediate(FIRST_JS_RECEIVER_TYPE));
      __ j(less, &is_false, Label::kNear);
      // ... and is not undefined (undetectable) nor callable.
      __ testl(FieldOperand(tmp, Map::kBitFieldOffset),
               Immediate(Map::Bits1::IsUndetectableBit::kMask |
                         Map::Bits1::IsCallableBit::kMask));
      __ j(not_zero, &is_false, Label::kNear);
      break;
    case LiteralFlag::kOther:
      UNREACHABLE();
  }
  __ bind(&is_true);
  __ LoadRoot(ToRegister(result()), RootIndex::kTrueValue);
  __ jmp(&done, Label::kNear);
  __ bind(&is_false);
  __ LoadRoot(ToRegister(result()), RootIndex::kFalseValue);
  __ bind(&done);
}

void ToName::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  using D = CallInterfaceDescriptorFor<Builtin::kToName>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(value_input(), D::GetRegisterParameter(D::kInput));
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void ToName::GenerateCode(MaglevAssembler* masm, const ProcessingState& state) {
#ifdef DEBUG
  using D = CallInterfaceDescriptorFor<Builtin::kToName>::type;
  DCHECK_EQ(ToRegister(context()), kContextRegister);
  DCHECK_EQ(ToRegister(value_input()), D::GetRegisterParameter(D::kInput));
#endif  // DEBUG
  __ CallBuiltin(Builtin::kToName);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void ToNumberOrNumeric::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  using D = TypeConversionDescriptor;
  UseFixed(context(), kContextRegister);
  UseFixed(value_input(), D::GetRegisterParameter(D::kArgument));
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void ToNumberOrNumeric::GenerateCode(MaglevAssembler* masm,
                                     const ProcessingState& state) {
  switch (mode()) {
    case Object::Conversion::kToNumber:
      __ CallBuiltin(Builtin::kToNumber);
      break;
    case Object::Conversion::kToNumeric:
      __ CallBuiltin(Builtin::kToNumeric);
      break;
  }
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void ToObject::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  using D = CallInterfaceDescriptorFor<Builtin::kToObject>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(value_input(), D::GetRegisterParameter(D::kInput));
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void ToObject::GenerateCode(MaglevAssembler* masm,
                            const ProcessingState& state) {
#ifdef DEBUG
  using D = CallInterfaceDescriptorFor<Builtin::kToObject>::type;
  DCHECK_EQ(ToRegister(context()), kContextRegister);
  DCHECK_EQ(ToRegister(value_input()), D::GetRegisterParameter(D::kInput));
#endif  // DEBUG
  Register value = ToRegister(value_input());
  Label call_builtin, done;
  // Avoid the builtin call if {value} is a JSReceiver.
  __ JumpIfSmi(value, &call_builtin);
  __ LoadMap(kScratchRegister, value);
  __ cmpw(FieldOperand(kScratchRegister, Map::kInstanceTypeOffset),
          Immediate(FIRST_JS_RECEIVER_TYPE));
  __ j(greater_equal, &done);
  __ bind(&call_builtin);
  __ CallBuiltin(Builtin::kToObject);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
  __ bind(&done);
}

void ToString::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  using D = CallInterfaceDescriptorFor<Builtin::kToString>::type;
  UseFixed(context(), kContextRegister);
  UseFixed(value_input(), D::GetRegisterParameter(D::kO));
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void ToString::GenerateCode(MaglevAssembler* masm,
                            const ProcessingState& state) {
#ifdef DEBUG
  using D = CallInterfaceDescriptorFor<Builtin::kToString>::type;
  DCHECK_EQ(ToRegister(context()), kContextRegister);
  DCHECK_EQ(ToRegister(value_input()), D::GetRegisterParameter(D::kO));
#endif  // DEBUG
  Register value = ToRegister(value_input());
  Label call_builtin, done;
  // Avoid the builtin call if {value} is a string.
  __ JumpIfSmi(value, &call_builtin);
  __ LoadMap(kScratchRegister, value);
  __ cmpw(FieldOperand(kScratchRegister, Map::kInstanceTypeOffset),
          Immediate(FIRST_NONSTRING_TYPE));
  __ j(less, &done);
  __ bind(&call_builtin);
  __ CallBuiltin(Builtin::kToString);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
  __ bind(&done);
}

void CheckedInt32ToUint32::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(input());
  DefineSameAsFirst(vreg_state, this);
}
void CheckedInt32ToUint32::GenerateCode(MaglevAssembler* masm,
                                        const ProcessingState& state) {
  Register input_reg = ToRegister(input());
  __ testl(input_reg, input_reg);
  __ EmitEagerDeoptIf(negative, DeoptimizeReason::kNotUint32, this);
}

void CheckedUint32ToInt32::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(input());
  DefineSameAsFirst(vreg_state, this);
}
void CheckedUint32ToInt32::GenerateCode(MaglevAssembler* masm,
                                        const ProcessingState& state) {
  Register input_reg = ToRegister(input());
  // Check if the top bit is set -- if it is, then this is not a valid int32,
  // otherwise it is.
  __ testl(input_reg, Immediate(1 << 31));
  __ EmitEagerDeoptIf(not_zero, DeoptimizeReason::kNotInt32, this);
}

void ChangeInt32ToFloat64::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(input());
  DefineAsRegister(vreg_state, this);
}
void ChangeInt32ToFloat64::GenerateCode(MaglevAssembler* masm,
                                        const ProcessingState& state) {
  __ Cvtlsi2sd(ToDoubleRegister(result()), ToRegister(input()));
}

void ChangeUint32ToFloat64::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(input());
  DefineAsRegister(vreg_state, this);
}
void ChangeUint32ToFloat64::GenerateCode(MaglevAssembler* masm,
                                         const ProcessingState& state) {
  // TODO(leszeks): Cvtlui2sd does a manual movl to clear the top bits of the
  // input register. We could eliminate this movl by ensuring that word32
  // registers are always written with 32-bit ops and not 64-bit ones.
  __ Cvtlui2sd(ToDoubleRegister(result()), ToRegister(input()));
}

void CheckedTruncateFloat64ToInt32::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(input());
  DefineAsRegister(vreg_state, this);
}
void CheckedTruncateFloat64ToInt32::GenerateCode(MaglevAssembler* masm,
                                                 const ProcessingState& state) {
  DoubleRegister input_reg = ToDoubleRegister(input());
  Register result_reg = ToRegister(result());
  DoubleRegister converted_back = kScratchDoubleReg;

  // Convert the input float64 value to int32.
  __ Cvttsd2si(result_reg, input_reg);
  // Convert that int32 value back to float64.
  __ Cvtlsi2sd(converted_back, result_reg);
  // Check that the result of the float64->int32->float64 is equal to the input
  // (i.e. that the conversion didn't truncate.
  __ Ucomisd(input_reg, converted_back);
  __ EmitEagerDeoptIf(not_equal, DeoptimizeReason::kNotInt32, this);

  // Check if {input} is -0.
  Label check_done;
  __ cmpl(result_reg, Immediate(0));
  __ j(not_equal, &check_done);

  // In case of 0, we need to check the high bits for the IEEE -0 pattern.
  Register high_word32_of_input = kScratchRegister;
  __ Pextrd(high_word32_of_input, input_reg, 1);
  __ cmpl(high_word32_of_input, Immediate(0));
  __ EmitEagerDeoptIf(less, DeoptimizeReason::kNotInt32, this);

  __ bind(&check_done);
}

void CheckedTruncateFloat64ToUint32::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(input());
  DefineAsRegister(vreg_state, this);
}
void CheckedTruncateFloat64ToUint32::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  DoubleRegister input_reg = ToDoubleRegister(input());
  Register result_reg = ToRegister(result());
  DoubleRegister converted_back = kScratchDoubleReg;

  Label fail;
  // Convert the input float64 value to uint32.
  __ Cvttsd2ui(result_reg, input_reg, &fail);
  // Convert that uint32 value back to float64.
  __ Cvtlui2sd(converted_back, result_reg);
  // Check that the result of the float64->uint32->float64 is equal to the input
  // (i.e. that the conversion didn't truncate.
  __ Ucomisd(input_reg, converted_back);
  __ EmitEagerDeoptIf(not_equal, DeoptimizeReason::kNotUint32, this);

  // Check if {input} is -0.
  Label check_done;
  __ cmpl(result_reg, Immediate(0));
  __ j(not_equal, &check_done);

  // In case of 0, we need to check the high bits for the IEEE -0 pattern.
  Register high_word32_of_input = kScratchRegister;
  __ Pextrd(high_word32_of_input, input_reg, 1);
  __ cmpl(high_word32_of_input, Immediate(0));
  __ EmitEagerDeoptIf(less, DeoptimizeReason::kNotUint32, this);

  __ bind(&fail);
  __ EmitEagerDeopt(this, DeoptimizeReason::kNotUint32);

  __ bind(&check_done);
}

void TruncateUint32ToInt32::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(input());
  DefineSameAsFirst(vreg_state, this);
}
void TruncateUint32ToInt32::GenerateCode(MaglevAssembler* masm,
                                         const ProcessingState& state) {
  // No code emitted -- as far as the machine is concerned, int32 is uint32.
  DCHECK_EQ(ToRegister(input()), ToRegister(result()));
}

void TruncateFloat64ToInt32::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(input());
  DefineAsRegister(vreg_state, this);
}
void TruncateFloat64ToInt32::GenerateCode(MaglevAssembler* masm,
                                          const ProcessingState& state) {
  __ TruncateDoubleToInt32(ToRegister(result()), ToDoubleRegister(input()));
}

void Phi::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  // Phi inputs are processed in the post-process, once loop phis' inputs'
  // v-regs are allocated.

  // We have to pass a policy, but it is later ignored during register
  // allocation. See StraightForwardRegisterAllocator::AllocateRegisters
  // which has special handling for Phis.
  static const compiler::UnallocatedOperand::ExtendedPolicy kIgnoredPolicy =
      compiler::UnallocatedOperand::REGISTER_OR_SLOT_OR_CONSTANT;

  result().SetUnallocated(kIgnoredPolicy,
                          vreg_state->AllocateVirtualRegister());
}
// TODO(verwaest): Remove after switching the register allocator.
void Phi::AllocateVregInPostProcess(MaglevVregAllocationState* vreg_state) {
  for (Input& input : *this) {
    UseAny(input);
  }
}
void Phi::GenerateCode(MaglevAssembler* masm, const ProcessingState& state) {}
void Phi::PrintParams(std::ostream& os,
                      MaglevGraphLabeller* graph_labeller) const {
  os << "(" << owner().ToString() << ")";
}

void Call::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  // TODO(leszeks): Consider splitting Call into with- and without-feedback
  // opcodes, rather than checking for feedback validity.
  if (feedback_.IsValid()) {
    using D = CallTrampoline_WithFeedbackDescriptor;
    UseFixed(function(), D::GetRegisterParameter(D::kFunction));
    UseFixed(arg(0), D::GetRegisterParameter(D::kReceiver));
  } else {
    using D = CallTrampolineDescriptor;
    UseFixed(function(), D::GetRegisterParameter(D::kFunction));
    UseAny(arg(0));
  }
  for (int i = 1; i < num_args(); i++) {
    UseAny(arg(i));
  }
  UseFixed(context(), kContextRegister);
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void Call::GenerateCode(MaglevAssembler* masm, const ProcessingState& state) {
  // TODO(leszeks): Port the nice Sparkplug CallBuiltin helper.
#ifdef DEBUG
  if (feedback_.IsValid()) {
    using D = CallTrampoline_WithFeedbackDescriptor;
    DCHECK_EQ(ToRegister(function()), D::GetRegisterParameter(D::kFunction));
    DCHECK_EQ(ToRegister(arg(0)), D::GetRegisterParameter(D::kReceiver));
  } else {
    using D = CallTrampolineDescriptor;
    DCHECK_EQ(ToRegister(function()), D::GetRegisterParameter(D::kFunction));
  }
#endif
  DCHECK_EQ(ToRegister(context()), kContextRegister);

  for (int i = num_args() - 1; i >= 0; --i) {
    __ PushInput(arg(i));
  }

  uint32_t arg_count = num_args();
  if (feedback_.IsValid()) {
    DCHECK_EQ(TargetType::kAny, target_type_);
    using D = CallTrampoline_WithFeedbackDescriptor;
    __ Move(D::GetRegisterParameter(D::kActualArgumentsCount),
            Immediate(arg_count));
    __ Move(D::GetRegisterParameter(D::kFeedbackVector), feedback().vector);
    __ Move(D::GetRegisterParameter(D::kSlot), Immediate(feedback().index()));

    switch (receiver_mode_) {
      case ConvertReceiverMode::kNullOrUndefined:
        __ CallBuiltin(Builtin::kCall_ReceiverIsNullOrUndefined_WithFeedback);
        break;
      case ConvertReceiverMode::kNotNullOrUndefined:
        __ CallBuiltin(
            Builtin::kCall_ReceiverIsNotNullOrUndefined_WithFeedback);
        break;
      case ConvertReceiverMode::kAny:
        __ CallBuiltin(Builtin::kCall_ReceiverIsAny_WithFeedback);
        break;
    }
  } else if (target_type_ == TargetType::kAny) {
    using D = CallTrampolineDescriptor;
    __ Move(D::GetRegisterParameter(D::kActualArgumentsCount),
            Immediate(arg_count));

    switch (receiver_mode_) {
      case ConvertReceiverMode::kNullOrUndefined:
        __ CallBuiltin(Builtin::kCall_ReceiverIsNullOrUndefined);
        break;
      case ConvertReceiverMode::kNotNullOrUndefined:
        __ CallBuiltin(Builtin::kCall_ReceiverIsNotNullOrUndefined);
        break;
      case ConvertReceiverMode::kAny:
        __ CallBuiltin(Builtin::kCall_ReceiverIsAny);
        break;
    }
  } else {
    DCHECK_EQ(TargetType::kJSFunction, target_type_);
    using D = CallTrampolineDescriptor;
    __ Move(D::GetRegisterParameter(D::kActualArgumentsCount),
            Immediate(arg_count));

    switch (receiver_mode_) {
      case ConvertReceiverMode::kNullOrUndefined:
        __ CallBuiltin(Builtin::kCallFunction_ReceiverIsNullOrUndefined);
        break;
      case ConvertReceiverMode::kNotNullOrUndefined:
        __ CallBuiltin(Builtin::kCallFunction_ReceiverIsNotNullOrUndefined);
        break;
      case ConvertReceiverMode::kAny:
        __ CallBuiltin(Builtin::kCallFunction_ReceiverIsAny);
        break;
    }
  }

  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}
void Call::PrintParams(std::ostream& os,
                       MaglevGraphLabeller* graph_labeller) const {
  os << "(" << receiver_mode_ << ", ";
  switch (target_type_) {
    case TargetType::kJSFunction:
      os << "JSFunction";
      break;
    case TargetType::kAny:
      os << "Any";
      break;
  }
  os << ")";
}

void CallKnownJSFunction::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseAny(receiver());
  for (int i = 0; i < num_args(); i++) {
    UseAny(arg(i));
  }
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void CallKnownJSFunction::GenerateCode(MaglevAssembler* masm,
                                       const ProcessingState& state) {
  int expected_parameter_count =
      shared_function_info().internal_formal_parameter_count_with_receiver();
  int actual_parameter_count = num_args() + 1;
  if (actual_parameter_count < expected_parameter_count) {
    int number_of_undefineds =
        expected_parameter_count - actual_parameter_count;
    __ LoadRoot(kScratchRegister, RootIndex::kUndefinedValue);
    for (int i = 0; i < number_of_undefineds; i++) {
      __ Push(kScratchRegister);
    }
  }
  for (int i = num_args() - 1; i >= 0; --i) {
    __ PushInput(arg(i));
  }
  __ PushInput(receiver());
  __ Move(kContextRegister, function_.context().object());
  __ Move(kJavaScriptCallTargetRegister, function_.object());
  __ LoadRoot(kJavaScriptCallNewTargetRegister, RootIndex::kUndefinedValue);
  __ Move(kJavaScriptCallArgCountRegister, Immediate(actual_parameter_count));
  if (shared_function_info().HasBuiltinId()) {
    __ CallBuiltin(shared_function_info().builtin_id());
  } else {
    __ AssertCallableFunction(kJavaScriptCallTargetRegister);
    __ LoadTaggedPointerField(
        kJavaScriptCallCodeStartRegister,
        FieldOperand(kJavaScriptCallTargetRegister, JSFunction::kCodeOffset));
    __ CallCodeTObject(kJavaScriptCallCodeStartRegister);
  }
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}
void CallKnownJSFunction::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << function_.object() << ")";
}

void Construct::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  using D = Construct_WithFeedbackDescriptor;
  UseFixed(function(), D::GetRegisterParameter(D::kTarget));
  UseFixed(new_target(), D::GetRegisterParameter(D::kNewTarget));
  UseFixed(context(), kContextRegister);
  for (int i = 0; i < num_args(); i++) {
    UseAny(arg(i));
  }
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void Construct::GenerateCode(MaglevAssembler* masm,
                             const ProcessingState& state) {
  using D = Construct_WithFeedbackDescriptor;
  DCHECK_EQ(ToRegister(function()), D::GetRegisterParameter(D::kTarget));
  DCHECK_EQ(ToRegister(new_target()), D::GetRegisterParameter(D::kNewTarget));
  DCHECK_EQ(ToRegister(context()), kContextRegister);

  for (int i = num_args() - 1; i >= 0; --i) {
    __ PushInput(arg(i));
  }
  static_assert(D::GetStackParameterIndex(D::kFeedbackVector) == 0);
  static_assert(D::GetStackParameterCount() == 1);
  __ Push(feedback().vector);

  uint32_t arg_count = num_args();
  __ Move(D::GetRegisterParameter(D::kActualArgumentsCount),
          Immediate(arg_count));
  __ Move(D::GetRegisterParameter(D::kSlot), Immediate(feedback().index()));

  __ CallBuiltin(Builtin::kConstruct_WithFeedback);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void CallBuiltin::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  auto descriptor = Builtins::CallInterfaceDescriptorFor(builtin());
  bool has_context = descriptor.HasContextParameter();
  int i = 0;
  for (; i < InputsInRegisterCount(); i++) {
    UseFixed(input(i), descriptor.GetRegisterParameter(i));
  }
  for (; i < InputCountWithoutContext(); i++) {
    UseAny(input(i));
  }
  if (has_context) {
    UseFixed(input(i), kContextRegister);
  }
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}

void CallBuiltin::PassFeedbackSlotOnStack(MaglevAssembler* masm) {
  DCHECK(has_feedback());
  switch (slot_type()) {
    case kTaggedIndex:
      __ Push(TaggedIndex::FromIntptr(feedback().index()));
      break;
    case kSmi:
      __ Push(Smi::FromInt(feedback().index()));
      break;
  }
}

void CallBuiltin::PassFeedbackSlotInRegister(MaglevAssembler* masm) {
  DCHECK(has_feedback());
  auto descriptor = Builtins::CallInterfaceDescriptorFor(builtin());
  int slot_index = InputCountWithoutContext();
  switch (slot_type()) {
    case kTaggedIndex:
      __ Move(descriptor.GetRegisterParameter(slot_index),
              TaggedIndex::FromIntptr(feedback().index()));
      break;
    case kSmi:
      __ Move(descriptor.GetRegisterParameter(slot_index),
              Smi::FromInt(feedback().index()));
      break;
  }
}

void CallBuiltin::PushFeedback(MaglevAssembler* masm) {
  DCHECK(has_feedback());

  auto descriptor = Builtins::CallInterfaceDescriptorFor(builtin());
  int slot_index = InputCountWithoutContext();
  int vector_index = slot_index + 1;

  // There are three possibilities:
  // 1. Feedback slot and vector are in register.
  // 2. Feedback slot is in register and vector is on stack.
  // 3. Feedback slot and vector are on stack.
  if (vector_index < descriptor.GetRegisterParameterCount()) {
    PassFeedbackSlotInRegister(masm);
    __ Move(descriptor.GetRegisterParameter(vector_index), feedback().vector);
  } else if (vector_index == descriptor.GetRegisterParameterCount()) {
    PassFeedbackSlotInRegister(masm);
    // We do not allow var args if has_feedback(), so here we have only one
    // parameter on stack and do not need to check stack arguments order.
    __ Push(feedback().vector);
  } else {
    // Same as above. We does not allow var args if has_feedback(), so feedback
    // slot and vector must be last two inputs.
    if (descriptor.GetStackArgumentOrder() == StackArgumentOrder::kDefault) {
      PassFeedbackSlotOnStack(masm);
      __ Push(feedback().vector);
    } else {
      DCHECK_EQ(descriptor.GetStackArgumentOrder(), StackArgumentOrder::kJS);
      __ Push(feedback().vector);
      PassFeedbackSlotOnStack(masm);
    }
  }
}

void CallBuiltin::GenerateCode(MaglevAssembler* masm,
                               const ProcessingState& state) {
  auto descriptor = Builtins::CallInterfaceDescriptorFor(builtin());

  if (descriptor.GetStackArgumentOrder() == StackArgumentOrder::kDefault) {
    for (int i = InputsInRegisterCount(); i < InputCountWithoutContext(); ++i) {
      __ PushInput(input(i));
    }
    if (has_feedback()) {
      PushFeedback(masm);
    }
  } else {
    DCHECK_EQ(descriptor.GetStackArgumentOrder(), StackArgumentOrder::kJS);
    if (has_feedback()) {
      PushFeedback(masm);
    }
    for (int i = InputCountWithoutContext() - 1; i >= InputsInRegisterCount();
         --i) {
      __ PushInput(input(i));
    }
  }
  __ CallBuiltin(builtin());
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}
void CallBuiltin::PrintParams(std::ostream& os,
                              MaglevGraphLabeller* graph_labeller) const {
  os << "(" << Builtins::name(builtin()) << ")";
}

void CallRuntime::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseFixed(context(), kContextRegister);
  for (int i = 0; i < num_args(); i++) {
    UseAny(arg(i));
  }
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void CallRuntime::GenerateCode(MaglevAssembler* masm,
                               const ProcessingState& state) {
  DCHECK_EQ(ToRegister(context()), kContextRegister);
  for (int i = 0; i < num_args(); i++) {
    __ PushInput(arg(i));
  }
  __ CallRuntime(function_id(), num_args());
  // TODO(victorgomes): Not sure if this is needed for all runtime calls.
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}
void CallRuntime::PrintParams(std::ostream& os,
                              MaglevGraphLabeller* graph_labeller) const {
  os << "(" << Runtime::FunctionForId(function_id())->name << ")";
}

void CallWithSpread::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  if (feedback_.IsValid()) {
    using D =
        CallInterfaceDescriptorFor<Builtin::kCallWithSpread_WithFeedback>::type;
    UseFixed(function(), D::GetRegisterParameter(D::kTarget));
    UseFixed(spread(), D::GetRegisterParameter(D::kSpread));
  } else {
    using D = CallInterfaceDescriptorFor<Builtin::kCallWithSpread>::type;
    UseFixed(function(), D::GetRegisterParameter(D::kTarget));
    UseFixed(spread(), D::GetRegisterParameter(D::kSpread));
  }
  UseFixed(context(), kContextRegister);
  for (int i = 0; i < num_args() - 1; i++) {
    UseAny(arg(i));
  }
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void CallWithSpread::GenerateCode(MaglevAssembler* masm,
                                  const ProcessingState& state) {
#ifdef DEBUG
  if (feedback_.IsValid()) {
    using D =
        CallInterfaceDescriptorFor<Builtin::kCallWithSpread_WithFeedback>::type;
    DCHECK_EQ(ToRegister(function()), D::GetRegisterParameter(D::kTarget));
    DCHECK_EQ(ToRegister(spread()), D::GetRegisterParameter(D::kSpread));
  } else {
    using D = CallInterfaceDescriptorFor<Builtin::kCallWithSpread>::type;
    DCHECK_EQ(ToRegister(function()), D::GetRegisterParameter(D::kTarget));
    DCHECK_EQ(ToRegister(spread()), D::GetRegisterParameter(D::kSpread));
  }
  DCHECK_EQ(ToRegister(context()), kContextRegister);
#endif
  // Push other arguments (other than the spread) to the stack.
  int argc_no_spread = num_args() - 1;
  for (int i = argc_no_spread - 1; i >= 0; --i) {
    __ PushInput(arg(i));
  }
  if (feedback_.IsValid()) {
    using D =
        CallInterfaceDescriptorFor<Builtin::kCallWithSpread_WithFeedback>::type;
    static_assert(D::GetStackParameterIndex(D::kReceiver) == 0);
    static_assert(D::GetStackParameterCount() == 1);
    __ PushInput(arg(0));
    __ Move(D::GetRegisterParameter(D::kArgumentsCount),
            Immediate(argc_no_spread));
    __ Move(D::GetRegisterParameter(D::kFeedbackVector), feedback().vector);
    __ Move(D::GetRegisterParameter(D::kSlot), Immediate(feedback().index()));
    __ CallBuiltin(Builtin::kCallWithSpread_WithFeedback);
  } else {
    using D = CallInterfaceDescriptorFor<Builtin::kCallWithSpread>::type;
    __ Move(D::GetRegisterParameter(D::kArgumentsCount),
            Immediate(argc_no_spread));
    __ CallBuiltin(Builtin::kCallWithSpread);
  }

  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void CallWithArrayLike::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  using D = CallInterfaceDescriptorFor<Builtin::kCallWithArrayLike>::type;
  UseFixed(function(), D::GetRegisterParameter(D::kTarget));
  UseAny(receiver());
  UseFixed(arguments_list(), D::GetRegisterParameter(D::kArgumentsList));
  UseFixed(context(), kContextRegister);
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void CallWithArrayLike::GenerateCode(MaglevAssembler* masm,
                                     const ProcessingState& state) {
#ifdef DEBUG
  using D = CallInterfaceDescriptorFor<Builtin::kCallWithArrayLike>::type;
  DCHECK_EQ(ToRegister(function()), D::GetRegisterParameter(D::kTarget));
  DCHECK_EQ(ToRegister(arguments_list()),
            D::GetRegisterParameter(D::kArgumentsList));
  DCHECK_EQ(ToRegister(context()), kContextRegister);
#endif  // DEBUG
  __ PushInput(receiver());
  __ CallBuiltin(Builtin::kCallWithArrayLike);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void ConstructWithSpread::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  using D = CallInterfaceDescriptorFor<
      Builtin::kConstructWithSpread_WithFeedback>::type;
  UseFixed(function(), D::GetRegisterParameter(D::kTarget));
  UseFixed(new_target(), D::GetRegisterParameter(D::kNewTarget));
  UseFixed(context(), kContextRegister);
  for (int i = 0; i < num_args() - 1; i++) {
    UseAny(arg(i));
  }
  UseFixed(spread(), D::GetRegisterParameter(D::kSpread));
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void ConstructWithSpread::GenerateCode(MaglevAssembler* masm,
                                       const ProcessingState& state) {
  using D = CallInterfaceDescriptorFor<
      Builtin::kConstructWithSpread_WithFeedback>::type;
  DCHECK_EQ(ToRegister(function()), D::GetRegisterParameter(D::kTarget));
  DCHECK_EQ(ToRegister(new_target()), D::GetRegisterParameter(D::kNewTarget));
  DCHECK_EQ(ToRegister(context()), kContextRegister);
  // Push other arguments (other than the spread) to the stack.
  int argc_no_spread = num_args() - 1;
  for (int i = argc_no_spread - 1; i >= 0; --i) {
    __ PushInput(arg(i));
  }
  static_assert(D::GetStackParameterIndex(D::kFeedbackVector) == 0);
  static_assert(D::GetStackParameterCount() == 1);
  __ Push(feedback().vector);

  __ Move(D::GetRegisterParameter(D::kActualArgumentsCount),
          Immediate(argc_no_spread));
  __ Move(D::GetRegisterParameter(D::kSlot), Immediate(feedback().index()));
  __ CallBuiltin(Builtin::kConstructWithSpread_WithFeedback);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
}

void ConvertReceiver::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  using D = CallInterfaceDescriptorFor<Builtin::kToObject>::type;
  UseFixed(receiver_input(), D::GetRegisterParameter(D::kInput));
  set_temporaries_needed(1);
  DefineAsFixed(vreg_state, this, kReturnRegister0);
}
void ConvertReceiver::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  Label convert_to_object, done;
  Register receiver = ToRegister(receiver_input());
  Register scratch = general_temporaries().first();
  __ JumpIfSmi(receiver, &convert_to_object, Label::kNear);
  static_assert(LAST_JS_RECEIVER_TYPE == LAST_TYPE);
  __ CmpObjectType(receiver, FIRST_JS_RECEIVER_TYPE, scratch);
  __ j(above_equal, &done);

  if (mode_ != ConvertReceiverMode::kNotNullOrUndefined) {
    Label convert_global_proxy;
    __ JumpIfRoot(receiver, RootIndex::kUndefinedValue, &convert_global_proxy,
                  Label::kNear);
    __ JumpIfNotRoot(receiver, RootIndex::kNullValue, &convert_to_object,
                     Label::kNear);
    __ bind(&convert_global_proxy);
    {
      // Patch receiver to global proxy.
      __ Move(ToRegister(result()),
              target_.native_context().global_proxy_object().object());
    }
    __ jmp(&done);
  }

  __ bind(&convert_to_object);
  // ToObject needs to be ran with the target context installed.
  __ Move(kContextRegister, target_.context().object());
  __ CallBuiltin(Builtin::kToObject);
  masm->DefineExceptionHandlerAndLazyDeoptPoint(this);
  __ bind(&done);
}

void ConvertHoleToUndefined::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(object_input());
  DefineSameAsFirst(vreg_state, this);
}
void ConvertHoleToUndefined::GenerateCode(MaglevAssembler* masm,
                                          const ProcessingState& state) {
  Label done;
  DCHECK_EQ(ToRegister(object_input()), ToRegister(result()));
  __ JumpIfNotRoot(ToRegister(object_input()), RootIndex::kTheHoleValue, &done,
                   Label::kNear);
  __ LoadRoot(ToRegister(result()), RootIndex::kUndefinedValue);
  __ bind(&done);
}

void IncreaseInterruptBudget::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  set_temporaries_needed(1);
}
void IncreaseInterruptBudget::GenerateCode(MaglevAssembler* masm,
                                           const ProcessingState& state) {
  Register scratch = general_temporaries().first();
  __ movq(scratch, MemOperand(rbp, StandardFrameConstants::kFunctionOffset));
  __ LoadTaggedPointerField(
      scratch, FieldOperand(scratch, JSFunction::kFeedbackCellOffset));
  __ addl(FieldOperand(scratch, FeedbackCell::kInterruptBudgetOffset),
          Immediate(amount()));
}
void IncreaseInterruptBudget::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << amount() << ")";
}

void ReduceInterruptBudget::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  set_temporaries_needed(1);
}
void ReduceInterruptBudget::GenerateCode(MaglevAssembler* masm,
                                         const ProcessingState& state) {
  Register scratch = general_temporaries().first();
  __ movq(scratch, MemOperand(rbp, StandardFrameConstants::kFunctionOffset));
  __ LoadTaggedPointerField(
      scratch, FieldOperand(scratch, JSFunction::kFeedbackCellOffset));
  __ subl(FieldOperand(scratch, FeedbackCell::kInterruptBudgetOffset),
          Immediate(amount()));
  ZoneLabelRef done(masm);
  __ JumpToDeferredIf(
      less,
      [](MaglevAssembler* masm, ZoneLabelRef done,
         ReduceInterruptBudget* node) {
        {
          SaveRegisterStateForCall save_register_state(
              masm, node->register_snapshot());
          __ Move(kContextRegister, masm->native_context().object());
          __ Push(MemOperand(rbp, StandardFrameConstants::kFunctionOffset));
          __ CallRuntime(Runtime::kBytecodeBudgetInterruptWithStackCheck_Maglev,
                         1);
          save_register_state.DefineSafepointWithLazyDeopt(
              node->lazy_deopt_info());
        }
        __ jmp(*done);
      },
      done, this);
  __ bind(*done);
}
void ReduceInterruptBudget::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << amount() << ")";
}

void ThrowReferenceErrorIfHole::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseAny(value());
}
void ThrowReferenceErrorIfHole::GenerateCode(MaglevAssembler* masm,
                                             const ProcessingState& state) {
  if (value().operand().IsRegister()) {
    __ CompareRoot(ToRegister(value()), RootIndex::kTheHoleValue);
  } else {
    DCHECK(value().operand().IsStackSlot());
    __ CompareRoot(masm->ToMemOperand(value()), RootIndex::kTheHoleValue);
  }
  __ JumpToDeferredIf(
      equal,
      [](MaglevAssembler* masm, ThrowReferenceErrorIfHole* node) {
        __ Move(kContextRegister, masm->native_context().object());
        __ Push(node->name().object());
        __ CallRuntime(Runtime::kThrowAccessedUninitializedVariable, 1);
        masm->DefineExceptionHandlerAndLazyDeoptPoint(node);
        __ Abort(AbortReason::kUnexpectedReturnFromThrow);
      },
      this);
}

void ThrowSuperNotCalledIfHole::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseAny(value());
}
void ThrowSuperNotCalledIfHole::GenerateCode(MaglevAssembler* masm,
                                             const ProcessingState& state) {
  if (value().operand().IsRegister()) {
    __ CompareRoot(ToRegister(value()), RootIndex::kTheHoleValue);
  } else {
    DCHECK(value().operand().IsStackSlot());
    __ CompareRoot(masm->ToMemOperand(value()), RootIndex::kTheHoleValue);
  }
  __ JumpToDeferredIf(
      equal,
      [](MaglevAssembler* masm, ThrowSuperNotCalledIfHole* node) {
        __ Move(kContextRegister, masm->native_context().object());
        __ CallRuntime(Runtime::kThrowSuperNotCalled, 0);
        masm->DefineExceptionHandlerAndLazyDeoptPoint(node);
        __ Abort(AbortReason::kUnexpectedReturnFromThrow);
      },
      this);
}

void ThrowSuperAlreadyCalledIfNotHole::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseAny(value());
}
void ThrowSuperAlreadyCalledIfNotHole::GenerateCode(
    MaglevAssembler* masm, const ProcessingState& state) {
  if (value().operand().IsRegister()) {
    __ CompareRoot(ToRegister(value()), RootIndex::kTheHoleValue);
  } else {
    DCHECK(value().operand().IsStackSlot());
    __ CompareRoot(masm->ToMemOperand(value()), RootIndex::kTheHoleValue);
  }
  __ JumpToDeferredIf(
      not_equal,
      [](MaglevAssembler* masm, ThrowSuperAlreadyCalledIfNotHole* node) {
        __ Move(kContextRegister, masm->native_context().object());
        __ CallRuntime(Runtime::kThrowSuperAlreadyCalledError, 0);
        masm->DefineExceptionHandlerAndLazyDeoptPoint(node);
        __ Abort(AbortReason::kUnexpectedReturnFromThrow);
      },
      this);
}

void ThrowIfNotSuperConstructor::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(constructor());
  UseRegister(function());
}
void ThrowIfNotSuperConstructor::GenerateCode(MaglevAssembler* masm,
                                              const ProcessingState& state) {
  __ LoadMap(kScratchRegister, ToRegister(constructor()));
  __ testl(FieldOperand(kScratchRegister, Map::kBitFieldOffset),
           Immediate(Map::Bits1::IsConstructorBit::kMask));
  __ JumpToDeferredIf(
      equal,
      [](MaglevAssembler* masm, ThrowIfNotSuperConstructor* node) {
        __ Push(ToRegister(node->constructor()));
        __ Push(ToRegister(node->function()));
        __ Move(kContextRegister, masm->native_context().object());
        __ CallRuntime(Runtime::kThrowNotSuperConstructor, 2);
        masm->DefineExceptionHandlerAndLazyDeoptPoint(node);
        __ Abort(AbortReason::kUnexpectedReturnFromThrow);
      },
      this);
}

// ---
// Control nodes
// ---
void Return::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseFixed(value_input(), kReturnRegister0);
}
void Return::GenerateCode(MaglevAssembler* masm, const ProcessingState& state) {
  DCHECK_EQ(ToRegister(value_input()), kReturnRegister0);

  // Read the formal number of parameters from the top level compilation unit
  // (i.e. the outermost, non inlined function).
  int formal_params_size =
      masm->compilation_info()->toplevel_compilation_unit()->parameter_count();

  // We're not going to continue execution, so we can use an arbitrary register
  // here instead of relying on temporaries from the register allocator.
  Register actual_params_size = r8;

  // Compute the size of the actual parameters + receiver (in bytes).
  // TODO(leszeks): Consider making this an input into Return to re-use the
  // incoming argc's register (if it's still valid).
  __ movq(actual_params_size,
          MemOperand(rbp, StandardFrameConstants::kArgCOffset));

  // Leave the frame.
  // TODO(leszeks): Add a new frame maker for Maglev.
  __ LeaveFrame(StackFrame::BASELINE);

  // If actual is bigger than formal, then we should use it to free up the stack
  // arguments.
  Label drop_dynamic_arg_size;
  __ cmpq(actual_params_size, Immediate(formal_params_size));
  __ j(greater, &drop_dynamic_arg_size);

  // Drop receiver + arguments according to static formal arguments size.
  __ Ret(formal_params_size * kSystemPointerSize, kScratchRegister);

  __ bind(&drop_dynamic_arg_size);
  // Drop receiver + arguments according to dynamic arguments size.
  __ DropArguments(actual_params_size, r9, TurboAssembler::kCountIsInteger,
                   TurboAssembler::kCountIncludesReceiver);
  __ Ret();
}

void Deopt::AllocateVreg(MaglevVregAllocationState* vreg_state) {}
void Deopt::GenerateCode(MaglevAssembler* masm, const ProcessingState& state) {
  __ EmitEagerDeopt(this, reason());
}
void Deopt::PrintParams(std::ostream& os,
                        MaglevGraphLabeller* graph_labeller) const {
  os << "(" << DeoptimizeReasonToString(reason()) << ")";
}

void Switch::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(value());
}
void Switch::GenerateCode(MaglevAssembler* masm, const ProcessingState& state) {
  std::unique_ptr<Label*[]> labels = std::make_unique<Label*[]>(size());
  for (int i = 0; i < size(); i++) {
    labels[i] = (targets())[i].block_ptr()->label();
  }
  __ Switch(kScratchRegister, ToRegister(value()), value_base(), labels.get(),
            size());
  if (has_fallthrough()) {
    DCHECK_EQ(fallthrough(), state.next_block());
  } else {
    __ Trap();
  }
}

void Jump::AllocateVreg(MaglevVregAllocationState* vreg_state) {}
void Jump::GenerateCode(MaglevAssembler* masm, const ProcessingState& state) {
  // Avoid emitting a jump to the next block.
  if (target() != state.next_block()) {
    __ jmp(target()->label());
  }
}

void JumpToInlined::AllocateVreg(MaglevVregAllocationState* vreg_state) {}
void JumpToInlined::GenerateCode(MaglevAssembler* masm,
                                 const ProcessingState& state) {
  // Avoid emitting a jump to the next block.
  if (target() != state.next_block()) {
    __ jmp(target()->label());
  }
}
void JumpToInlined::PrintParams(std::ostream& os,
                                MaglevGraphLabeller* graph_labeller) const {
  os << "(" << Brief(*unit()->shared_function_info().object()) << ")";
}

void JumpFromInlined::AllocateVreg(MaglevVregAllocationState* vreg_state) {}
void JumpFromInlined::GenerateCode(MaglevAssembler* masm,
                                   const ProcessingState& state) {
  // Avoid emitting a jump to the next block.
  if (target() != state.next_block()) {
    __ jmp(target()->label());
  }
}

namespace {

void AttemptOnStackReplacement(MaglevAssembler* masm,
                               ZoneLabelRef no_code_for_osr,
                               JumpLoopPrologue* node, Register scratch0,
                               Register scratch1, int32_t loop_depth,
                               FeedbackSlot feedback_slot,
                               BytecodeOffset osr_offset) {
  // Two cases may cause us to attempt OSR, in the following order:
  //
  // 1) Presence of cached OSR Turbofan code.
  // 2) The OSR urgency exceeds the current loop depth - in that case, trigger
  //    a Turbofan OSR compilation.
  //
  // See also: InterpreterAssembler::OnStackReplacement.

  baseline::BaselineAssembler basm(masm);
  __ AssertFeedbackVector(scratch0);

  // Case 1).
  Label deopt;
  Register maybe_target_code = scratch1;
  {
    basm.TryLoadOptimizedOsrCode(maybe_target_code, scratch0, feedback_slot,
                                 &deopt, Label::kFar);
  }

  // Case 2).
  {
    __ movb(scratch0, FieldOperand(scratch0, FeedbackVector::kOsrStateOffset));
    __ DecodeField<FeedbackVector::OsrUrgencyBits>(scratch0);
    basm.JumpIfByte(baseline::Condition::kUnsignedLessThanEqual, scratch0,
                    loop_depth, *no_code_for_osr, Label::kNear);

    // The osr_urgency exceeds the current loop_depth, signaling an OSR
    // request. Call into runtime to compile.
    {
      // At this point we need a custom register snapshot since additional
      // registers may be live at the eager deopt below (the normal
      // register_snapshot only contains live registers *after this
      // node*).
      // TODO(v8:7700): Consider making the snapshot location
      // configurable.
      RegisterSnapshot snapshot = node->register_snapshot();
      AddDeoptRegistersToSnapshot(&snapshot, node->eager_deopt_info());
      DCHECK(!snapshot.live_registers.has(maybe_target_code));
      SaveRegisterStateForCall save_register_state(masm, snapshot);
      __ Move(kContextRegister, masm->native_context().object());
      __ Push(Smi::FromInt(osr_offset.ToInt()));
      __ CallRuntime(Runtime::kCompileOptimizedOSRFromMaglev, 1);
      save_register_state.DefineSafepoint();
      __ Move(maybe_target_code, kReturnRegister0);
    }

    // A `0` return value means there is no OSR code available yet. Fall
    // through for now, OSR code will be picked up once it exists and is
    // cached on the feedback vector.
    __ Cmp(maybe_target_code, 0);
    __ j(equal, *no_code_for_osr, Label::kNear);
  }

  __ bind(&deopt);
  if (V8_LIKELY(v8_flags.turbofan)) {
    // None of the mutated input registers should be a register input into the
    // eager deopt info.
    DCHECK_REGLIST_EMPTY(
        RegList{scratch0, scratch1} &
        GetGeneralRegistersUsedAsInputs(node->eager_deopt_info()));
    __ EmitEagerDeopt(node, DeoptimizeReason::kPrepareForOnStackReplacement);
  } else {
    // Fall through. With TF disabled we cannot OSR and thus it doesn't make
    // sense to start the process. We do still perform all remaining
    // bookkeeping above though, to keep Maglev code behavior roughly the same
    // in both configurations.
  }
}

}  // namespace

void JumpLoopPrologue::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  if (!v8_flags.use_osr) return;
  set_temporaries_needed(2);
}
void JumpLoopPrologue::GenerateCode(MaglevAssembler* masm,
                                    const ProcessingState& state) {
  if (!v8_flags.use_osr) return;
  Register scratch0 = general_temporaries().PopFirst();
  Register scratch1 = general_temporaries().PopFirst();

  const Register osr_state = scratch1;
  __ Move(scratch0, unit_->feedback().object());
  __ AssertFeedbackVector(scratch0);
  __ movb(osr_state, FieldOperand(scratch0, FeedbackVector::kOsrStateOffset));

  // The quick initial OSR check. If it passes, we proceed on to more
  // expensive OSR logic.
  static_assert(FeedbackVector::MaybeHasOptimizedOsrCodeBit::encode(true) >
                FeedbackVector::kMaxOsrUrgency);
  __ cmpl(osr_state, Immediate(loop_depth_));
  ZoneLabelRef no_code_for_osr(masm);
  __ JumpToDeferredIf(above, AttemptOnStackReplacement, no_code_for_osr, this,
                      scratch0, scratch1, loop_depth_, feedback_slot_,
                      osr_offset_);
  __ bind(*no_code_for_osr);
}

void JumpLoop::AllocateVreg(MaglevVregAllocationState* vreg_state) {}
void JumpLoop::GenerateCode(MaglevAssembler* masm,
                            const ProcessingState& state) {
  __ jmp(target()->label());
}

void BranchIfRootConstant::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(condition_input());
}
void BranchIfRootConstant::GenerateCode(MaglevAssembler* masm,
                                        const ProcessingState& state) {
  __ CompareRoot(ToRegister(condition_input()), root_index());
  __ Branch(equal, if_true(), if_false(), state.next_block());
}
void BranchIfRootConstant::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << RootsTable::name(root_index_) << ")";
}

void BranchIfUndefinedOrNull::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(condition_input());
}
void BranchIfUndefinedOrNull::GenerateCode(MaglevAssembler* masm,
                                           const ProcessingState& state) {
  Register value = ToRegister(condition_input());
  __ JumpIfRoot(value, RootIndex::kUndefinedValue, if_true()->label());
  __ JumpIfRoot(value, RootIndex::kNullValue, if_true()->label());
  auto* next_block = state.next_block();
  if (if_false() != next_block) {
    __ jmp(if_false()->label());
  }
}

void BranchIfJSReceiver::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(condition_input());
}
void BranchIfJSReceiver::GenerateCode(MaglevAssembler* masm,
                                      const ProcessingState& state) {
  Register value = ToRegister(condition_input());
  __ JumpIfSmi(value, if_false()->label());
  __ LoadMap(kScratchRegister, value);
  __ CmpInstanceType(kScratchRegister, FIRST_JS_RECEIVER_TYPE);
  __ Branch(above_equal, if_true(), if_false(), state.next_block());
}

void BranchIfInt32Compare::AllocateVreg(MaglevVregAllocationState* vreg_state) {
  UseRegister(left_input());
  UseRegister(right_input());
}
void BranchIfInt32Compare::GenerateCode(MaglevAssembler* masm,
                                        const ProcessingState& state) {
  Register left = ToRegister(left_input());
  Register right = ToRegister(right_input());
  __ cmpl(left, right);
  __ Branch(ConditionFor(operation_), if_true(), if_false(),
            state.next_block());
}
void BranchIfFloat64Compare::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << operation_ << ")";
}

void BranchIfFloat64Compare::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(left_input());
  UseRegister(right_input());
}
void BranchIfFloat64Compare::GenerateCode(MaglevAssembler* masm,
                                          const ProcessingState& state) {
  DoubleRegister left = ToDoubleRegister(left_input());
  DoubleRegister right = ToDoubleRegister(right_input());
  __ Ucomisd(left, right);
  __ Branch(ConditionFor(operation_), if_true(), if_false(),
            state.next_block());
}
void BranchIfInt32Compare::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << operation_ << ")";
}

void BranchIfReferenceCompare::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  UseRegister(left_input());
  UseRegister(right_input());
}
void BranchIfReferenceCompare::GenerateCode(MaglevAssembler* masm,
                                            const ProcessingState& state) {
  Register left = ToRegister(left_input());
  Register right = ToRegister(right_input());
  __ cmp_tagged(left, right);
  __ Branch(ConditionFor(operation_), if_true(), if_false(),
            state.next_block());
}
void BranchIfReferenceCompare::PrintParams(
    std::ostream& os, MaglevGraphLabeller* graph_labeller) const {
  os << "(" << operation_ << ")";
}

void BranchIfToBooleanTrue::AllocateVreg(
    MaglevVregAllocationState* vreg_state) {
  // TODO(victorgomes): consider using any input instead.
  UseRegister(condition_input());
}
void BranchIfToBooleanTrue::GenerateCode(MaglevAssembler* masm,
                                         const ProcessingState& state) {
  // BasicBlocks are zone allocated and so safe to be casted to ZoneLabelRef.
  ZoneLabelRef true_label =
      ZoneLabelRef::UnsafeFromLabelPointer(if_true()->label());
  ZoneLabelRef false_label =
      ZoneLabelRef::UnsafeFromLabelPointer(if_false()->label());
  bool fallthrough_when_true = (if_true() == state.next_block());
  __ ToBoolean(ToRegister(condition_input()), true_label, false_label,
               fallthrough_when_true);
}

}  // namespace maglev
}  // namespace internal
}  // namespace v8
