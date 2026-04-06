# LVGL布局系统调试检查计划

## 问题现象
- 所有12个UI子对象coords为(0,0)-(-1,-1)，宽高为0
- 只有屏幕背景和小边框被渲染

## 检查步骤

### Step 1: lv_obj_set_size()设置阶段
**目标**: 确认`lv_obj_set_size(width, 30)`正确设置了样式值

**代码路径**:
```
main.c: lv_obj_set_size(obj, 200, 30)
  -> lv_obj_set_width(obj, 200)
      -> lv_obj_set_style_width(obj, 200, selector)
          -> lv_obj_set_local_style_prop(obj, LV_STYLE_WIDTH, value, selector)
```

**检查点**:
1. `lv_obj_set_local_style_prop`中设置的value是否正确（200）
2. `lv_style_set_prop`是否正确将值存入style结构体
3. 对象style_cnt和styles[]数组是否正确更新

### Step 2: 布局更新触发阶段
**目标**: 确认`lv_obj_mark_layout_as_dirty()`被正确调用

**代码路径**:
```
lv_obj_set_size
  -> lv_obj_refresh_style(obj, selector, LV_STYLE_PROP_LAYOUT_REFR)
      -> lv_obj_mark_layout_as_dirty(obj)
```

**检查点**:
1. `obj->layout_inv`是否为1
2. 父对象的`layout_inv`是否也为1

### Step 3: lv_obj_refr_size()执行阶段
**目标**: 确认获取到的style_w是200而不是0或LV_SIZE_CONTENT

**代码路径**:
```
layout_update_core(obj)  // lvgl/src/core/lv_obj_pos.c:1124
  -> lv_obj_refr_size(obj)
      -> w = lv_obj_get_style_width(obj, LV_PART_MAIN)
```

**关键检查点**:
```c
w = lv_obj_get_style_width(obj, LV_PART_MAIN);
// 此时w应该等于200，而不是0或8197(LV_SIZE_CONTENT)
```

**问题排查**:
- 如果w=0: 说明`lv_obj_get_local_style_prop`没找到本地样式
- 如果w=8197: 说明获取到的是LV_SIZE_CONTENT，触发内容计算
- 如果w=200: 正常，继续检查后续计算

### Step 4: calc_content_width()计算阶段
**目标**: 如果w是LV_SIZE_CONTENT，确认内容宽度计算

**代码路径**:
```c
if(w_is_content) {
    w = calc_content_width(obj);
}
```

**calc_content_width检查点**:
```c
lv_coord_t self_w;
self_w = lv_obj_get_self_width(obj) + pad_left + pad_right;
// self_w来自lv_obj_get_self_width -> LV_EVENT_GET_SELF_SIZE
```

**关键**: 标签的`LV_EVENT_GET_SELF_SIZE`处理是否返回正确的文本宽度

### Step 5: coords应用阶段
**目标**: 确认coords被正确设置为(0,0)-(199,29)

**代码路径**:
```c
obj->coords.y2 = obj->coords.y1 + h - 1;
obj->coords.x2 = obj->coords.x1 + w - 1;
```

**检查点**:
- x1应该是之前align设置的父对象内相对坐标
- x2 = x1 + w - 1 = x1 + 200 - 1
- y2 = y1 + h - 1 = y1 + 30 - 1

### Step 6: lv_obj_refr_pos()位置更新
**目标**: 确认align设置的位置被正确应用

**代码路径**:
```c
lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, 10);
  -> lv_obj_refr_pos(obj)
```

## 调试输出格式

### 添加的调试printf:

**main.c中**:
```c
printf("[STEP1] lv_obj_set_size: obj=%p, w=%d, h=%d\n", obj, w, h);
```

**lv_obj_pos.c中**:
```c
printf("[STEP3] lv_obj_refr_size: obj=%p, style_w=%d, LV_SIZE_CONTENT=%d\n",
       obj, style_w, LV_SIZE_CONTENT);
printf("[STEP3] calc: w=%d, parent_w=%d\n", w, parent_w);
printf("[STEP5] coords: x1=%d, y1=%d, x2=%d, y2=%d, w=%d, h=%d\n",
       obj->coords.x1, obj->coords.y1, obj->coords.x2, obj->coords.y2, w, h);
```

## 预期结果（正常情况）

1. Step1: `lv_obj_set_size`设置200x30
2. Step3: `lv_obj_refr_size`获取style_w=200
3. Step5: coords变为(0,0)-(199,29)或类似正确坐标
4. 渲染: 对象可见

## 可能的问题原因

1. **样式值未正确保存**: PSRAM内存访问问题
2. **布局未触发**: `layout_inv`未被设置
3. **计算错误**: `calc_content_width`返回0
4. **坐标被覆盖**: 后续代码将coords重置
