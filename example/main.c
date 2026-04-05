/*Cube MX端口初始化函数：SDA、SCL开漏输出，RST、INT上拉推挽输出*/
#define TOUCH_SDA_Pin GPIO_PIN_8
#define TOUCH_SDA_GPIO_Port GPIOI
#define TOUCH_SCL_Pin GPIO_PIN_11
#define TOUCH_SCL_GPIO_Port GPIOI
#define TOUCH_RST_Pin GPIO_PIN_4
#define TOUCH_RST_GPIO_Port GPIOH
#define TOUCH_INT_Pin GPIO_PIN_3
#define TOUCH_INT_GPIO_Port GPIOG
 
void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
 
  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOI_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();
 
  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOI, TOUCH_SDA_Pin|TOUCH_SCL_Pin, GPIO_PIN_SET);
 
  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(TOUCH_RST_GPIO_Port, TOUCH_RST_Pin, GPIO_PIN_RESET);
 
  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(TOUCH_INT_GPIO_Port, TOUCH_INT_Pin, GPIO_PIN_RESET);
 
  /*Configure GPIO pins : PIPin PIPin */
  GPIO_InitStruct.Pin = TOUCH_SDA_Pin|TOUCH_SCL_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOI, &GPIO_InitStruct);
 
  /*Configure GPIO pin : PtPin */
  GPIO_InitStruct.Pin = TOUCH_RST_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(TOUCH_RST_GPIO_Port, &GPIO_InitStruct);
 
  /*Configure GPIO pin : PtPin */
  GPIO_InitStruct.Pin = TOUCH_INT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(TOUCH_INT_GPIO_Port, &GPIO_InitStruct);
}