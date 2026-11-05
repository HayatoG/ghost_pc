// Compiled twice: once against the frozen baseline (GSCpuBackend renamed to
// GSCpuBackendOld, GSMem to GSMemOld) and once against the working tree.
#include "runtime/gs/gs_cpu_backend.h"

#ifndef FACTORY_SUFFIX
#error FACTORY_SUFFIX
#endif
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)

GSRasterBackend *CAT(CreateBackend, FACTORY_SUFFIX)()
{
    return new GSCpuBackend();
}

bool CAT(RestoreBackend, FACTORY_SUFFIX)(GSRasterBackend *backend, std::FILE *file)
{
    return static_cast<GSCpuBackend *>(backend)->RestoreRecordedState(file);
}
