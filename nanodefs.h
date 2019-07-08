#define MSG_SIZE     128    /*!< Maximum size of messages */
#define POOL_SIZE     10    /*!< Maximum number of clients */
// #define TIMEOUT    60000    /*!< Socket reading timeout, milliseconds */
#define TIMEOUT       -1    /*!< Socket reading timeout, milliseconds */

#define T_AVAILABLE     1   /*!< Thread is available */
#define T_INITIALISING  2   /*!< Thread is initialising */
#define T_TIMEOUT       4   /*!< Thread has timed out */
#define T_WORKING       8   /*!< Thread is active */
#define T_FAILED       16   /*!< Thread failed initialisation */
#define T_REQEXIT      32   /*!< Thread has got a request to end */
