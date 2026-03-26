#ifndef COD2_SCRIPT_H
#define COD2_SCRIPT_H

#include "cod2_definitions.h"
#include "cod2_shared.h"
#include "cod2_entity.h"
#include <cstdint>

#define level_finished (*(int*)ADDR(0x0193dd70, 0x0864f970)) // 1=map_restart(), 2=map(), 3=exitLevel() (map_rotate or fast_restart)
#define level_savePersist (*(int*)ADDR(0x0193c4d4, 0x0864e0d4)) // set from GSC when calling exitLevel() or map_restart()

typedef struct scr_entref_s
{
	uint16_t entnum;
	uint16_t classnum;
} scr_entref_t;


typedef void (*xfunction_t)();
typedef void (*xmethod_t)(scr_entref_t);


typedef struct callback_s
{
	int *variable;
	const char* scriptName;
	const char* functionName;
	bool isNeeded;
} callback_t;


typedef struct scr_function_s
{
	const char      *name;
	xfunction_t     call;
	qboolean        developer;
} scr_function_t;

typedef struct scr_method_s
{
	const char     *name;
	xmethod_t      call;
	qboolean       developer;
} scr_method_t;



inline xmethod_t Scr_GetMethod(const char** v_methodName, qboolean* v_developer)
{
	return ((xmethod_t(*)(const char**, qboolean*))(ADDR(0x0050D310, 0x08117DEA)))(v_methodName, v_developer);
}

inline xfunction_t Scr_GetFunction(const char** v_functionName, qboolean* v_developer)
{
	return ((xfunction_t(*)(const char**, qboolean*))(ADDR(0x50D280, 0x08117CB2)))(v_functionName, v_developer);
}


inline void Scr_Error( const char *error )
{
	ASM_CALL(RETURN_VOID, ADDR(0x00483a80, 0x080853fc), 1, PUSH(error));
}


inline unsigned int Scr_GetNumParam()
{
	unsigned int ret;
	ASM_CALL(RETURN(ret), ADDR(0x00483520, 0x08085136));
	return ret;
}

// Executes a script immediately until wait command is called.
// Scr_FreeThread must be called to free the thread after it's done.
inline unsigned short Scr_ExecThread(int callbackHook, unsigned int numArgs) {
	unsigned short ret;
	ASM_CALL(RETURN_SHORT(ret), ADDR(0x00482080, 0x08083FD6), WL(1, 2), WL(EAX, PUSH)(callbackHook), PUSH(numArgs));
	return ret;
}

// Executes a script immediately for an entity, use classnum 0
// Scr_FreeThread must be called to free the thread after it's done.
inline unsigned short Scr_ExecEntThreadNum(int entnum, int classnum, int handle, unsigned int paramcount) {
	unsigned short ret;
	ASM_CALL(RETURN_SHORT(ret), ADDR(0x00482190, 0x08084062), WL(3, 4), PUSH(entnum), PUSH(classnum), WL(EAX, PUSH)(handle), PUSH(paramcount));
	return ret;
}

// Executes a script immediately until wait command is called.
// Return value can be read.
inline void Scr_AddExecThread(int callbackHook, unsigned int numArgs){
	ASM_CALL(RETURN_VOID, ADDR(0x004822a0, 0x080840f4), WL(1, 2), WL(EAX, PUSH)(callbackHook), PUSH(numArgs));
}

// Executes a script immediately for an entity, use classnum 0
// Return value can be read.
inline void Scr_AddExecEntThreadNum(int entnum, int classnum, int handle, unsigned int paramcount) {
	ASM_CALL(RETURN_VOID, ADDR(0x00482360, 0x0808415c), WL(3, 4), PUSH(entnum), PUSH(classnum), WL(EAX, PUSH)(handle), PUSH(paramcount));
}


inline void Scr_FreeThread(unsigned short thread_id)
{
	ASM_CALL(RETURN_VOID, ADDR(0x00479660, 0x080841D6), WL(0, 1), WL(EAX, PUSH)(thread_id));
}

inline int Scr_IsSystemActive()
{
	int ret;
	ASM_CALL(RETURN(ret), ADDR(0x00482b70, 0x08084678), 0);
	return ret;
}

inline int Scr_GetFunctionHandle(const char* scriptName, const char* labelName, int isNeeded)
{
	int ret;
	ASM_CALL(RETURN(ret), ADDR(0x00503f30, 0x08110208), WL(1, 3), WL(EDI, PUSH)(scriptName), WL(EBX, PUSH)(labelName), PUSH(isNeeded));
	return ret;
}



