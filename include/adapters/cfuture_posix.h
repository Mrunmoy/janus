#ifndef CFUTURE_POSIX_H
#define CFUTURE_POSIX_H

#include "cfuture.h"

#ifdef __cplusplus
extern "C" {
#endif

const cfuture_sync_ops_t *cfuture_posix_sync_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* CFUTURE_POSIX_H */
