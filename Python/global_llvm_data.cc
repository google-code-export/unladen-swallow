/* Note: this file is not compiled if configured with --without-llvm. */
#define DEBUG_TYPE "pygloballlvmdata"
#include "Python.h"

#include "osdefs.h"
#undef MAXPATHLEN  /* Conflicts with definition in LLVM's config.h */
#include "Python/global_llvm_data.h"
#include "Util/ConstantMirror.h"
#include "Util/DeadGlobalElim.h"
#include "Util/DiffConstants.h"
#include "Util/PyAliasAnalysis.h"
#include "Util/SingleFunctionInliner.h"
#include "Util/Stats.h"
#include "_llvmfunctionobject.h"

#include "llvm/Analysis/DebugInfo.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/CallingConv.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/ExecutionEngine/JIT.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/Function.h"
#include "llvm/GlobalVariable.h"
#include "llvm/Module.h"
#include "llvm/ModuleProvider.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/ValueHandle.h"
#include "llvm/System/Path.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetSelect.h"
#include "llvm/Transforms/Scalar.h"

using llvm::Constant;
using llvm::ExecutionEngine;
using llvm::FunctionPassManager;
using llvm::GlobalVariable;
using llvm::Module;
using llvm::StringRef;
using llvm::dyn_cast;

PyGlobalLlvmData *
PyGlobalLlvmData_New()
{
    return new PyGlobalLlvmData;
}

void
PyGlobalLlvmData_Clear(PyGlobalLlvmData *)
{
    // So far, do nothing.
}

void
PyGlobalLlvmData_Free(PyGlobalLlvmData * global_data)
{
    delete global_data;
}

PyGlobalLlvmData *
PyGlobalLlvmData::Get()
{
    return PyThreadState_GET()->interp->global_llvm_data;
}

#define STRINGIFY(X) STRINGIFY2(X)
#define STRINGIFY2(X) #X
// The basename of the bitcode file holding the standard library.
#ifdef MS_WINDOWS
#ifdef Py_DEBUG
#define LIBPYTHON_BC "python" STRINGIFY(PY_MAJOR_VERSION) \
    STRINGIFY(PY_MINOR_VERSION) "_d.bc"
#else
#define LIBPYTHON_BC "python" STRINGIFY(PY_MAJOR_VERSION) \
    STRINGIFY(PY_MINOR_VERSION) ".bc"
#endif
#else
#define LIBPYTHON_BC "libpython" STRINGIFY(PY_MAJOR_VERSION) "." \
    STRINGIFY(PY_MINOR_VERSION) ".bc"
#endif

// Searches for the bitcode file holding the Python standard library.
// If one is found, returns its contents in a MemoryBuffer.  If not,
// dies with a fatal error.
static llvm::MemoryBuffer *
find_stdlib_bc()
{
    llvm::sys::Path path;
    llvm::SmallVector<StringRef, 8> sys_path;
    const char delim[] = { DELIM, '\0' };
    StringRef(Py_GetPath()).split(sys_path, delim);
    for (ssize_t i = 0, size = sys_path.size(); i < size; ++i) {
        StringRef elem = sys_path[i];
        path = elem;
        path.appendComponent(LIBPYTHON_BC);
        llvm::MemoryBuffer *stdlib_file =
            llvm::MemoryBuffer::getFile(path.str(), NULL);
        if (stdlib_file != NULL) {
            return stdlib_file;
        }
    }
    Py_FatalError("Could not find " LIBPYTHON_BC " on sys.path");
    return NULL;
}

