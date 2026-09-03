#ifndef CFUTURE_POLLING_H
#define CFUTURE_POLLING_H

#include "cfuture.h"

#ifdef __cplusplus
extern "C" {
#endif

const cfuture_sync_ops_t *cfuture_polling_sync_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* CFUTURE_POLLING_H */
