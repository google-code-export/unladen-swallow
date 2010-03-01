#ifndef UTIL_RUNTIMEFEEDBACK_FWD_H
#define UTIL_RUNTIMEFEEDBACK_FWD_H

#ifdef __cplusplus
extern "C" {
#endif

struct PyFeedbackMap;

struct PyFeedbackMapFuncs {
    struct PyFeedbackMap *(*make)(void);
    void (*del)(struct PyFeedbackMap *);
    void (*clear)(struct PyFeedbackMap *);
};

PyAPI_DATA(struct PyFeedbackMapFuncs) _PyFeedbackMap;

/* These are the counters used for feedback in the JUMP_IF opcodes.
 * The number of boolean inputs can be computed as (PY_FDO_JUMP_TRUE +
 * PY_FDO_JUMP_FALSE - PY_FDO_JUMP_NON_BOOLEAN). */
enum { PY_FDO_JUMP_TRUE = 0, PY_FDO_JUMP_FALSE, PY_FDO_JUMP_NON_BOOLEAN };

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* UTIL_RUNTIMEFEEDBACK_FWD_H */