PyGlobalLlvmData::PyGlobalLlvmData()
    : optimizations_(3, (FunctionPassManager*)NULL),
      num_globals_after_last_gc_(0)
{
    std::string error;
    llvm::MemoryBuffer *stdlib_file = find_stdlib_bc();
    this->module_provider_ =
      llvm::getBitcodeModuleProvider(stdlib_file, this->context(), &error);
    if (this->module_provider_ == NULL) {
      Py_FatalError(error.c_str());
    }
    // TODO(jyasskin): Change this to getModule once we avoid the crash
    // in JIT::runJITOnFunctionUnlocked.  http://llvm.org/PR5735
    this->module_ = this->module_provider_->materializeModule(&error);
    if (this->module_ == NULL) {
        Py_FatalError(error.c_str());
    }

    if (Py_GenerateDebugInfoFlag) {
      this->debug_info_.reset(new llvm::DIFactory(*module_));
    }

    llvm::InitializeNativeTarget();
    engine_ = llvm::ExecutionEngine::create(
        module_provider_,
        // Don't force the interpreter (use JIT if possible).
        false,
        &error,
        // JIT slowly, to produce better machine code.  TODO: We'll
        // almost certainly want to make this configurable per
        // function.
        llvm::CodeGenOpt::Default,
        // Allocate GlobalVariables separately from code.
        false);
    if (engine_ == NULL) {
        Py_FatalError(error.c_str());
    }

    engine_->RegisterJITEventListener(llvm::createOProfileJITEventListener());

    // When we ask to JIT a function, we should also JIT other
    // functions that function depends on.  This lets us JIT in a
    // background thread to avoid blocking the main thread during
    // codegen, and (once the GIL is gone) JITting lazily is
    // thread-unsafe anyway.
    engine_->DisableLazyCompilation();

    this->constant_mirror_.reset(new PyConstantMirror(this));

    this->InstallInitialModule();

    this->InitializeOptimizations();
    this->gc_.add(PyCreateDeadGlobalElimPass(&this->bitcode_gvs_));
}

template<typename Iterator>
static void insert_gvs(
    llvm::DenseSet<llvm::AssertingVH<const llvm::GlobalValue> > &set,
    Iterator first, Iterator last) {
    for (; first != last; ++first) {
        set.insert(&*first);
    }
}

void
PyGlobalLlvmData::InstallInitialModule()
{
    insert_gvs(bitcode_gvs_, this->module_->begin(), this->module_->end());
    insert_gvs(bitcode_gvs_, this->module_->global_begin(),
               this->module_->global_end());
    insert_gvs(bitcode_gvs_, this->module_->alias_begin(),
               this->module_->alias_end());

    for (llvm::Module::iterator it = this->module_->begin();
         it != this->module_->end(); ++it) {
        if (it->getName().startswith("_PyLlvm_Fast")) {
            it->setCallingConv(llvm::CallingConv::Fast);
        }
    }

    this->GatherAddresses();
}

// The idea here is a bit subtle. When we have an available_externally
// GlobalVariable (GV1), we have both its initialization, and the address of
// that initialization available through dlsym().  If the initializer contains
// any pointers, GV1.getInitializer() will hold a symbolic reference to another
// GlobalValue (GV2), but the matching memory will hold a real pointer to an
// ABI-compatible address inside the main executable.  If we add a
// global-mapping to the ExecutionEngine, then it won't need to emit GV2 itself,
// saving both memory and time.  If GV2 is an initialized GlobalVariable, we can
// repeat the above process on its initializer.
//
// This could break if someone changes the in-memory pointer before
// GatherAddresses has a chance to match it to the right @global.  To avoid
// that, we run this to completion just after startup, before user-defined code
// has a chance to run.  We could do this more lazily by checking the
// PyAliasAnalysis about whether each pointer was constant, and only match up
// constant pointers.
void
PyGlobalLlvmData::GatherAddresses()
{
    std::vector<const GlobalVariable*> worklist;
    llvm::SmallPtrSet<const GlobalVariable*, 32> visited;
    // Fill the ExecutionEngine with the addresses of known global variables.
    for (Module::global_iterator it = this->module_->global_begin();
         it != this->module_->global_end(); ++it) {
        if (it->hasExternalLinkage() && it->isDeclaration()) {
            void *addr = this->engine_->getOrEmitGlobalVariable(it);
            (void)addr;
            DEBUG(llvm::errs() << "Mapped declaration of " << it->getName()
                  << " to " << addr << "\n");
        } else if (it->hasAvailableExternallyLinkage()) {
            void *addr = this->engine_->getOrEmitGlobalVariable(it);
            (void)addr;
            worklist.push_back(it);
            DEBUG(llvm::errs() << "Mapped available_externally "
                  << it->getName() << " to " << addr << "\n");
        }
    }

    while (!worklist.empty()) {
        const GlobalVariable *known_global = worklist.back();
        worklist.pop_back();
        if (!visited.insert(known_global))
            continue;
        void *addr = this->engine_->getPointerToGlobalIfAvailable(known_global);
        DEBUG(llvm::errs() << "Visiting " << known_global->getName() << " at "
              << addr << "\n");
        assert(addr != NULL &&
               "Objects should only be on worklist after they get a value.");
        assert(known_global->hasInitializer() &&
               "Anything on the worklist must have an initializer");
        Constant *known_initializer = known_global->getInitializer();
        Constant *memory_value = this->constant_mirror_->ConstantFromMemory(
            known_initializer->getType(), addr);

        struct AddressExtractor : public PyDiffVisitor {
            AddressExtractor(std::vector<const GlobalVariable*> &worklist,
                             ExecutionEngine &engine)
                : worklist_(worklist), engine_(engine) {}
            void *ReadAddress(const llvm::Constant *C) {
                if (const llvm::ConstantExpr *CE =
                    dyn_cast<llvm::ConstantExpr>(C)) {
                    if (CE->getOpcode() == llvm::Instruction::IntToPtr) {
                        if (const llvm::ConstantInt *val =
                            dyn_cast<llvm::ConstantInt>(CE->getOperand(0))) {
                            const llvm::APInt &ival = val->getValue();
                            if (ival.getActiveBits() > sizeof(void*) * 8)
                                return NULL;
                            return (void*)ival.getZExtValue();
                        }
                    }
                }
                return NULL;
            }
            virtual void Visit(const llvm::Constant *C1,
                               const llvm::Constant *C2) {
                if (void *addr = ReadAddress(C1)) {
                    if (const llvm::GlobalValue *GVal =
                        dyn_cast<llvm::GlobalValue>(C2)) {
                        if (void *known_addr =
                            this->engine_.getPointerToGlobalIfAvailable(GVal)) {
                            (void)known_addr;
                            assert(addr == known_addr &&
                                   "Found inconsistent addresses for"
                                   " same global");
                        } else {
                            this->engine_.addGlobalMapping(GVal, addr);
                            DEBUG(llvm::errs() << "Mapped " << GVal->getName()
                                  << " to " << addr);
                            if (const GlobalVariable *GVar =
                                dyn_cast<GlobalVariable>(GVal)) {
                                if (GVar->hasInitializer()) {
                                    this->worklist_.push_back(GVar);
                                    DEBUG(llvm::errs()
                                          << " and added to worklist");
                                }
                            }
                            DEBUG(llvm::errs() << "\n");
                        }
                    }
                }
            }
            std::vector<const GlobalVariable*> &worklist_;
            ExecutionEngine &engine_;
        };

        AddressExtractor extractor(worklist, *this->engine_);
        PyDiffConstants(memory_value, known_initializer, extractor);
    }
}

