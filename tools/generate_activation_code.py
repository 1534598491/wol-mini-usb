#!/usr/bin/env python3
"""
WOL-Mini-USB 激活码生成器
用于为ESP32设备生成唯一激活码

使用方法：
  python generate_activation_code.py <MAC地址>

示例：
  python generate_activation_code.py 30:ed:a0:27:15:0c
  输出：A1B2-C3D4-E5F6
"""

import hashlib
import sys
import re

# 密钥（与固件中保持一致，不要泄露！）
SECRET_KEY = "WOL-MINI-USB-2024-SECRET"

def generate_activation_code(mac_address: str) -> str:
    """
    根据MAC地址生成激活码

    算法：
    1. 移除MAC地址中的分隔符
    2. 与密钥组合
    3. HMAC-SHA256哈希运算
    4. 取前12位
    5. 格式化为 XXXX-XXXX-XXXX
    """
    # 移除分隔符，统一为大写
    mac_clean = mac_address.replace(":", "").replace("-", "").upper()

    # 验证MAC地址格式
    if len(mac_clean) != 12:
        raise ValueError(f"MAC地址格式错误: {mac_address}")

    if not re.match(r'^[0-9A-F]{12}$', mac_clean):
        raise ValueError(f"MAC地址必须是12位十六进制: {mac_address}")

    # 组合MAC和密钥
    data = mac_clean + SECRET_KEY

    # HMAC-SHA256哈希
    hash_result = hashlib.sha256(data.encode()).hexdigest()

    # 取前12位作为激活码
    activation_raw = hash_result[:12].upper()

    # 格式化：XXXX-XXXX-XXXX
    activation_code = f"{activation_raw[:4]}-{activation_raw[4:8]}-{activation_raw[8:12]}"

    return activation_code

def main():
    print("=" * 50)
    print("WOL-Mini-USB 激活码生成器")
    print("=" * 50)

    if len(sys.argv) < 2:
        print("\n使用方法: python generate_activation_code.py <MAC地址>")
        print("\n示例:")
        print("  python generate_activation_code.py 30:ed:a0:27:15:0c")
        print("  python generate_activation_code.py 30-ED-A0-27-15-0C")
        return

    mac_address = sys.argv[1]

    try:
        activation_code = generate_activation_code(mac_address)
        print(f"\nMAC地址:  {mac_address}")
        print(f"激活码:   {activation_code}")
        print("\n请将激活码提供给用户，用于设备激活")
        print("=" * 50)
    except ValueError as e:
        print(f"\n错误: {e}")
        return

if __name__ == "__main__":
    main()