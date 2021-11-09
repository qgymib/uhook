#ifndef __INLINE_HOOK_H__
#define __INLINE_HOOK_H__
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inject function
 * @param[out] origin       Inject Context, also can be called as original function.
 *                          If failure, this value is set to NULL.
 * @param[in] target        The function to be inject
 * @param[in] detour        The function to replace original function
 * @return                  Inject result
 */
int inline_hook_inject(void** origin, void* target, void* detour);

/**
 * @brief Uninject function
 * @param[in,out] origin    The context to be uninject. This value will be set to NULL.
 */
void inline_hook_uninject(void** origin);

/**
 * @brief Dump information into buffer
 * @param[out] buffer		Buffer to store information
 * @param[in] size			Buffer size
 * @param[in] origin		Inject context
 * @return					The number of characters printed should have written.
 */
int inline_hook_dump(char* buffer, unsigned size, const void* origin);

#ifdef __cplusplus
}
#endif
#endif
