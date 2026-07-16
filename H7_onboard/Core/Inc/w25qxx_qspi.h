#ifndef W25QXX_QSPI_H
#define W25QXX_QSPI_H

#include "stdint.h"

#define w25qxx_DTRMode     0
#define w25qxx_STRMode     1

void w25qxx_Init(void);
void w25qxx_EnterQPI(void);
void w25qxx_Startup(uint8_t mode);

#endif
