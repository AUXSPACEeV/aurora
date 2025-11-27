/*
 * Copyright (c) 2025 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_STORAGE_H_
#define APP_LIB_STORAGE_H_

/**
* @defgroup lib_storage Storage library
* @ingroup lib
* @{
*
* @brief AURORA Storage library for avionics telemetry.
*
* This library contains data storage functions.
*/

/*
*  Note the fatfs library is able to mount only strings inside _VOLUME_STRS
*  in ffconf.h
*/

#define MAX_PATH 256

/**
* @brief Initialize the storage for the AURORA application.
*
* Function initializes the Kconfig controlled storage type and
* returns 0 on success.
*
* @retval zero on success, negative error code on failure.
*/
int storage_init();

/** @} */

#endif /* APP_LIB_STORAGE_H_ */