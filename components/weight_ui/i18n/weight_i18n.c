#include "weight_i18n.h"
#include "lang_en.h"
#include "lang_ja.h"
#include "weight_config.h"

static const weight_lang_t *s_lang = &LANG_EN;
static int s_lang_index = 0;

static void apply_lang(int index)
{
    s_lang_index = index;
    s_lang = (index == 1) ? &LANG_JA : &LANG_EN;
}

void weight_i18n_init(int index)
{
    apply_lang(index); /* boot: no NVS write */
}

void weight_i18n_set_lang(int index)
{
    apply_lang(index); /* user toggle: persist */
    weight_config_set_lang(index);
    weight_config_save();
}

const weight_lang_t *weight_i18n_get(void)
{
    return s_lang;
}

int weight_i18n_get_lang_index(void)
{
    return s_lang_index;
}