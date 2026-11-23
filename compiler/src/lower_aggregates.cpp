// Large values in memory, not in SSA registers.
//
// The generator works with values: a struct is loaded, passed, returned, built with
// `insertvalue` and merged with `phi` as one LLVM value. That is simple, but for a large struct
// (a table of 1050 OpenGL function pointers, an application object holding arrays) LLVM's
// instruction selection splits every such value into its elements, again for every copy: a
// debug build of the windowing example spent most of its 12 s there.
//
// This pass gives every value of a large struct or array type a place in memory and turns the
// operations on it into `memcpy`s and element accesses:
//
//   load  T, p           ->  memcpy into a slot of the function (the value's snapshot)
//   store T v, p         ->  memcpy(p, address of v)
//   insertvalue a, x, i  ->  copy of a's slot into a new slot, store of x into the element
//   extractvalue a, i    ->  the element's address (large) or a load from it (small)
//   phi / select         ->  phi / select of addresses (a phi copies into a slot of its own)
//   functions            ->  a large result goes through a pointer the caller passes (sret),
//                            a large parameter is a pointer to the caller's copy
//
// A value's slot is written once, where the value is made, and only read afterwards, so
// addresses can be shared like the values were. A phi copies its incoming value into its own
// slot at the end of the predecessor: in a loop the incoming value's slot is written again in
// the next iteration while the phi's value may still be in use.

#include "edax/lower_aggregates.hpp"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/IR/CFG.h"

#include <map>
#include <set>
#include <vector>

namespace edax {
namespace {

class Lowering {
public:
    Lowering(llvm::Module& m, std::uint64_t limit) : m_(m), dl_(m.getDataLayout()), limit_(limit) {}

    void run() {
        // Functions whose signature changes: Edax's own (local linkage); C and the runtime keep theirs.
        std::vector<llvm::Function*> rewrite;
        for (llvm::Function& f : m_) {
            if (f.hasLocalLinkage() && !f.isDeclaration() && changes(f.getFunctionType())) rewrite.push_back(&f);
        }
        for (llvm::Function* f : rewrite) new_signature(f);
        for (llvm::Function& f : m_) {
            if (!f.isDeclaration()) lower_function(f);
        }
        for (auto& [old_fn, new_fn] : replaced_) {
            old_fn->replaceAllUsesWith(new_fn);   // vtables, function pointers: the same address
            old_fn->eraseFromParent();
        }
    }

private:
    llvm::Module&                                    m_;
    const llvm::DataLayout&                          dl_;
    std::uint64_t                                    limit_;
    std::map<llvm::Function*, llvm::Function*>       replaced_;   // old -> new
    std::set<llvm::Function*>                        has_sret_;   // new functions with a result pointer first

    bool large(llvm::Type* t) const {
        return t && (t->isStructTy() || t->isArrayTy()) && t->isSized() && dl_.getTypeAllocSize(t).getFixedValue() >= limit_;
    }

    bool changes(llvm::FunctionType* ft) const {
        if (ft->isVarArg()) return false;
        if (large(ft->getReturnType())) return true;
        for (llvm::Type* p : ft->params()) {
            if (large(p)) return true;
        }
        return false;
    }

    llvm::FunctionType* lowered_type(llvm::FunctionType* ft) {
        llvm::Type*              ptr = llvm::PointerType::get(m_.getContext(), 0);
        std::vector<llvm::Type*> params;
        if (large(ft->getReturnType())) params.push_back(ptr);
        for (llvm::Type* p : ft->params()) params.push_back(large(p) ? ptr : p);
        llvm::Type* ret = large(ft->getReturnType()) ? llvm::Type::getVoidTy(m_.getContext()) : ft->getReturnType();
        return llvm::FunctionType::get(ret, params, false);
    }

    // The attributes for the lowered signature: those of large parameters and results are
    // dropped (they described the values), the others move with their parameter.
    llvm::AttributeList lowered_attributes(const llvm::AttributeList& a, llvm::FunctionType* ft) {
        std::vector<llvm::AttributeSet> params;
        if (large(ft->getReturnType())) params.push_back(llvm::AttributeSet());
        for (unsigned i = 0; i < ft->getNumParams(); ++i) params.push_back(large(ft->getParamType(i)) ? llvm::AttributeSet() : a.getParamAttrs(i));
        return llvm::AttributeList::get(m_.getContext(), a.getFnAttrs(), large(ft->getReturnType()) ? llvm::AttributeSet() : a.getRetAttrs(),
                                        params);
    }

