#ifndef _LTE_EG800K_H
#define _LTE_EG800K_H

#include "main.h"

#define AT_CMD_DEFAULT_TIMEOUT  5000


#define UART3_RX_BUF_SIZE 512
#define LTE_READ_CHUNK_SIZE 40960U


/************************* 1. 定义回调函数类型 *************************/
/**
 * @brief  回调函数原型：检测到目标响应后执行
 * @param  rx_buf：接收缓冲区（存储EC800的完整响应数据）
 * @param  user_data：用户自定义数据（可传递任意类型数据，如结构体、变量地址等）
 * @retval 无
 */
typedef int32_t (*_Resp_Callback)(void *);

extern volatile uint8_t RX_FLAG;
extern volatile uint32_t FILE_SIZE;
extern uint16_t uart3_rx_len;
extern uint8_t uart3_rx_buf[UART3_RX_BUF_SIZE];
extern uint8_t up_file_buf[LTE_READ_CHUNK_SIZE];
void eg800k_init(void);
extern uint8_t get_uart3_flag(void);
extern int32_t CheckResp(char *target_resp);
extern int32_t CheckResp_loop(char *target_resp, uint32_t delay_ms);
extern int32_t LTE_SendCmdWaitResp(char *cmd, char *target_resp, uint32_t timeout_ms);




#endif // _NETWORK_EG800K_H