void
PyGlobalLlvmData::InitializeOptimizations()
{
    optimizations_[0] = new FunctionPassManager(this->module_provider_);

    FunctionPassManager *quick =
        new FunctionPassManager(this->module_provider_);
    optimizations_[1] = quick;
    quick->add(new llvm::TargetData(*engine_->getTargetData()));
    quick->add(llvm::createPromoteMemoryToRegisterPass());
    quick->add(llvm::createInstructionCombiningPass());
    quick->add(llvm::createCFGSimplificationPass());
    quick->add(llvm::createVerifierPass());

    // This is the default optimization used by the JIT.
    FunctionPassManager *O2 =
        new FunctionPassManager(this->module_provider_);
    optimizations_[2] = O2;
    O2->add(new llvm::TargetData(*engine_->getTargetData()));
    O2->add(llvm::createCFGSimplificationPass());
    O2->add(PyCreateSingleFunctionInliningPass(this->module_provider_));
    O2->add(llvm::createJumpThreadingPass());
    O2->add(llvm::createPromoteMemoryToRegisterPass());
    O2->add(llvm::createInstructionCombiningPass());
    O2->add(llvm::createCFGSimplificationPass());
    O2->add(llvm::createScalarReplAggregatesPass());
    O2->add(CreatePyAliasAnalysis(*this));
    O2->add(llvm::createLICMPass());
    O2->add(llvm::createJumpThreadingPass());
    O2->add(CreatePyAliasAnalysis(*this));
    O2->add(llvm::createGVNPass());
    O2->add(llvm::createSCCPPass());
    O2->add(llvm::createAggressiveDCEPass());
    O2->add(llvm::createCFGSimplificationPass());
    O2->add(llvm::createVerifierPass());
}

PyGlobalLlvmData::~PyGlobalLlvmData()
{
    this->bitcode_gvs_.clear();  // Stop asserting values aren't destroyed.
    this->constant_mirror_->python_shutting_down_ = true;
    for (size_t i = 0; i < this->optimizations_.size(); ++i) {
        delete this->optimizations_[i];
    }
    delete this->engine_;
}

