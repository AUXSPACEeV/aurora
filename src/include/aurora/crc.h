/**
 * @file crc.h
 * @brief CRC Library
 *
 * Author: Maximilian Stephan @ Auxspace e.V.
 * Copyright (C) 2025 Auxspace e.V.
 * Derived from:
Copyright 2021 Carl John Kugler III

Licensed under the Apache License, Version 2.0 (the License); you may not use 
this file except in compliance with the License. You may obtain a copy of the 
License at

http://www.apache.org/licenses/LICENSE-2.0 
Unless required by applicable law or agreed to in writing, software distributed 
under the License is distributed on an AS IS BASIS, WITHOUT WARRANTIES OR 
CONDITIONS OF ANY KIND, either express or implied. See the License for the 
specific language governing permissions and limitations under the License.
*/
/* Derived from:
 * SD/MMC File System Library
 * Copyright (c) 2016 Neil Thiessen
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stddef.h>

/**
 * @brief Calculate the CRC7 checksum for the specified data block
 *
 * @param data Pointer to the data block
 * @param length Length of the data block
 *
 * @return The calculated CRC7 checksum
 */
char crc7(const char* data, int length);

/**
 * @brief Calculate the CRC16 checksum for the specified data block
 *
 * @param data Pointer to the data block
 * @param length Length of the data block
 *
 * @return The calculated CRC16 checksum
 */
unsigned short crc16(const char* data, int length);

/**
 * @brief Update the CRC16 checksum with the specified data block
 *
 * @param pCrc16 Pointer to the current CRC16 checksum
 * @param data Pointer to the data block
 * @param length Length of the data block
 */
void update_crc16(unsigned short *pCrc16, const char data[], size_t length);

/* [] END OF FILE */