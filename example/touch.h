#ifndef __TOUCH_H__
#define __TOUCH_H__
 
#include "main.h"
#define IIC_SDA(x) if(x) HAL_GPIO_WritePin(TOUCH_SDA_GPIO_Port,TOUCH_SDA_Pin,GPIO_PIN_SET);\
                    else HAL_GPIO_WritePin(TOUCH_SDA_GPIO_Port,TOUCH_SDA_Pin,GPIO_PIN_RESET)
#define IIC_SCL(x) if(x) HAL_GPIO_WritePin(TOUCH_SCL_GPIO_Port,TOUCH_SCL_Pin,GPIO_PIN_SET);\
                    else HAL_GPIO_WritePin(TOUCH_SCL_GPIO_Port,TOUCH_SCL_Pin,GPIO_PIN_RESET)
#define TOUCH_RST(x) if(x) HAL_GPIO_WritePin(TOUCH_RST_GPIO_Port,TOUCH_RST_Pin,GPIO_PIN_SET);\
                    else HAL_GPIO_WritePin(TOUCH_RST_GPIO_Port,TOUCH_RST_Pin,GPIO_PIN_RESET)
 
void Touch_IIC_Delay(uint32_t a);
void TOUCH_INT_SetOut(void);
void TOUCH_INT_SetIn(void);
 
/* IIC读写命令 */
#define FT5XXX_CMD_WR               0X70        /* 写命令(最低位为0) */
#define FT5XXX_CMD_RD               0X71        /* 读命令(最低位为1) */
 
/* FT5XXX 部分寄存器定义  */
#define FT5XXX_DEVIDE_MODE          0x00        /* FT5206模式控制寄存器 */
#define FT5XXX_REG_NUM_FINGER       0x02        /* 触摸状态寄存器 */
#define FT5XXX_TP1_REG              0X03        /* 第一个触摸点数据地址 */
#define FT5XXX_TP2_REG              0X09        /* 第二个触摸点数据地址 */
#define FT5XXX_TP3_REG              0X0F        /* 第三个触摸点数据地址 */
#define FT5XXX_TP4_REG              0X15        /* 第四个触摸点数据地址 */
#define FT5XXX_TP5_REG              0X1B        /* 第五个触摸点数据地址 */ 
 
#define	FT5XXX_ID_G_LIB_VERSION     0xA1        /* 版本 */
#define FT5XXX_ID_G_MODE            0xA4        /* FT5XXX中断模式控制寄存器 */
#define FT5XXX_ID_G_THGROUP         0x80        /* 触摸有效值设置寄存器 */
#define FT5XXX_ID_G_PERIODACTIVE    0x88        /* 激活状态周期设置寄存器 */
 
void FT5XXX_Init(void);/*FT5XX6初始化*/
void FT5XXX_Scan(void);
 
#endif