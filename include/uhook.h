#ifndef __UHOOK_H__
#define __UHOOK_H__
#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#   if defined(uhook_EXPORTS) || defined(UHOOK_EXPORTS)
#       define UHOOK_API __declspec(dllexport)
#   else
#       define UHOOK_API __declspec(dllimport)
#   endif
#else
#   define UHOOK_API
#endif

enum uhook_errno
{
    UHOOK_SUCCESS       = 0,    /**< Success */
    UHOOK_UNKNOWN       = -1,   /**< Unknown error */
    UHOOK_NOMEM         = -2,   /**< Not enough space/cannot allocate memory */
    UHOOK_SMALLFUNC     = -3,   /**< Function is too small to inject inline hook opcode */
    UHOOK_NOFUNCSIZE    = -4,   /**< Can not get function size, may be stripped? */
    UHOOK_GOTNOTFOUND   = -5,   /**< Function not found in GOT/PLT */
};

typedef struct uhook_token
{
    void*           fcall;      /**< Original function, cast to original protocol to call it. */
    unsigned long   attrs;      /**< Attributes */
    void*           token;      /**< Inject token */
}uhook_token_t;

/**
 * @brief Inject function
 * @param[out] origin       Inject Context, also can be called as original function.
 *                          If failure, this value is set to NULL.
 * @param[in] target        The function to be inject
 * @param[in] detour        The function to replace original function
 * @return                  Inject result
 */
UHOOK_API int uhook_inject(uhook_token_t* token, void* target, void* detour);

/**
 * @brief Inject GOT/PLT
 * @param[out] token        Inject context
 * @param[in] name          Function name. If '@' followed, inject specify library.
 * @param[in] detour        The function to replace original function
 * @return                  Inject result
 */
UHOOK_API int uhook_inject_got(uhook_token_t* token, const char* name, void* detour);

/**
 * @brief Uninject function
 * @param[in,out] origin    The context to be uninject. This value will be set to NULL.
 */
UHOOK_API void uhook_uninject(uhook_token_t* token);

#ifdef __cplusplus
}
#endif
#endif
