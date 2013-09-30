#ifndef WESTON_COMPOSITOR_ICE_H_
# define WESTON_COMPOSITOR_ICE_H_

#ifdef  __cplusplus
extern "C" {
#endif

#include "compositor.h"

#define WESTON_ICE_BACKEND_CONFIG_VERSION 1

struct weston_ice_backend_config {
	struct weston_backend_config base;
	bool use_pixman;
};

#ifdef  __cplusplus
}
#endif

#endif /* !WESTON_COMPOSITOR_ICE_H_ */
