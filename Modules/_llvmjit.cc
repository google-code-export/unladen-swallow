#include "Python.h"

#include "Python/global_llvm_data.h"
#include "llvm_compile.h"
#include "llvm_thread.h"
#include "_llvmfunctionobject.h"
#include "opcode.h"
#include "llvm/Function.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "Util/RuntimeFeedback.h"
#include "Util/Stats.h"
#include "Util/EventTimer.h"

#include <set>

using llvm::errs;


#ifdef Py_WITH_INSTRUMENTATION
static std::string
get_code_name(PyCodeObject *code)
{
    std::string result;
    llvm::raw_string_ostream wrapper(result);

    wrapper << PyString_AsString(code->co_filename)
            << ":" << code->co_firstlineno << " "
            << "(" << PyString_AsString(code->co_name) << ")";
    wrapper.flush();
    return result;
}

class EvalBlockedTimes : public DataVectorStats<int64_t> {
public:
    EvalBlockedTimes()
        : DataVectorStats<int64_t>("Time blocked in eval loop in ns") {}
};

static llvm::ManagedStatic<McCompilationTimes> eval_blocked_times;


class FeedbackMapCounter {
public:
    ~FeedbackMapCounter() {
        errs() << "\nFeedback maps created:\n";
        errs() << "N: " << this->counter_ << "\n";
    }

    void IncCounter() {
        this->counter_++;
    }

private:
    unsigned counter_;
};

static llvm::ManagedStatic<FeedbackMapCounter> feedback_map_counter;


class HotnessTracker {
    // llvm::DenseSet or llvm::SmallPtrSet may be better, but as of this
    // writing, they don't seem to work with std::vector.
    std::set<PyCodeObject*> hot_code_;
public:
    ~HotnessTracker();

    void AddHotCode(PyCodeObject *code_obj) {
        // This will prevent the code object from ever being
        // deleted.
        Py_INCREF(code_obj);
        this->hot_code_.insert(code_obj);
    }
};

static bool
compare_hotness(const PyCodeObject *first, const PyCodeObject *second)
{
    return first->co_hotness > second->co_hotness;
}

HotnessTracker::~HotnessTracker()
{
    errs() << "\nCode objects deemed hot:\n";
    errs() << "N: " << this->hot_code_.size() << "\n";
    errs() << "Function -> hotness score:\n";
    std::vector<PyCodeObject*> to_sort(this->hot_code_.begin(),
                                       this->hot_code_.end());
    std::sort(to_sort.begin(), to_sort.end(), compare_hotness);
    for (std::vector<PyCodeObject*>::iterator co = to_sort.begin();
         co != to_sort.end(); ++co) {
        errs() << get_code_name(*co)
               << " -> " << (*co)->co_hotness << "\n";
    }
}

static llvm::ManagedStatic<HotnessTracker> hot_code;


// Keep track of which functions failed fatal guards, but kept being called.
// This can help gauge the efficacy of optimizations that involve fatal guards.
class FatalBailTracker {
public:
    ~FatalBailTracker() {
        errs() << "\nCode objects that failed fatal guards:\n";
        errs() << "\tfile:line (funcname) bail hotness"
               << " -> final hotness\n";

        for (TrackerData::const_iterator it = this->code_.begin();
             it != this->code_.end(); ++it) {
            PyCodeObject *code = it->first;
            if (code->co_hotness == it->second)
                continue;
            errs() << "\t"
                   << get_code_name(code)
                   << "\t" << it->second << " -> "
                   << code->co_hotness << "\n";
        }
    }

    void RecordFatalBail(PyCodeObject *code) {
        Py_INCREF(code);
        this->code_.push_back(std::make_pair(code, code->co_hotness));
    }

private:
    // Keep a list of (code object, hotness) where hotness is the
    // value of co_hotness when RecordFatalBail() was called. This is
    // used to hide code objects whose machine code functions are
    // invalidated during shutdown because their module dict has gone away;
    // these code objects are uninteresting for our analysis.
    typedef std::pair<PyCodeObject *, long> DataPoint;
    typedef std::vector<DataPoint> TrackerData;

