
#ifndef Py_INTRCHECK_H
#define Py_INTRCHECK_H
#ifdef __cplusplus
extern "C" {
#endif

PyAPI_FUNC(int) PyOS_InterruptOccurred(void);
PyAPI_FUNC(void) PyOS_InitInterrupts(void);
PyAPI_FUNC(void) PyOS_BeforeExec(void);
PyAPI_FUNC(void) PyOS_BeforeFork(void);
PyAPI_FUNC(void) PyOS_AfterFork(int is_child);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTRCHECK_H */
