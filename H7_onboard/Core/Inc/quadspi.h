#ifndef QUADSPI_H
#define QUADSPI_H

#include "stdint.h"

typedef struct {
    uint32_t Instance;
    uint32_t Init;
} QSPI_HandleTypeDef;

#define QSPI_OK       0
#define QSPI_ERROR    1
#define QSPI_BUSY     2
#define QSPI_TIMEOUT  3

void MX_QUADSPI_Init(void);

#endif
