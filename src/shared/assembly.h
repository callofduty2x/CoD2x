
/*
__asm__ (
    "assembly_code"            // The actual assembly code
    : output_operands           // Output constraints
    : input_operands            // Input constraints
    : clobbers                  // Clobbered registers or flags
);

Common constraint types:
    r: Use a general-purpose register.
    m: Use memory (the variable's address in memory).
    i: Use an immediate value.
    =r: Writable register for output.
    +r: A register that's both an input and an output.

Clobbers specify which registers, memory, or flags are modified by the assembly code but are not part of the inputs or outputs. 
The compiler uses this information to avoid using those resources for other purposes.

If the assembly code modifies global memory (e.g., via pointers), add "memory" to the clobber list to tell the compiler that memory content has changed:

*/


/*
    Helper macros for inline assembly
*/

#define ASM(op, ...) ASM__##op(__VA_ARGS__)

#define ASM__push(arg) \
    __asm__ volatile("push %0\n" : : "m"(arg) : "memory")

#define ASM__mov(dest, src) \
    __asm__ volatile("movl %0, %%" dest "\n" : : "m"(src) : "memory", dest)

#define ASM__call(func) \
    __asm__ volatile("call *%0\n" : : "m"(func) : \
        "memory", "cc", \
        "eax", "ebx", "ecx", "edx", "esi", "edi", /*"ebp",*/ /*"esp",*/ \
        "st","st(1)","st(2)","st(3)", "st(4)","st(5)","st(6)","st(7)", \
        "xmm0","xmm1","xmm2","xmm3", "xmm4","xmm5","xmm6","xmm7")

#define ASM__add(dest, imm) \
    __asm__ volatile("addl %0, %%" dest "\n" : : "i"(imm) : "memory", "cc", dest)

#define ASM__add_esp(bytes) \
    __asm__ volatile("addl %0, %%esp\n" : : "i"(bytes) : "memory", "cc")

#define ASM__movr(dest, src) \
    __asm__ volatile("movl %%" src ", %0\n" : "=m"(dest) : : "memory", src)


/*
Macros to make inline function call easy.
Example:
ASM_CALL(RETURN_VOID, 0x00434460, 1, EAX(var1), ECX(var2), PUSH(var3));
*/
#define EAX(var) ASM__mov("eax", var)
#define EBX(var) ASM__mov("ebx", var)
#define ECX(var) ASM__mov("ecx", var)
#define EDX(var) ASM__mov("edx", var)
#define EDI(var) ASM__mov("edi", var)
#define ESI(var) ASM__mov("esi", var)
#define PUSH(var) ASM__push(var)
#define RETURN(var) ASM__movr(var, "eax")
#define RETURN_SHORT(var) __asm__ volatile("movw %%ax, %0\n" : "=m"(var) : : "memory", "cc")
#define RETURN_ST0_FLOAT(var) __asm__ volatile("fstps %0" : "=m"(var) : : "memory")
#define RETURN_ST0_DOUBLE(var) __asm__ volatile("fstpl %0" : "=m"(var) : : "memory")
#define RETURN_VOID

// Push 32-bit word from a struct by offset.
#define ASM_PUSH_OFFSET(ptr, offset)                         \
    ASM__push(*((unsigned int *)((char*)(ptr) + (offset))));

// Macros for pushing in reverse order (highest offset first).
#define ASM_PUSH_STRUCT_OF_SIZE_1(ptr)   \
    ASM_PUSH_OFFSET(ptr, 0)
#define ASM_PUSH_STRUCT_OF_SIZE_2(ptr)   \
    ASM_PUSH_OFFSET(ptr, 4)              \
    ASM_PUSH_STRUCT_OF_SIZE_1(ptr)
#define ASM_PUSH_STRUCT_OF_SIZE_3(ptr)   \
    ASM_PUSH_OFFSET(ptr, 8)              \
    ASM_PUSH_STRUCT_OF_SIZE_2(ptr)
#define ASM_PUSH_STRUCT_OF_SIZE_4(ptr)   \
    ASM_PUSH_OFFSET(ptr, 12)             \
    ASM_PUSH_STRUCT_OF_SIZE_3(ptr)
#define ASM_PUSH_STRUCT_OF_SIZE_5(ptr)   \
    ASM_PUSH_OFFSET(ptr, 16)             \
    ASM_PUSH_STRUCT_OF_SIZE_4(ptr)

#define ASM_PUSH_STRUCT_OF_SIZE(ptr, cnt)  ASM_PUSH_STRUCT_OF_SIZE_##cnt(ptr)

/**
 * Push specified number of 32bit fields of struct on the stack.
 * @param ptr Pointer to struct to push on the stack
 * @param cnt Number of 32-bit words to copy
 */
#define PUSH_STRUCT(ptr, cnt)  ASM_PUSH_STRUCT_OF_SIZE(&(ptr), cnt)


#define ASM_CALL_13(ret, func, stack, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12 )  \
    const void* original_func = (void*)func; \
    a12; \
    a11; \
    a10; \
    a9; \
    a8; \
    a7; \
    a6; \
    a5; \
    a4; \
    a3; \
    a2; \
    a1; \
    ASM__call(original_func); \
    ret; \
    ASM__add_esp(stack * 4);

