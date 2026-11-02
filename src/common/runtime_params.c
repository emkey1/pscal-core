#include <stdbool.h>
#include "core/globals.h"

PSCAL_THREAD_LOCAL int gParamCount = 0;
PSCAL_THREAD_LOCAL char **gParamValues = NULL;
PSCAL_THREAD_LOCAL bool gParamValuesOwned = false;