int
PyGlobalLlvmData::Optimize(llvm::Function &f, int level)
{
    if (level < 0 || (size_t)level >= this->optimizations_.size())
        return -1;
    FunctionPassManager *opts_pm = this->optimizations_[level];
    assert(opts_pm != NULL && "Optimization was NULL");
    assert(this->module_ == f.getParent() &&
           "We assume that all functions belong to the same module.");
    opts_pm->run(f);
    return 0;
}

int
PyGlobalLlvmData_Optimize(struct PyGlobalLlvmData *global_data,
                          _LlvmFunction *llvm_function,
                          int level)
{
    return _LlvmFunction_Optimize(global_data, llvm_function, level);
}

#ifdef Py_WITH_INSTRUMENTATION
// Collect statistics about the time it takes to collect unused globals.
class GlobalGCTimes : public DataVectorStats<int64_t> {
public:
    GlobalGCTimes()
        : DataVectorStats<int64_t>("Time for a globaldce run in ns") {}
};

class GlobalGCCollected : public DataVectorStats<int> {
public:
    GlobalGCCollected()
        : DataVectorStats<int>("Number of globals collected by globaldce") {}
};

static llvm::ManagedStatic<GlobalGCTimes> global_gc_times;
static llvm::ManagedStatic<GlobalGCCollected> global_gc_collected;

#endif  // Py_WITH_INSTRUMENTATION

void
PyGlobalLlvmData::MaybeCollectUnusedGlobals()
{
    unsigned num_globals = this->module_->getGlobalList().size() +
        this->module_->getFunctionList().size();
    // Don't incur the cost of collecting globals if there are too few
    // of them, or if doing so now would cost a quadratic amount of
    // time as we allocate more long-lived globals.  The thresholds
    // here are just guesses, not tuned numbers.
    if (num_globals < 20 ||
        num_globals < (this->num_globals_after_last_gc_ +
                       (this->num_globals_after_last_gc_ >> 2)))
        return;
    this->CollectUnusedGlobals();
}

void
PyGlobalLlvmData::CollectUnusedGlobals()
{
#if Py_WITH_INSTRUMENTATION
    unsigned num_globals = this->module_->getGlobalList().size() +
        this->module_->getFunctionList().size();
#endif
    {
#if Py_WITH_INSTRUMENTATION
        Timer timer(*global_gc_times);
#endif
        this->gc_.run(*this->module_);
    }
    this->num_globals_after_last_gc_ = this->module_->getGlobalList().size() +
        this->module_->getFunctionList().size();
#if Py_WITH_INSTRUMENTATION
    global_gc_collected->RecordDataPoint(
        num_globals - num_globals_after_last_gc_);
#endif
}

void
PyGlobalLlvmData_CollectUnusedGlobals(struct PyGlobalLlvmData *global_data)
{
    global_data->CollectUnusedGlobals();
}

llvm::Value *
PyGlobalLlvmData::GetGlobalStringPtr(const std::string &value)
{
    // Use operator[] because we want to insert a new value if one
    // wasn't already present.
    llvm::WeakVH& the_string = this->constant_strings_[value];
    if (the_string == NULL) {
        llvm::Constant *str_const = llvm::ConstantArray::get(this->context(),
                                                             value, true);
        the_string = new llvm::GlobalVariable(
            *this->module_,
            str_const->getType(),
            true,  // Is constant.
            llvm::GlobalValue::InternalLinkage,
            str_const,
            value,  // Name.
            false);  // Not thread-local.
    }

    // the_string is a [(value->size()+1) x i8]*. C functions
    // expecting string constants instead expect an i8* pointing to
    // the first element.  We use GEP instead of bitcasting to make
    // type safety more obvious.
    const llvm::Type *int64_type = llvm::Type::getInt64Ty(this->context());
    llvm::Constant *indices[] = {
        llvm::ConstantInt::get(int64_type, 0),
        llvm::ConstantInt::get(int64_type, 0)
    };
    return llvm::ConstantExpr::getGetElementPtr(
        llvm::cast<llvm::Constant>(the_string), indices, 2);
}

int
_PyLlvm_Init()
{
    if (PyType_Ready(&PyLlvmFunction_Type) < 0)
        return 0;

    llvm::cl::ParseEnvironmentOptions("python", "PYTHONLLVMFLAGS", "", true);

    return 1;
}

void
_PyLlvm_Fini()
{
    llvm::llvm_shutdown();
}

int
PyLlvm_SetDebug(int on)
{
#ifdef NDEBUG
    if (on)
        return 0;
#else
    llvm::DebugFlag = on;
#endif
    return 1;
}