    // The function with the new signature takes over the body; the old parameters are replaced
    // in lower_function (a large one by the address it now is).
    std::map<llvm::Function*, std::vector<llvm::Value*>> param_addresses_;   // new fn -> per old param: address or null

    void new_signature(llvm::Function* f) {
        llvm::FunctionType* ft   = f->getFunctionType();
        llvm::FunctionType* nft  = lowered_type(ft);
        bool                sret = large(ft->getReturnType());
        llvm::Function*     nf   = llvm::Function::Create(nft, f->getLinkage(), f->getAddressSpace(), "", &m_);
        nf->takeName(f);
        nf->copyAttributesFrom(f);
        nf->setAttributes(lowered_attributes(f->getAttributes(), ft));
        unsigned shift = sret ? 1 : 0;
        if (sret) {
            nf->addParamAttr(0, llvm::Attribute::getWithStructRetType(m_.getContext(), ft->getReturnType()));
            nf->addParamAttr(0, llvm::Attribute::NoAlias);
            has_sret_.insert(nf);
        }
        llvm::SmallVector<std::pair<unsigned, llvm::MDNode*>> mds;
        f->getAllMetadata(mds);
        for (auto& [kind, node] : mds) nf->setMetadata(kind, node);
        nf->splice(nf->begin(), f);
        std::vector<llvm::Value*> addresses(ft->getNumParams(), nullptr);
        for (unsigned i = 0; i < ft->getNumParams(); ++i) {
            llvm::Argument* old_arg = f->getArg(i);
            llvm::Argument* new_arg = nf->getArg(i + shift);
            new_arg->takeName(old_arg);
            if (large(ft->getParamType(i))) {
                addresses[i] = new_arg;   // the old argument's uses are lowered against this address
                pending_args_[old_arg] = new_arg;
            } else {
                old_arg->replaceAllUsesWith(new_arg);
            }
        }
        param_addresses_[nf] = std::move(addresses);
        replaced_[f] = nf;
    }

    std::map<llvm::Value*, llvm::Value*> pending_args_;   // old large argument -> its address in the new function

    // --- one function ---------------------------------------------------------------------------

    llvm::Function*                      fn_ = nullptr;
    std::map<llvm::Value*, llvm::Value*> address_;     // large value -> where it is
    std::vector<llvm::Instruction*>      dead_;
    std::vector<llvm::PHINode*>          phis_;        // large phis, with their slot
    std::map<llvm::PHINode*, llvm::AllocaInst*> phi_slot_;

    llvm::AllocaInst* slot(llvm::Type* t, const llvm::Twine& name = "agg") {
        llvm::IRBuilder<> b(&*fn_->getEntryBlock().getFirstInsertionPt());
        llvm::AllocaInst* a = b.CreateAlloca(t, nullptr, name);
        a->setAlignment(dl_.getPrefTypeAlign(t));
        return a;
    }

    void copy(llvm::IRBuilder<>& b, llvm::Value* dst, llvm::Value* src, llvm::Type* t) {
        b.CreateMemCpy(dst, dl_.getABITypeAlign(t), src, dl_.getABITypeAlign(t), dl_.getTypeAllocSize(t).getFixedValue());
    }

    // Where the large value `v` is. Constants get a private global (read-only, like every slot
    // after it is written); undefined values a fresh slot.
    llvm::Value* address(llvm::Value* v, llvm::Instruction* before) {
        if (auto it = address_.find(v); it != address_.end()) return it->second;
        if (auto it = pending_args_.find(v); it != pending_args_.end()) return address_[v] = it->second;
        if (llvm::isa<llvm::UndefValue>(v)) return address_[v] = slot(v->getType(), "undef");
        if (auto* c = llvm::dyn_cast<llvm::Constant>(v)) {
            auto* g = new llvm::GlobalVariable(m_, c->getType(), true, llvm::GlobalValue::PrivateLinkage, c, ".agg");
            g->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
            g->setAlignment(dl_.getABITypeAlign(c->getType()));
            return address_[v] = g;
        }
        // Anything else (a parameter of a function that keeps its signature, a value this pass
        // doesn't know): stored into a slot where it is made.
        llvm::AllocaInst* s = slot(v->getType(), "value");
        llvm::Instruction* at = before;
        if (auto* i = llvm::dyn_cast<llvm::Instruction>(v)) {
            at = i->getNextNode();
            if (llvm::isa<llvm::PHINode>(i)) at = &*i->getParent()->getFirstInsertionPt();
        } else if (llvm::isa<llvm::Argument>(v)) {
            at = &*fn_->getEntryBlock().getFirstInsertionPt();
        }
        llvm::IRBuilder<> b(at);
        b.CreateStore(v, s);
        return address_[v] = s;
    }

