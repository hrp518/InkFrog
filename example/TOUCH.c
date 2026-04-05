#include "TOUCH.h"
#include "usart.h"
#include "ltdc.h"
#include "string.h"
 
void Touch_IIC_Delay(uint32_t a)
{
	volatile uint16_t i;
	while (a --)				
	{
		for (i = 0; i < 8; i++);
	}
}
 
void TOUCH_INT_SetOut(void)
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};
	GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;// 输出模式
	GPIO_InitStruct.Pull  = GPIO_PULLUP;        // 上拉	
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;// 速度等级
	GPIO_InitStruct.Pin   = TOUCH_INT_Pin ;     // 初始化 INT 引脚
	HAL_GPIO_Init(TOUCH_INT_GPIO_Port, &GPIO_InitStruct);		
}
 
void TOUCH_INT_SetIn(void)
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};
	GPIO_InitStruct.Mode  = GPIO_MODE_INPUT;    // 输入模式
	GPIO_InitStruct.Pull  = GPIO_NOPULL;        // 浮空	
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;// 速度等级
	GPIO_InitStruct.Pin   = TOUCH_INT_Pin ;     // 初始化 INT 引脚
	HAL_GPIO_Init(TOUCH_INT_GPIO_Port, &GPIO_InitStruct);		
 
}
 
void IIC_Start(void)//IIC开始
{
    IIC_SDA(1);Touch_IIC_Delay(20);
	IIC_SCL(1);Touch_IIC_Delay(20);
	IIC_SDA(0);Touch_IIC_Delay(20);
	IIC_SCL(0);Touch_IIC_Delay(20);
}
 
void IIC_Stop(void)//IIC停止
{
    IIC_SCL(0);Touch_IIC_Delay(20);
	IIC_SDA(0);Touch_IIC_Delay(20);
	IIC_SCL(1);Touch_IIC_Delay(20);
	IIC_SDA(1);Touch_IIC_Delay(20);
}
 
void IIC_SendAck(uint8_t Ack)//IIC发送应答0为应答1为非应答
{
    IIC_SDA(Ack);Touch_IIC_Delay(20);
	IIC_SCL(1);Touch_IIC_Delay(20);
	IIC_SCL(0);Touch_IIC_Delay(20);
	IIC_SDA(0);Touch_IIC_Delay(20);
}
 
uint8_t IIC_ReceiveAck(void)//IIC接收应答
{
	uint8_t Ack;
	IIC_SDA(1);//接收时先释放IIC_SDA置1
    Touch_IIC_Delay(20);
    IIC_SCL(1);Touch_IIC_Delay(20);
	IIC_SDA(Ack);Touch_IIC_Delay(20);
	IIC_SCL(0);Touch_IIC_Delay(20);
	IIC_SDA(0);Touch_IIC_Delay(20);
	return Ack;
}
 
void IIC_SendByte(uint8_t Byte)//IIC发送一个字节，先发高位
{
    uint8_t i;
	for(i=0;i<8;i++)
    {
        IIC_SDA(Byte&(0x80>>i));Touch_IIC_Delay(20);
		IIC_SCL(1);Touch_IIC_Delay(20);
		IIC_SCL(0);Touch_IIC_Delay(20);
    }
}
 
uint8_t IIC_ReceiveByte(void)//IIC接收一个字节
{
	uint8_t Byte=0,i;
	IIC_SDA(1);//接收时先释放IIC_SDA置1
    Touch_IIC_Delay(20);
	for(i=0;i<8;i++)
    {
		IIC_SCL(1);Touch_IIC_Delay(20);
		if(HAL_GPIO_ReadPin(TOUCH_SDA_GPIO_Port,TOUCH_SDA_Pin))
			Byte|=(0x80>>i);
        Touch_IIC_Delay(20);
		IIC_SCL(0);Touch_IIC_Delay(20);
    }
	return Byte;
}
 
