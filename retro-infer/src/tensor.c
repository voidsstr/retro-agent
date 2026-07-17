#include "infer.h"

size_t dtype_size(dtype_t dt)
{
    switch (dt) {
    case DT_F32: return 4;
    case DT_I8:  return 1;
    case DT_U8:  return 1;
    case DT_I32: return 4;
    case DT_BIN: return 1;   /* per 8 packed weights; see tensor_bytes */
    default:     return 0;
    }
}

size_t tensor_elems(const tensor_t *t)
{
    size_t n = 1;
    int i;
    for (i = 0; i < t->ndim; i++)
        n *= (size_t)t->shape[i];
    return n;
}

size_t tensor_bytes(const tensor_t *t)
{
    size_t n = tensor_elems(t);
    if (t->dtype == DT_BIN)
        return (n + 7) / 8;
    return n * dtype_size(t->dtype);
}