    // The value itself, loaded where an operation that still needs it is.
    llvm::Value* materialize(llvm::Value* v, llvm::Instruction* before) {
        llvm::IRBuilder<> b(before);
        return b.CreateLoad(v->getType(), address(v, before));
    }

    llvm::Value* element(llvm::IRBuilder<>& b, llvm::Type* t, llvm::Value* base, llvm::ArrayRef<unsigned> indices) {
        std::vector<llvm::Value*> path{b.getInt32(0)};
        for (unsigned i : indices) path.push_back(b.getInt32(i));
        return b.CreateInBoundsGEP(t, base, path);
    }

    void lower_function(llvm::Function& f) {
        fn_ = &f;
        address_.clear();
        dead_.clear();
        phis_.clear();
        phi_slot_.clear();
        if (auto it = param_addresses_.find(&f); it != param_addresses_.end()) {
            for (auto& [old_arg, addr] : pending_args_) {
                if (llvm::cast<llvm::Argument>(addr)->getParent() == &f) address_[old_arg] = addr;
            }
        }
        llvm::ReversePostOrderTraversal<llvm::Function*> order(&f);
        std::vector<llvm::Instruction*> work;
        for (llvm::BasicBlock* bb : order) {
            for (llvm::Instruction& i : *bb) work.push_back(&i);
        }
        for (llvm::Instruction* i : work) lower(i);
        // The phis' incoming values: copied into the phi's slot at the end of each predecessor.
        // An incoming value that is itself a phi of the same block (a swap in a loop) is read
        // into a temporary first, before any phi's slot is written.
        std::map<std::pair<llvm::PHINode*, unsigned>, llvm::Value*> source;
        for (llvm::PHINode* p : phis_) {
            for (unsigned k = 0; k < p->getNumIncomingValues(); ++k) {
                llvm::Instruction* end = p->getIncomingBlock(k)->getTerminator();
                llvm::Value*       in  = p->getIncomingValue(k);
                llvm::Value*       src = address(in, end);
                if (auto* other = llvm::dyn_cast<llvm::PHINode>(in); other && other->getParent() == p->getParent()) {
                    llvm::AllocaInst* tmp = slot(p->getType(), "phi.tmp");
                    llvm::IRBuilder<> b(end);
                    copy(b, tmp, src, p->getType());
                    src = tmp;
                }
                source[{p, k}] = src;
            }
        }
        for (llvm::PHINode* p : phis_) {
            for (unsigned k = 0; k < p->getNumIncomingValues(); ++k) {
                llvm::IRBuilder<> b(p->getIncomingBlock(k)->getTerminator());
                copy(b, phi_slot_.at(p), source.at({p, k}), p->getType());
            }
        }
        // The old instructions: every use was rewritten.
        for (auto it = dead_.rbegin(); it != dead_.rend(); ++it) {
            llvm::Instruction* i = *it;
            if (!i->use_empty()) i->replaceAllUsesWith(llvm::PoisonValue::get(i->getType()));
            i->eraseFromParent();
        }
        for (auto& [old_arg, addr] : pending_args_) {
            if (llvm::cast<llvm::Argument>(addr)->getParent() == &f && !old_arg->use_empty()) {
                old_arg->replaceAllUsesWith(llvm::PoisonValue::get(old_arg->getType()));
            }
        }
    }