    TrackerData code_;
};

static llvm::ManagedStatic<FatalBailTracker> fatal_bail_tracker;


// Collect stats on how many watchers the globals/builtins dicts acculumate.
// This currently records how many watchers the dict had when it changed, ie,
// how many watchers it had to notify.
class WatcherCountStats : public DataVectorStats<size_t> {
public:
    WatcherCountStats() :
        DataVectorStats<size_t>("Number of watchers accumulated") {};
};

static llvm::ManagedStatic<WatcherCountStats> watcher_count_stats;


class BailCountStats {
public:
    BailCountStats() : total_(0), trace_on_entry_(0), line_trace_(0),
                       backedge_trace_(0), call_profile_(0),
                       fatal_guard_fail_(0), guard_fail_(0) {};

    ~BailCountStats() {
        errs() << "\nBailed to the interpreter " << this->total_
               << " times:\n";
        errs() << "TRACE_ON_ENTRY: " << this->trace_on_entry_ << "\n";
        errs() << "LINE_TRACE: " << this->line_trace_ << "\n";
        errs() << "BACKEDGE_TRACE:" << this->backedge_trace_ << "\n";
        errs() << "CALL_PROFILE: " << this->call_profile_ << "\n";
        errs() << "FATAL_GUARD_FAIL: " << this->fatal_guard_fail_
               << "\n";
        errs() << "GUARD_FAIL: " << this->guard_fail_ << "\n";

        errs() << "\n" << this->bail_site_freq_.size()
               << " bail sites:\n";
        for (BailData::iterator i = this->bail_site_freq_.begin(),
             end = this->bail_site_freq_.end(); i != end; ++i) {
            errs() << "    " << i->getKey() << " bailed "
                   << i->getValue() << " times\n";
        }

        errs() << "\n" << this->guard_bail_site_freq_.size()
               << " guard bail sites:\n";
        for (BailData::iterator i = this->guard_bail_site_freq_.begin(),
             end = this->guard_bail_site_freq_.end(); i != end; ++i) {
            errs() << "    " << i->getKey() << " bailed "
                   << i->getValue() << " times\n";
        }

    }

    void RecordBail(PyFrameObject *frame, _PyFrameBailReason bail_reason) {
        ++this->total_;

        std::string record;
        llvm::raw_string_ostream wrapper(record);
        wrapper << PyString_AsString(frame->f_code->co_filename) << ":";
        wrapper << frame->f_code->co_firstlineno << ":";
        wrapper << PyString_AsString(frame->f_code->co_name) << ":";
        // See the comment in PyEval_EvalFrame about how f->f_lasti is
        // initialized.
        wrapper << frame->f_lasti + 1;
        wrapper.flush();

        BailData::value_type &entry =
                this->bail_site_freq_.GetOrCreateValue(record, 0);
        entry.setValue(entry.getValue() + 1);

#define BAIL_CASE(name, field) \
        case name: \
            ++this->field; \
            break

        switch (bail_reason) {
            BAIL_CASE(_PYFRAME_TRACE_ON_ENTRY, trace_on_entry_);
            BAIL_CASE(_PYFRAME_LINE_TRACE, line_trace_);
            BAIL_CASE(_PYFRAME_BACKEDGE_TRACE, backedge_trace_);
            BAIL_CASE(_PYFRAME_CALL_PROFILE, call_profile_);
            BAIL_CASE(_PYFRAME_FATAL_GUARD_FAIL, fatal_guard_fail_);
            BAIL_CASE(_PYFRAME_GUARD_FAIL, guard_fail_);
            default:
                abort();   // Unknown bail reason.
        }
#undef BAIL_CASE

        if (bail_reason != _PYFRAME_GUARD_FAIL)
            return;

        wrapper << ":";

#define GUARD_CASE(name) \
        case name: \
            wrapper << #name; \
            break

        switch (frame->f_guard_type) {
            GUARD_CASE(_PYGUARD_DEFAULT);
            GUARD_CASE(_PYGUARD_BINOP);
            GUARD_CASE(_PYGUARD_ATTR);
            GUARD_CASE(_PYGUARD_CFUNC);
            GUARD_CASE(_PYGUARD_BRANCH);
            GUARD_CASE(_PYGUARD_STORE_SUBSCR);
            default:
                wrapper << ((int)frame->f_guard_type);
        }
#undef GUARD_CASE

        wrapper.flush();

        BailData::value_type &g_entry =
                this->guard_bail_site_freq_.GetOrCreateValue(record, 0);
        g_entry.setValue(g_entry.getValue() + 1);
    }

private:
    typedef llvm::StringMap<unsigned> BailData;
    BailData bail_site_freq_;
    BailData guard_bail_site_freq_;

