#pragma once

#if defined(MAPPER_5)
#include "mapper5.h"
#elif defined(MAPPER_4)
#include "mapper4.h"
#else
#error "Mapper not found"
#endif
