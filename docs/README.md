# WOL-Mini-USB

即插即用远程唤醒/睡眠方案，完全零依赖。

## 特点

- **即插即用**: ESP32插入PC USB口即可使用
- **零软件安装**: 不需要PC客户端软件
- **零服务器**: 使用公共MQTT broker
- **零维护成本**: 部署后无需维护

## 架构

```
Web控制页面 → 公共MQTT → ESP32 → USB HID → PC唤醒/睡眠
```

## 文件结构

```
wol-mini-usb/
├── firmware/
│   ├── src/main.cpp      # ESP32固件
│   └── platformio.ini    # 编译配置
├── web/
│   └── index.html        # Web控制页面
└── docs/
    └── README.md         # 说明文档
```

## 使用方法

### 1. 烧录固件

```bash
cd firmware
pio run -t upload
```

### 2. 配网

1. 连接WiFi热点 `WOL-Mini-USB`
2. 浏览器自动跳转配网页面
3. 扫描WiFi并选择
4. 设置设备名称和Token
5. 保存配置

### 3. 插入USB

将ESP32插入PC USB口。

### 4. Web控制

打开 `index.html`，输入设备ID和Token，点击唤醒/睡眠。

## 工作原理

| 功能 | 方式 |
|------|------|
| 唤醒 | USB HID System Standby命令 |
| 睡眠 | USB HID System Standby命令 |

**Sleep键双向工作**：
- PC睡眠时 → Sleep键唤醒
- PC运行时 → Sleep键触发睡眠

## 安全

- Token本地验证（ESP32）
- 公共MQTT消息无加密（建议复杂Token）
- Sleep键无外挂价值（反作弊风险低）

## 限制

- 只能唤醒S3睡眠状态
- 不能唤醒关机/休眠
- ESP32需一直插在USB口