// Adds an undefined value to the stack, used when returning values back to GSC script
inline void Scr_AddUndefined(void)
{
	ASM_CALL(RETURN_VOID, ADDR(0x00483670, 0x080851D0), 0);
}
// Adds value of type boolean to the stack, used when returning values back to GSC script
inline void Scr_AddBool(qboolean value)
{
	ASM_CALL(RETURN_VOID, ADDR(0x00483530, 0x08085140), 1, PUSH(value));
}
// Adds value of type integer to the stack, used when returning values back to GSC script
inline void Scr_AddInt(int value)
{
	ASM_CALL(RETURN_VOID, ADDR(0x00483580, 0x08085164), 1, PUSH(value));
}
// Adds value of type float to the stack, used when returning values back to GSC script
inline void Scr_AddFloat(float value)
{
	ASM_CALL(RETURN_VOID, ADDR(0x004835d0, 0x08085188), 1, PUSH(value));
}
// Adds value of type string to the stack, used when returning values back to GSC script
inline void Scr_AddString(const char *string)
{
	ASM_CALL(RETURN_VOID, ADDR(0x00483770, 0x08085262), WL(0, 1), WL(ESI, PUSH)(string));
}
// Adds value of type localized string to the stack, used when returning values back to GSC script
inline void Scr_AddVector(float* vec)
{
	ASM_CALL(RETURN_VOID, ADDR(0x004838b0, 0x08085306), WL(0, 1), WL(ESI, PUSH)(vec));
}
// Creates array variable, must be called before Scr_AddArray
inline void Scr_MakeArray(void)
{
	ASM_CALL(RETURN_VOID, ADDR(0x00483930, 0x08085338), 0);
}
// Add array to stack, must be called after Scr_MakeArray
inline void Scr_AddArray(void)
{
	ASM_CALL(RETURN_VOID, ADDR(0x004839a0, 0x08085364), 0);
}


// Get value of passed parameter at index, e.g. testFunc(1)
inline int Scr_GetInt(unsigned int param)
{
	int ret;
	ASM_CALL(RETURN(ret), ADDR(0x00482b80, 0x08084694), WL(0, 1), WL(EAX, PUSH)(param));
	return ret;
}
// Get value of passed parameter at index, e.g. testFunc(1.0f)
inline float Scr_GetFloat(unsigned int param)
{
	float ret;
	ASM_CALL(RETURN_ST0_FLOAT(ret), ADDR(0x00482db0, 0x08084968), WL(0, 1), WL(EAX, PUSH)(param));
	return (float)ret;
}
// Get value of passed parameter at index, e.g. testFunc("test")
inline const char* Scr_GetString(unsigned int param)
{
	const char* ret;
	ASM_CALL(RETURN(ret), ADDR(0x00482ff0, 0x08084bb2), WL(0, 1), WL(EAX, PUSH)(param));
	return ret;
}
// Get value of passed parameter at index, e.g. testFunc(&"test")
inline const char* Scr_GetLocalizedString(unsigned int param)
{
	const char* ret;
	ASM_CALL(RETURN(ret), ADDR(0x00483140, 0x08084d24), WL(0, 1), WL(EAX, PUSH)(param));
	return ret;
}
// Get value of passed parameter at index, e.g. testFunc((0, 1, 2))
inline void Scr_GetVector(unsigned int param, vec3_t vec)
{
	ASM_CALL(RETURN_VOID, ADDR(0x00483160, 0x08084d40), WL(0, 2), WL(EAX, PUSH)(param), WL(EDX, PUSH)(vec));
}
// Get function handle of passed parameter at index, e.g. testFunc(::func)
inline void* Scr_GetParamFunction(int param)
{
	void* ret;
	ASM_CALL(RETURN(ret), ADDR(0x004831f0, 0x08084dd0), WL(0, 1), WL(EAX, PUSH)(param));
	return ret;
}
// Get type name of passed parameter at index, e.g "undefined", "int", "float", "string", ""localized string", "vector", "array", etc
inline char* Scr_GetTypeName(int param)
{
	char* ret;
	ASM_CALL(RETURN(ret), ADDR(0x00483440, 0x08085040), WL(0, 1), WL(EAX, PUSH)(param));
	return ret;
}


// Clears out all output parameters
inline void Scr_ClearOutParams()
{
	ASM_CALL(RETURN_VOID, ADDR(0x0047d630, 0x080800ec), 0);
}


inline char* SL_ConvertToString(int index)
{
	char* ret;
    ASM_CALL(RETURN(ret), ADDR(0x00477100, 0x08078ee6), WL(0, 1), WL(EAX, PUSH)(index));
	return ret;
}





#endif