    void lower(llvm::Instruction* i) {
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(i)) {
            lower_call(call);
            return;
        }
        if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(i)) {
            llvm::Value* v = ret->getReturnValue();
            if (v && large(v->getType()) && has_sret_.count(fn_)) {
                llvm::IRBuilder<> b(ret);
                copy(b, fn_->getArg(0), address(v, ret), v->getType());
                b.CreateRetVoid();
                dead_.push_back(ret);
            }
            return;
        }
        if (auto* st = llvm::dyn_cast<llvm::StoreInst>(i)) {
            llvm::Value* v = st->getValueOperand();
            if (!large(v->getType()) || st->isVolatile() || st->isAtomic()) return;
            llvm::IRBuilder<> b(st);
            copy(b, st->getPointerOperand(), address(v, st), v->getType());
            dead_.push_back(st);
            return;
        }
        if (!large(i->getType())) {
            // A small result of a large operand (extractvalue of a field, a comparison, ...).
            if (auto* ev = llvm::dyn_cast<llvm::ExtractValueInst>(i); ev && large(ev->getAggregateOperand()->getType())) {
                llvm::IRBuilder<> b(ev);
                llvm::Value* at = element(b, ev->getAggregateOperand()->getType(), address(ev->getAggregateOperand(), ev), ev->getIndices());
                ev->replaceAllUsesWith(b.CreateLoad(ev->getType(), at));
                dead_.push_back(ev);
                return;
            }
            for (llvm::Use& op : i->operands()) {   // anything else that reads a large value gets the value
                if (large(op->getType()) && !llvm::isa<llvm::PHINode>(i)) op.set(materialize(op.get(), i));
            }
            return;
        }
        // A large result.
        llvm::Type* t = i->getType();
        if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(i)) {
            if (ld->isVolatile() || ld->isAtomic()) return;
            llvm::AllocaInst* s = slot(t, "load");
            llvm::IRBuilder<> b(ld);
            copy(b, s, ld->getPointerOperand(), t);
            address_[ld] = s;
            dead_.push_back(ld);
            return;
        }
        if (auto* iv = llvm::dyn_cast<llvm::InsertValueInst>(i)) {
            llvm::AllocaInst* s = slot(t, "build");
            llvm::IRBuilder<> b(iv);
            llvm::Value* agg = iv->getAggregateOperand();
            if (!llvm::isa<llvm::UndefValue>(agg)) copy(b, s, address(agg, iv), t);
            llvm::Value* at = element(b, t, s, iv->getIndices());
            llvm::Value* x  = iv->getInsertedValueOperand();
            if (large(x->getType())) copy(b, at, address(x, iv), x->getType());
            else b.CreateStore(x, at);
            address_[iv] = s;
            dead_.push_back(iv);
            return;
        }
        if (auto* ev = llvm::dyn_cast<llvm::ExtractValueInst>(i)) {   // a large part of a large value
            llvm::IRBuilder<> b(ev);
            address_[ev] = element(b, ev->getAggregateOperand()->getType(), address(ev->getAggregateOperand(), ev), ev->getIndices());
            dead_.push_back(ev);
            return;
        }
        if (auto* p = llvm::dyn_cast<llvm::PHINode>(i)) {
            llvm::AllocaInst* own = slot(t, "phi");
            phi_slot_[p] = own;
            address_[p]  = own;
            phis_.push_back(p);
            dead_.push_back(p);
            return;
        }
        if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(i)) {
            llvm::IRBuilder<> b(sel);
            address_[sel] = b.CreateSelect(sel->getCondition(), address(sel->getTrueValue(), sel), address(sel->getFalseValue(), sel));
            dead_.push_back(sel);
            return;
        }
        if (auto* fr = llvm::dyn_cast<llvm::FreezeInst>(i)) {
            address_[fr] = address(fr->getOperand(0), fr);
            dead_.push_back(fr);
            return;
        }
        // Other instructions keep their large result; its users store it into a slot.
    }

    void lower_call(llvm::CallInst* call) {
        llvm::FunctionType* ft = call->getFunctionType();
        llvm::Function*     callee = call->getCalledFunction();
        if (callee && callee->isIntrinsic()) return;
        if (auto it = replaced_.find(callee); it != replaced_.end()) callee = it->second;
        bool rewritten = callee ? has_sret_.count(callee) || param_addresses_.count(callee) : changes(ft);
        if (!rewritten || !changes(ft)) {
            for (llvm::Use& op : call->args()) {
                if (large(op->getType())) op.set(materialize(op.get(), call));
            }
            return;
        }
        llvm::IRBuilder<>         b(call);
        std::vector<llvm::Value*> args;
        llvm::AllocaInst*         result = nullptr;
        if (large(ft->getReturnType())) {
            result = slot(ft->getReturnType(), "result");
            args.push_back(result);
        }
        for (llvm::Use& op : call->args()) args.push_back(large(op->getType()) ? address(op.get(), call) : op.get());
        llvm::Value*    target = callee ? static_cast<llvm::Value*>(callee) : call->getCalledOperand();
        llvm::CallInst* nc     = b.CreateCall(lowered_type(ft), target, args);
        nc->setCallingConv(call->getCallingConv());
        nc->setTailCallKind(llvm::CallInst::TCK_None);
        nc->setDebugLoc(call->getDebugLoc());
        nc->setAttributes(lowered_attributes(call->getAttributes(), ft));
        if (result) {
            nc->addParamAttr(0, llvm::Attribute::getWithStructRetType(m_.getContext(), ft->getReturnType()));
            address_[call] = result;
        } else if (!call->getType()->isVoidTy()) {
            call->replaceAllUsesWith(nc);
        }
        dead_.push_back(call);
    }
};

} // namespace

void lower_large_aggregates(llvm::Module& m, std::uint64_t limit_bytes) { Lowering(m, limit_bytes).run(); }

} // namespace edax