    long total_;
    long trace_on_entry_;
    long line_trace_;
    long backedge_trace_;
    long call_profile_;
    long fatal_guard_fail_;
    long guard_fail_;
};

static llvm::ManagedStatic<BailCountStats> bail_count_stats;


// C wrappers for instrumentation functions.
extern "C" {
    static void feedback_map_inc_counter() {
        feedback_map_counter->IncCounter();
    }

    static void add_hot_code(PyCodeObject *co) {
        hot_code->AddHotCode(co);
    }

    static void record_fatal_bail(PyCodeObject *code) {
        fatal_bail_tracker->RecordFatalBail(code);
    }

    static void record_watcher_count(size_t watcher_count) {
        watcher_count_stats->RecordDataPoint(watcher_count);
    }

    static void record_bail(PyFrameObject *frame,
                            _PyFrameBailReason bail_reason) {
        bail_count_stats->RecordBail(frame, bail_reason);
    }
};
#endif  // Py_WITH_INSTRUMENTATION

static void
assert_expected_opcode(PyCodeObject *co, int expected_opcode, int opcode_index)
{
#ifndef NDEBUG
    unsigned char actual_opcode =
            PyString_AS_STRING(co->co_code)[opcode_index];
    assert((actual_opcode == expected_opcode ||
            actual_opcode == EXTENDED_ARG) &&
           "Mismatch between feedback and opcode array.");
#endif  // NDEBUG
}

