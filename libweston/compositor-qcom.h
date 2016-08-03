#ifndef WESTON_COMPOSITOR_QCOM_H_
# define WESTON_COMPOSITOR_QCOM_H_

#ifdef  __cplusplus
extern "C" {
#endif

#include "compositor.h"

#define WESTON_QCOM_BACKEND_CONFIG_VERSION 1

struct weston_qcom_backend_config {
	struct weston_backend_config base;
	char *device;
	uint32_t output_transform;
};

#ifdef  __cplusplus
}
#endif

#endif /* !WESTON_COMPOSITOR_QCOM_H_ */
