/* config.h
** Default values for the configuration, and also a few random debug things
*/

#ifndef AC_CONFIG_H
#define AC_CONFIG_H

/*** Version information ***/
#define ACVERSION "2.0"

/*** Default configuration stuff ***/
#define TARGET 32767		/*!< Target level (on a scale of 0-32767) */

#define GAINMAX 64		/*!< The maximum amount to amplify by */
#define GAINSMOOTH 8		/*!< How much inertia ramping has*/
#define BUCKETS 20		/*!< How long of a history to use by default */

#endif

