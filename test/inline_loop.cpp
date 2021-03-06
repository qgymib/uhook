#include "common.hpp"

typedef int(*fn_sig)(int, int);

static int sum(int a, int b)
{
    int i;
    int ret = 0;
    for (i = a; i <= b; i++)
    {
        ret += i;
    }
    return ret;
}

static int del(int a, int b)
{
    int i;
    int ret = 0;
    for (i = a; i <= b; i++)
    {
        ret -= i;
    }
    return ret;
}

DISABLE_OPTIMIZE
TEST(inline_hook, loop)
{
    ASSERT_EQ_D32(sum(1, 10), 55);

    uhook_token_t token;
    ASSERT_EQ_D32(uhook_inject(&token, (void*)sum, (void*)del), 0);
    ASSERT_NE_PTR(token.fcall, NULL);

    ASSERT_EQ_D32(sum(1, 10), -55);
    ASSERT_EQ_D32(((fn_sig)token.fcall)(1, 10), 55);

    uhook_uninject(&token);
    ASSERT_EQ_D32(sum(1, 10), 55);
}