#define ASM_CALL_0(ret, func)  \
    const void* original_func = (void*)func; \
    ASM__call(original_func); \
    ret;

#define ASM_CALL_12(ret, func, stack, a1, a2, a3, a4, a5, a6, a7, a8, a9, aa, ab )   ASM_CALL_13(ret, func, stack, a1, a2, a3, a4, a5, a6, a7, a8, a9, aa, ab, RETURN_VOID)
#define ASM_CALL_11(ret, func, stack, a1, a2, a3, a4, a5, a6, a7, a8, a9, aa )       ASM_CALL_12(ret, func, stack, a1, a2, a3, a4, a5, a6, a7, a8, a9, aa, RETURN_VOID)
#define ASM_CALL_10(ret, func, stack, a1, a2, a3, a4, a5, a6, a7, a8, a9 )           ASM_CALL_11(ret, func, stack, a1, a2, a3, a4, a5, a6, a7, a8, a9, RETURN_VOID)
#define ASM_CALL_9(ret, func, stack, a1, a2, a3, a4, a5, a6, a7, a8 )                ASM_CALL_10(ret, func, stack, a1, a2, a3, a4, a5, a6, a7, a8, RETURN_VOID)
#define ASM_CALL_8(ret, func, stack, a1, a2, a3, a4, a5, a6, a7 )                    ASM_CALL_9(ret, func, stack, a1, a2, a3, a4, a5, a6, a7, RETURN_VOID)
#define ASM_CALL_7(ret, func, stack, a1, a2, a3, a4, a5, a6 )                        ASM_CALL_8(ret, func, stack, a1, a2, a3, a4, a5, a6, RETURN_VOID)
#define ASM_CALL_6(ret, func, stack, a1, a2, a3, a4, a5 )                            ASM_CALL_7(ret, func, stack, a1, a2, a3, a4, a5, RETURN_VOID)
#define ASM_CALL_5(ret, func, stack, a1, a2, a3, a4 )                                ASM_CALL_6(ret, func, stack, a1, a2, a3, a4, RETURN_VOID)
#define ASM_CALL_4(ret, func, stack, a1, a2, a3 )                                    ASM_CALL_5(ret, func, stack, a1, a2, a3, RETURN_VOID)
#define ASM_CALL_3(ret, func, stack, a1, a2 )                                        ASM_CALL_4(ret, func, stack, a1, a2, RETURN_VOID)
#define ASM_CALL_2(ret, func, stack, a1 )                                            ASM_CALL_3(ret, func, stack, a1, RETURN_VOID)
#define ASM_CALL_1(ret, func, stack )                                                ASM_CALL_2(ret, func, stack, RETURN_VOID)

#define GET_ASM_CALL_MACRO(ret, func, stack, _1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,NAME,...) NAME

/**
 * Call a function with a variable number of arguments.
 * 
 * Example:
 * - `ASM_CALL(RETURN_VOID, 0x00434460);` - calling void function with no arguments and void return value
 * - `ASM_CALL(RETURN(varName), 0x00434460);` - calling function with return value in `varName`
 * - `ASM_CALL(RETURN(varName), 0x00434460, 1, PUSH(var1));` - calling function with return value in `varName`, one argument pushed on the stack
 * - `ASM_CALL(RETURN(varName), 0x00434460, 1, EAX(var1), PUSH(var2));` - calling function with return value in `varName`, one argument in EAX and one pushed on the stack
 * 
 * Parameters:
 * - Param 1. `ret` – Return value - can be VOID or RETURN(variable)
 * - Param 2. `func` – Function address
 * - Param 3. `stack` – Number of arguments pushed on the stack
 * - Param 4. `...` – Arguments of the function - can be PUSH(variable) or EAX(variable), ECX(variable), etc.
 */
#define ASM_CALL(...) \
    { \
    GET_ASM_CALL_MACRO(__VA_ARGS__, \
        ASM_CALL_13, ASM_CALL_12, ASM_CALL_11, ASM_CALL_10, ASM_CALL_9, ASM_CALL_8, ASM_CALL_7, ASM_CALL_6, ASM_CALL_5, ASM_CALL_4, ASM_CALL_3, ASM_CALL_2, ASM_CALL_1, ASM_CALL_0)(__VA_ARGS__)\
    }

/**
 * Call a function with a variable number of arguments and return the value.
 * 
 * Example:
 * - `inline materialHandle_t* CG_RegisterMaterial(const char* name, materialType_e type) {`
 * - `   ASM_CALL_RETURN(materialHandle_t*, 0x00402160, 0, ECX(name), EAX(type))`
 * - `}`
 * 
 */
#define ASM_CALL_RETURN(type, ...) \
    { \
    type _variable_to_return; \
    ASM_CALL(RETURN(_variable_to_return), __VA_ARGS__) \
    return _variable_to_return; \
    }

        
// Make structure packed - remove paddding, ensure that the structure is aligned to 1 byte
#define PACKED __attribute__((__packed__))

// Make structure aligned - ensure that the structure is aligned to x bytes
#define ALIGNED(x) __attribute__((__aligned__(x)))