extern "C" {
// Record data for use in generating optimized machine code.
static void
inc_feedback_counter(PyCodeObject *co, int expected_opcode, int opcode_index,
                     int counter_id)
{
    assert_expected_opcode(co, expected_opcode, opcode_index);
    PyRuntimeFeedback &feedback =
            co->co_runtime_feedback->GetOrCreateFeedbackEntry(
                    opcode_index, 0);
    feedback.IncCounter(counter_id);
}

// Records func into the feedback array.
static void
record_func(PyCodeObject *co, int expected_opcode, int opcode_index,
            int arg_index, PyObject *func)
{
    assert_expected_opcode(co, expected_opcode, opcode_index);
    PyRuntimeFeedback &feedback =
            co->co_runtime_feedback->GetOrCreateFeedbackEntry(
                    opcode_index, arg_index);
    feedback.AddFuncSeen(func);
}

// Records obj into the feedback array. Only use this on long-lived objects,
// since the feedback system will keep any object live forever.
static void
record_object(PyCodeObject *co, int expected_opcode, int opcode_index,
              int arg_index, PyObject *obj)
{
    assert_expected_opcode(co, expected_opcode, opcode_index);
    PyRuntimeFeedback &feedback =
            co->co_runtime_feedback->GetOrCreateFeedbackEntry(
                    opcode_index, arg_index);
    feedback.AddObjectSeen(obj);
}

// Records the type of obj into the feedback array.
static void
record_type(PyCodeObject *co, int expected_opcode, int opcode_index,
            int arg_index, PyObject *obj)
{
    if (obj == NULL)
        return;
    PyObject *type = (PyObject *)Py_TYPE(obj);
    record_object(co, expected_opcode, opcode_index, arg_index, type);
}

// Backend of eval's maybe_compile; see there for the decision making about
// whether or not to compile a piece of code.
static Py_CompileResult
jit_compile(PyCodeObject *co, PyFrameObject *f, int opt_level,
            Py_ShouldBlock block)
{
    if (f != NULL && _PyCode_WatchGlobals(co, f->f_globals, f->f_builtins)) {
        return PY_COMPILE_ERROR;
    }

    PY_LOG_TSC_EVENT(EVAL_COMPILE_START);
#if Py_WITH_INSTRUMENTATION
    Timer timer(*eval_blocked_times);
#endif
    Py_CompileResult r;
    r = PyLlvm_JitInBackground(co, opt_level, block);
    PY_LOG_TSC_EVENT(EVAL_COMPILE_END);

    return r;
}

static void
global_data_clear(PyGlobalLlvmData *)
{
    // So far, do nothing.
}

static void
global_data_free(PyGlobalLlvmData * global_data)
{
    delete global_data;
}

static PyGlobalLlvmData *
global_data_get()
{
    return PyGlobalLlvmData::Get();
}

static int
global_data_optimize(_LlvmFunction *llvm_function, int level)
{
    return _LlvmFunction_Optimize(PyGlobalLlvmData::Get(), llvm_function, level);
}

static void
global_data_collect_unused_globals(struct PyGlobalLlvmData *global_data)
{
    global_data->CollectUnusedGlobals();
}

static int initialized;
static int
llvm_init()
{
    if (PyType_Ready(&PyLlvmFunction_Type) < 0)
        return 0;

    PyGlobalLlvmData::Init();

    llvm::cl::ParseEnvironmentOptions("python", "PYTHONLLVMFLAGS", "", true);

    initialized = 1;

    return 1;
}

static void
llvm_fini()
{
    if (initialized) {
        llvm::llvm_shutdown();
        initialized = 0;
    }
}

static int
set_debug(int on)
{
#ifdef NDEBUG
    if (on)
        return 0;
#else
    llvm::DebugFlag = on;
#endif
    return 1;
}

static struct PyLlvmFuncs llvmfuncs = {
    1,  /* loaded */

    &inc_feedback_counter,
    &record_func,
    &record_object,
    &record_type,

    &jit_compile,

    &global_data_get,
    &global_data_clear,
    &global_data_free,
    &global_data_optimize,
    &global_data_collect_unused_globals,

    &llvm_fini,
    &set_debug,

    (PyObject*)&PyLlvmFunction_Type,
    &_PyLlvmFunction_FromCodeObject,
    &_LlvmFunction_Dealloc,

    &PyLlvm_StartCompilation,
    &PyLlvm_StopCompilation,
    &PyLlvm_PauseCompilation,
    &PyLlvm_UnpauseCompilation,
    &PyLlvm_ResetAfterFork,
    &PyLlvm_ApplyFinishedJobs,
    &PyLlvm_WaitForJit,

#ifdef Py_WITH_INSTRUMENTATION
    &feedback_map_inc_counter,
    &add_hot_code,
    &record_fatal_bail,
    &record_watcher_count,
    &record_bail,
#endif
};

static PyFeedbackMap *
PyFeedbackMap_New()
{
    return new PyFeedbackMap;
}

static void
PyFeedbackMap_Del(PyFeedbackMap *map)
{
    delete map;
}

static void
PyFeedbackMap_Clear(PyFeedbackMap *map)
{
    map->Clear();
}

static struct PyFeedbackMapFuncs feedbackmap = {
    &PyFeedbackMap_New,
    &PyFeedbackMap_Del,
    &PyFeedbackMap_Clear
};

PyMODINIT_FUNC
init_llvmjit(void)
{
    PyObject *module;

    // Create the module and add the functions.
    module = Py_InitModule3("_llvmjit", NULL, NULL);
    if (module == NULL)
        return;

    if (!llvm_init()) {
        PyErr_SetString(PyExc_ImportError, "Error while initializing LLVM\n");
        return;
    }

    _PyLlvmFuncs = llvmfuncs;
    _PyFeedbackMap = feedbackmap;

    // Start the background thread.
    // TODO(rnk): We should spin this up lazily instead of at LLVM load time.
    _PyLlvmFuncs.llvmthread_start();
}

}
