#ifndef UI_SIDEBAR_H
#define UI_SIDEBAR_H

void widget_sidebar_render(struct nk_context *ctx);
bool widget_sidebar_is_visible();
void widget_sidebar_toggle();
void widget_sidebar_set_visible(bool visible);
float widget_sidebar_get_width();

#endif
