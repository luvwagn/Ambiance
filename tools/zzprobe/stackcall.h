/*
 * stack_call: run f(arg) on a stack of its own (exec's StackSwap), so
 * deep work does not depend on the caller's stack. A Shell gives 4 KB by
 * default; a datatype runs on whatever its caller has. A stack that runs
 * over corrupts the task's saved exception frame when exec switches
 * away from it: "8000 000E" (Format Error) when it is switched back.
 * AmigaMCE, MIT.
 *
 * Returns f's result. If the stack cannot be had, f runs where it is.
 * Everything f needs goes through arg: the caller's locals are not on
 * the new stack, but their addresses stay valid.
 */
#ifndef STACKCALL_H
#define STACKCALL_H

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <proto/exec.h>

static ULONG stack_call(ULONG size, ULONG (*f)(void *), void *arg)
{
    struct StackSwapStruct ss;
    UBYTE *mem = AllocVec(size, MEMF_ANY);
    if (!mem)
        return f(arg);
    ss.stk_Lower = mem;
    ss.stk_Upper = (ULONG)(mem + size);
    ss.stk_Pointer = (APTR)ss.stk_Upper;
    {
        /* callee-saved registers carry everything across both swaps:
         * StackSwap keeps all but d0/d1/a0/a1, and so does f (C) */
        register struct StackSwapStruct *rs __asm("a2") = &ss;
        register ULONG (*rf)(void *) __asm("a3") = f;
        register void *rv __asm("d2") = arg;
        __asm volatile(
            "move.l %%a6,-(%%sp)\n\t"       /* on the caller's stack */
            "move.l 4,%%a6\n\t"
            "move.l %0,%%a0\n\t"
            "jsr -732(%%a6)\n\t"            /* StackSwap: to the new one */
            "move.l %2,-(%%sp)\n\t"
            "jsr (%1)\n\t"
            "addq.l #4,%%sp\n\t"
            "move.l %%d0,%2\n\t"
            "move.l 4,%%a6\n\t"
            "move.l %0,%%a0\n\t"
            "jsr -732(%%a6)\n\t"            /* and back */
            "move.l (%%sp)+,%%a6"
            : "+a"(rs), "+a"(rf), "+d"(rv)
            :
            : "d0", "d1", "a0", "a1", "cc", "memory");
        FreeVec(mem);
        return (ULONG)rv;
    }
}

#endif