uint8_t FT5XXX_WR_Reg(uint8_t reg,uint8_t *buf,uint8_t len)
{
    uint8_t ack=0;
	IIC_Start();
    IIC_SendByte(FT5XXX_CMD_WR);
    IIC_ReceiveAck();
    IIC_SendByte(reg);
    IIC_ReceiveAck();
    
    for(uint8_t i = 0; i < len; i++)
    {
        IIC_SendByte(buf[i]);
        ack = IIC_ReceiveAck();
        if(ack)break;
    }
    IIC_Stop();
    return ack;
}
 
void FT5XXX_Reg(uint8_t reg,uint8_t *buf,uint8_t len)
{
	IIC_Start();
    IIC_SendByte(FT5XXX_CMD_WR);
    IIC_ReceiveAck();
    IIC_SendByte(reg);
    IIC_ReceiveAck();
    
    IIC_Start();
    IIC_SendByte(FT5XXX_CMD_RD);
    IIC_ReceiveAck();
    for(uint8_t i = 0; i < len; i++)
    {
        buf[i] = IIC_ReceiveByte();
        if(i == len-1)/*1:ack 0:Nack*/
            IIC_SendAck(1);
        else
            IIC_SendAck(0);
    }
    IIC_Stop();
}
 
void FT5XXX_Init(void)
{
    uint8_t temp[2];
    /*初始化INT,RST低电平*/
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    HAL_GPIO_WritePin(TOUCH_INT_GPIO_Port,TOUCH_INT_Pin,GPIO_PIN_SET);//拉高INT
    TOUCH_RST(0);//复位
    HAL_Delay(20);
    TOUCH_RST(1);//释放复位
    HAL_Delay(50);
    
    /*配置完成后将INT设置为浮空输入模式*/
    GPIO_InitStruct.Pin = TOUCH_INT_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(TOUCH_INT_GPIO_Port,&GPIO_InitStruct);
    
    temp[0] = 0;
    FT5XXX_WR_Reg(FT5XXX_DEVIDE_MODE, temp, 1);     /* 进入正常操作模式 */
    temp[0] = 1;
    FT5XXX_WR_Reg(FT5XXX_ID_G_MODE, temp, 1);       /* 查询模式 0启用对主机中断,1禁止对主机中断*/
    temp[0] = 22;                                   /* 触摸有效值，22，越小越灵敏 */
    FT5XXX_WR_Reg(FT5XXX_ID_G_THGROUP, temp, 1);    /* 设置触摸有效值 */
    temp[0] = 12;                                   /* 激活周期，不能小于12，最大14 */
    FT5XXX_WR_Reg(FT5XXX_ID_G_PERIODACTIVE, temp, 1);
    
    FT5XXX_Reg(FT5XXX_ID_G_LIB_VERSION, &temp[0], 2);
    if ((temp[0] == 0X30 && temp[1] == 0X03) || temp[1] == 0X01 || temp[1] == 0X02 || (temp[0] == 0x0 && temp[1] == 0X0))   /* 版本:0X3003/0X0001/0X0002/CST340 */
    {
        printf("CTP ID:%x\r\n", ((uint16_t)temp[0] << 8) + temp[1]);
    }
}
 
void FT5XXX_Scan(void)
{
    uint8_t mode = 0;
    uint8_t temp[4];
    uint16_t x,y;
    
    /*读取状态寄存器*/
    FT5XXX_Reg(FT5XXX_REG_NUM_FINGER,&mode,1);
    
    if ((mode & 0xF) && ((mode & 0xF) <= 10))//有触摸并且小于10个点
    {
        FT5XXX_Reg(FT5XXX_TP1_REG,temp,4);
        x = ((uint16_t)(temp[0] & 0X0F) << 8) + temp[1];
        y = ((uint16_t)(temp[2] & 0X0F) << 8) + temp[3];
        printf("x:%d y:%d\r\n",x,y);
    }	
}