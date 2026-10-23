/* The guest side of the forwarding (nvcuda.dll exports these; nvEncodeAPI64.dll uses them) */
#ifndef CF_TRANSPORT_H
#define CF_TRANSPORT_H

#include <limits.h>
#include "proto.h"

#ifdef CF_BUILDING_NVCUDA
#define CF_API __declspec(dllexport)
#else
#define CF_API __declspec(dllimport)
#endif

/* cf_generic's results besides the function's own */
#define CF_ERR_TRANSPORT INT_MIN       /* no server, or the connection broke */
#define CF_ERR_MISSING (INT_MIN + 1)   /* the host has no such function */

/* Calls a generically forwarded function (desc: proto.h); *id caches its id (0 at first) */
CF_API int cf_generic(int *id, const char *name, const char *desc, ...);

/* A custom call: resolves name once into *id, sends req, and returns the reply (valid until this thread's next call)
 * positioned after the result, which goes to *result. 0, or CF_ERR_TRANSPORT / CF_ERR_MISSING. */
CF_API int cf_custom(int *id, const char *name, const cf_buf *req, int *result, cf_rd *reply);

/* A desc with structure sizes in it, formatted once */
#define CF_DESC(...) ({ static char d_[96]; if (!d_[0]) snprintf(d_, sizeof(d_), __VA_ARGS__); (const char *)d_; })

#endif
